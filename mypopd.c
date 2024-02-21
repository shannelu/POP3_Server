#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

typedef enum state {
    Undefined,
    // TODO: Add additional states as necessary
    UserExist,
    TRANSACTION,
    UPDATE,
} State;

typedef struct serverstate {
    int fd;
    net_buffer_t nb;
    char recvbuf[MAX_LINE_LENGTH + 1];
    char *words[MAX_LINE_LENGTH];
    int nwords;
    State state;
    struct utsname my_uname;
    // TODO: Add additional fields as necessary
    char *username;
    mail_list_t mailList;
    int mailLength;
    size_t mailSize;
    int mailLengthIncludeDeleted;
} serverstate;
static void handle_client(int fd);

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
        return 1;
    }
    run_server(argv[1], handle_client);
    return 0;
}

// syntax_error returns
//   -1 if the server should exit
//    1 otherwise
int syntax_error(serverstate *ss) {
    if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
    return 1;
}

// checkstate returns
//   -1 if the server should exit
//    0 if the server is in the appropriate state
//    1 if the server is not in the appropriate state
int checkstate(serverstate *ss, State s) {
    if (ss->state != s) {
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Bad sequence of commands") <= 0) return -1;
        return 1;
    }
    return 0;
}

// All the functions that implement a single command return
//   -1 if the server should exit
//    0 if the command was successful
//    1 if the command was unsuccessful

int do_quit(serverstate *ss) {
    dlog("Executing quit\n");
    // TODO: Implement this function
    if(ss->state == TRANSACTION){
        ss->state = UPDATE;
        int errorNum = mail_list_destroy(ss->mailList);
        if(errorNum > 0){
            if(send_formatted(ss->fd, "-ERR %s\r\n", "some deleted messages not removed") <= 0) return -1;
            return -1;
        }
        if(send_formatted(ss->fd, "+OK %s\r\n", "Service closing transmission channel") <= 0) return -1;
    } else {   //the POP3 session is in AUTHORIZATION state
        if (send_formatted(ss->fd, "+OK %s\r\n", "Service closing transmission channel") <= 0) return -1;
    }
    return -1;
}

int do_user(serverstate *ss) {
    dlog("Executing user\n");
    // TODO: Implement this function
    if(checkstate(ss, Undefined) != 0){
        return 1;
    }
    if(ss->nwords != 2){
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
        return 1;
    }
    if(!is_valid_user(ss->words[1],NULL)){
        if(send_formatted(ss->fd, "-ERR %s \r\n", "never heard of mailbox name") <= 0) return -1;
        return 1;
    } else{
        if (send_formatted(ss->fd, "+OK name is a valid mailbox \r\n") <= 0) return -1;
        ss->state = UserExist;
        ss->username = strdup(ss->words[1]);
        return 0;
    }
}

int do_pass(serverstate *ss) {
    dlog("Executing pass\n");
    // TODO: Implement this function
    char *password = ss->words[1];
    if(ss->state != UserExist){
        if(send_formatted(ss->fd,"-ERR %s \r\n", "Bad sequence of commands") <= 0) return -1;
        return 1;
    }
    if(ss->nwords != 2){
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
        return 1;
    }

    if(!is_valid_user(ss->username, password)){
        if(send_formatted(ss->fd, "-ERR %s \r\n", "Password is invalid") <= 0) return -1;
        ss->state = Undefined;
        return 1;
    }

    if(send_formatted(ss->fd, "+OK %s \r\n", "Password is valid, mail loaded") <= 0) return -1;
    ss->state = TRANSACTION;
    ss->mailList = load_user_mail(ss->username);
    ss->mailLength = mail_list_length(ss->mailList, 0); // deleted messages are not included in the count
    ss->mailSize = mail_list_size(ss->mailList);
    ss->mailLengthIncludeDeleted = mail_list_length(ss->mailList,1);
    return 0;
}

int do_stat(serverstate *ss) {
    dlog("Executing stat\n");
    // TODO: Implement this function
    if(ss->state != TRANSACTION){
        if(send_formatted(ss->fd, "-ERR %s \r\n", "Bad sequence of commands") <= 0) return -1;
        return 1;
    }
    if(send_formatted(ss->fd, "+OK %d %ld \r\n", ss->mailLength, ss->mailSize) <= 0) return -1;
    return 0;
}

int do_list(serverstate *ss) {
    dlog("Executing list\n");
    // TODO: Implement this function
    if(checkstate(ss, TRANSACTION)) return 1;
    if(ss->nwords != 2 && ss->nwords != 1){
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
        return 1;
    }

    if(ss->nwords == 1) {
        if (send_formatted(ss->fd, "+OK %d messages (%ld octets)\r\n", ss->mailLength, ss->mailSize) <= 0) return -1;
        for(int i=0; i < ss->mailLengthIncludeDeleted; i++){
            mail_item_t mailItem = mail_list_retrieve(ss->mailList, i);
            // if the message in this position is marked as deleted, find the next position not marked as deleted
            if(mailItem != NULL){
                if(send_formatted(ss->fd, "%d %ld\r\n", i+1, mail_item_size(mailItem)) <= 0) return -1;
            }
        }
        if (send_formatted(ss->fd, ".\r\n") <= 0) return -1;
        return 0;
    }

    // there is one argument
    char *scanListing = ss->words[1];
    // Check whether the argument is a integer
    if(!isdigit(*scanListing)){
        if(send_formatted(ss->fd, "-ERR %s\r\n", "Invalid arguments") <= 0) return -1;
        return 1;
    }
    int index = atoi(scanListing) - 1; //convert char pointer to 0-based integer
    if(index < 0) {
        if(send_formatted(ss->fd, "-ERR %s\r\n", "Invalid arguments") <= 0) return -1;
        return 1;
    }
    if(index >= ss->mailLengthIncludeDeleted){
        if(send_formatted(ss->fd, "-ERR no such message, only %d messages in maildrop\r\n", ss->mailLength) <= 0) return -1;
        return 1;
    }
    mail_item_t mailItem = mail_list_retrieve(ss->mailList, index);
    if(mailItem == NULL){ // the message in this position is marked as deleted
        if(send_formatted(ss->fd, "-ERR message %d is marked as deleted\r\n", index+1) <= 0) return -1;
        return 1;
    }
    if(send_formatted(ss->fd, "+OK %d %ld\r\n", index + 1, mail_item_size(mailItem)) <= 0) return -1;
    return 0;
}

int do_retr(serverstate *ss) {
    dlog("Executing retr\n");
    // TODO: Implement this function
    if(checkstate(ss,TRANSACTION)) return 1;
    if(ss->nwords != 2){
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
        return 1;
    }

    char *scanListing = ss->words[1];
    // Check whether the argument is a positive integer
    if(!isdigit(*scanListing)){
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
        return 1;
    }
    int index = atoi(scanListing) - 1; //convert char pointer to integer
    if(index < 0) {
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
        return 1;
    }

    mail_item_t mailItem = mail_list_retrieve(ss->mailList, index);
    if(mailItem == NULL){
        if(send_formatted(ss->fd, "-ERR no such message\r\n") <= 0) return -1;
        return 1;
    }

    if(send_formatted(ss->fd, "+OK %ld octets \r\n", mail_item_size(mailItem)) <= 0) return -1;
    FILE *file = mail_item_contents(mailItem);
    char buffer[1024];
    while(fgets(buffer, sizeof(buffer), file)) {
        if(send_formatted(ss->fd, "%s", buffer) <= 0) return -1;
    }
    if(send_formatted(ss->fd, ".\r\n") <= 0) return -1;
    fclose(file);
    return 0;
}

int do_rset(serverstate *ss) {
    dlog("Executing rset\n");
    // TODO: Implement this function
    if(checkstate(ss,TRANSACTION)) return 1;
    if(ss->nwords != 1) {
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
        return 1;
    }
    int numRset = mail_list_undelete(ss->mailList);
    ss->mailLength = mail_list_length(ss->mailList,0);
    ss->mailSize = mail_list_size(ss->mailList);
    if(send_formatted(ss->fd, "+OK %d messages restored\r\n", numRset) <= 0) return -1;
    return 0;
}

int do_noop(serverstate *ss) {
    dlog("Executing noop\n");
    // TODO: Implement this function
    if(checkstate(ss,TRANSACTION) != 0) return 1;
    if (send_formatted(ss->fd, "+OK \r\n") <= 0) return -1;
    return 0;
}

int do_dele(serverstate *ss) {
    dlog("Executing dele\n");
    // TODO: Implement this function
    if(checkstate(ss,TRANSACTION)) return 1;
    if(ss->nwords != 2) {
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
        return 1;
    }

    char *scanListing = ss->words[1];
    // Check whether the argument is a positive integer
    if(!isdigit(*scanListing)){
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
        return 1;
    }
    int index = atoi(scanListing) - 1; //convert char pointer to integer
    if(index < 0 || index >= ss->mailLengthIncludeDeleted) {
        if(send_formatted(ss->fd, "-ERR %s\r\n", "Invalid arguments") <= 0) return -1;
        return 1;
    }

    mail_item_t mailItem = mail_list_retrieve(ss->mailList, index);
    if(mailItem == NULL){
        if(send_formatted(ss->fd, "-ERR message %d already deleted\r\n", index) <= 0) return -1;
        return 1;
    }

    mail_item_delete(mailItem);
    ss->mailLength = mail_list_length(ss->mailList,0);
    ss->mailSize = mail_list_size(ss->mailList);
    if(send_formatted(ss->fd, "+OK message %d deleted\r\n", index+1) <= 0) return -1;
    return 0;
}

void handle_client(int fd) {
    size_t len;
    serverstate mstate, *ss = &mstate;
    ss->fd = fd;
    ss->nb = nb_create(fd, MAX_LINE_LENGTH);
    ss->state = Undefined;
    uname(&ss->my_uname);
    if (send_formatted(fd, "+OK POP3 Server on %s ready\r\n", ss->my_uname.nodename) <= 0) return;

    while ((len = nb_read_line(ss->nb, ss->recvbuf)) >= 0) {
        if (ss->recvbuf[len - 1] != '\n') {
            // command line is too long, stop immediately
            send_formatted(fd, "-ERR Syntax error, command unrecognized\r\n");
            break;
        }
        if (strlen(ss->recvbuf) < len) {
            // received null byte somewhere in the string, stop immediately.
            send_formatted(fd, "-ERR Syntax error, command unrecognized\r\n");
            break;
        }
        // Remove CR, LF and other space characters from end of buffer
        while (isspace(ss->recvbuf[len - 1])) ss->recvbuf[--len] = 0;
        dlog("Command is %s\n", ss->recvbuf);
        if (strlen(ss->recvbuf) == 0) {
            send_formatted(fd, "-ERR Syntax error, blank command unrecognized\r\n");
            break;
        }
        // Split the command into its component "words"
        ss->nwords = split(ss->recvbuf, ss->words);
        char *command = ss->words[0];
        if (!strcasecmp(command, "QUIT")) {
            if (do_quit(ss) == -1) break;
        } else if (!strcasecmp(command, "USER")) {
            if (do_user(ss) == -1) break;
        } else if (!strcasecmp(command, "PASS")) {
            if (do_pass(ss) == -1) break;
        } else if (!strcasecmp(command, "STAT")) {
            if (do_stat(ss) == -1) break;
        } else if (!strcasecmp(command, "LIST")) {
            if (do_list(ss) == -1) break;
        } else if (!strcasecmp(command, "RETR")) {
            if (do_retr(ss) == -1) break;
        } else if (!strcasecmp(command, "RSET")) {
            if (do_rset(ss) == -1) break;
        } else if (!strcasecmp(command, "NOOP")) {
            if (do_noop(ss) == -1) break;
        } else if (!strcasecmp(command, "DELE")) {
            if (do_dele(ss) == -1) break;
        } else if (!strcasecmp(command, "TOP") ||
                   !strcasecmp(command, "UIDL") ||
                   !strcasecmp(command, "APOP")) {
            dlog("Command not implemented %s\n", ss->words[0]);
            if (send_formatted(fd, "-ERR Command not implemented\r\n") <= 0) break;
        } else {
            // invalid command
            if (send_formatted(fd, "-ERR Syntax error, command unrecognized\r\n") <= 0) break;
        }
    }
    nb_destroy(ss->nb);
}
