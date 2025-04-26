#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"

// Reads a client file (A batch job, connect to the server fifo)

/* example client file (client01.file)

N deposit 300 // New account, deposit 300, if there is no previous account it
will be created as BankID_01 BankID_None withdraw 30 // Withdraw 30 from account
BankID_None, it will fail N deposit 2000 // New account, deposit 2000

another file Client02.FILE

BankID_01 withdraw 300 // if we execute cclient01.file first, this will succeed
N deposit 20 // New account, deposit 20


another file:
BankID_02 withdraw 30 // since BankID_02 created with deposit 20, this will fail
since there is not enough money N deposit 2000 BankID_02 deposit 200 BankID_02
withdraw 300 N withdraw 20


*/
ClientRequest generateRequestFromLine(int clid, char* line);

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <client_file>\n", argv[0]);
        return 1;
    }

    printf("Reading %s\n", argv[1]);

    // Read the client file
    FILE* client_file = fopen(argv[1], "r");
    if (client_file == NULL) {
        perror("Error opening client file");
        return 1;
    }
    ClientRequest requests[256];
    char line[256];
    int line_number = 0;
    while (fgets(line, sizeof(line), client_file) != NULL) {
        line_number++;
        // Skip empty lines
        if (strlen(line) == 0 || line[0] == '\n') {
            continue;
        }
        // Generate request from line
        ClientRequest request = generateRequestFromLine(0, line);
        printf("Generated request from line %d: %s %s %d\n", line_number,
               request.bankName,
               request.operation == DEPOSIT ? "deposit" : "withdraw",
               request.amount);
        // Add request to requests array
        requests[line_number - 1] = request;
    }
    fclose(client_file);

    // mkfifo
    //  Create the client FIFO

    // read client starting id from server fifo.
    int client_fifo_fd = open(CLIENT_FIFO, O_RDONLY);
    if (client_fifo_fd == -1) {
        perror("client_fifo_fd opening server FIFO");
        return 1;
    }
    int client_id;

    if (read(client_fifo_fd, &client_id, sizeof(client_id)) == -1) {
        perror("Error writing to client FIFO");
        close(client_fifo_fd);
        return 1;
    }

    close(client_fifo_fd);

    for (int i = 0; i < line_number; i++) {
        requests[i].clientID = client_id + i;
    }

    // Open the server FIFO
    int server_fifo_fd = open(SERVER_FIFO, O_WRONLY);
    if (server_fifo_fd == -1) {
        perror("Error opening server FIFO");
        return 1;
    }
    // Write the requests to the server FIFO
    if (write(server_fifo_fd, &line_number, sizeof(line_number)) == -1) {
        perror("Error writing to server FIFO");
        close(server_fifo_fd);
        return 1;
    }
    for (int i = 0; i < line_number; i++) {
        if (write(server_fifo_fd, &requests[i], sizeof(ClientRequest)) == -1) {
            perror("Error writing to server FIFO");
            close(server_fifo_fd);
            return 1;
        }
    }
    close(server_fifo_fd);

    for (int i = 0; i < line_number; i++) {
        char client_fifo[64];
        sprintf(client_fifo, "%s_%d", CLIENT_FIFO, client_id + i);
        printf("Waiting for server response on %s\n", client_fifo);

        while (access(client_fifo, F_OK) == -1);

        client_fifo_fd = open(client_fifo, O_RDONLY);
        if (client_fifo_fd == -1) {
            perror("Error opening client FIFO");
            return 1;
        }
        // Read the response from the server FIFO
        ServerResponse response;
        if (read(client_fifo_fd, &response, sizeof(ServerResponse)) == -1) {
            perror("Error reading from client FIFO");
            close(client_fifo_fd);
            return 1;
        }
        close(client_fifo_fd);
        // Print the response
        printf("Response from server: %s\n", response.message);
    }
    // Cleanup

    // Close the server FIFO

    // Open the client file

    return 0;
}

ClientRequest generateRequestFromLine(int clid, char* line) {
    ClientRequest request;
    char operation[10];
    sscanf(line, "%s %s %d", request.bankName, operation, &request.amount);
    request.clientID = clid;
    if (strcmp(operation, "deposit") == 0) {
        request.operation = DEPOSIT;
    } else if (strcmp(operation, "withdraw") == 0) {
        request.operation = WITHDRAW;
    } else {
        fprintf(stderr, "Unknown operation: %s\n", operation);
        exit(1);
    }

    return request;
}
