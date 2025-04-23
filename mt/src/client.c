#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "common.h"

// Reads a client file (A batch job, connect to the server fifo)

/* example client file (client01.file)

N deposit 300 // New account, deposit 300, if there is no previous account it will be created as BankID_01
BankID_None withdraw 30 // Withdraw 30 from account BankID_None, it will fail
N deposit 2000 // New account, deposit 2000

another file Client02.FILE

BankID_01 withdraw 300 // if we execute cclient01.file first, this will succeed
N deposit 20 // New account, deposit 20


another file:
BankID_02 withdraw 30 // since BankID_02 created with deposit 20, this will fail since there is not enough money
N deposit 2000
BankID_02 deposit 200
BankID_02 withdraw 300
N withdraw 20


*/

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <client_file>\n", argv[0]);
        return 1;
    }

    printf("Reading %s\n", argv[1]);

    // Open the client file
    FILE *file = fopen(argv[1], "r");
    if (file == NULL) {
        perror("Error opening client file");
        return 1;
    }

    // Array to store requests
    ClientRequest requests[100];
    int request_count = 0;

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        // Parse the line
        char clientName[32];
        char operation[10];
        int amount;
        if (sscanf(line, "%s %s %d", clientName, operation, &amount) < 2) {
            fprintf(stderr, "Invalid line format: %s", line);
            continue;
        }

        // Create a ClientRequest struct
        ClientRequest req;
        req.clientID = -1; // Default value for new clients
        req.op = (strcmp(operation, "deposit") == 0) ? DEPOSIT : WITHDRAW;
        req.amount = amount;
        strncpy(req.clientName, clientName, sizeof(req.clientName) - 1);
        req.clientName[sizeof(req.clientName) - 1] = '\0'; // Ensure null-termination
        req.clientName[0] = '-'; // name will be given by the server

        // Save the request
        requests[request_count++] = req;
    }
    fclose(file);

    printf("Number of clients to be processed: %d\n", request_count);

    // Connect to the server FIFO
    int server_fifo_fd = open(SERVER_FIFO, O_WRONLY);
    if (server_fifo_fd == -1) {
        perror("Error opening server FIFO");
        return 1;
    }
    printf("Connected to Adabank.\n");

    // Process the requests
    for (int i = 0; i < request_count; i++) {
        ssize_t bytes_written = write(server_fifo_fd, &requests[i], sizeof(ClientRequest));
        if (bytes_written == -1) {
            perror("Error writing to server FIFO");
            close(server_fifo_fd);
            return 1;
        }
        printf("Sent request: %s %s %d\n", requests[i].clientName, 
               (requests[i].op == DEPOSIT) ? "deposit" : "withdraw", 
               requests[i].amount);
    }

    // Close the server FIFO
    close(server_fifo_fd);

    return 0;
}

