#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"

ClientRequest generateRequestFromLine(int clid, char* line);
int wait_for_server_fifo();

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
        
        // Add request to requests array
        requests[line_number - 1] = request;
    }
    fclose(client_file);
    printf("%d clients to connect.. creating clients..\n", line_number);

    // mkfifo
    //  Create the client FIFO

    // read client starting id from server fifo.
    int client_fifo_fd = open(CLIENT_FIFO, O_RDONLY);
    if (client_fifo_fd == -1) {
        char err[64];
        sprintf(err, "Cannot connect %s\n", SERVER_FIFO);
        printf(err);
        return 1;
    }
    int client_id;

    // TODO, if another client is already connected, we should wait for it to finish, we can try to open for read non-blocking
    // if it does not block, there is another client connected, we should wait for it to finish
    if (wait_for_server_fifo() == -1) {
        return 1; // Error waiting for server FIFO
    }


    if (read(client_fifo_fd, &client_id, sizeof(client_id)) == -1) {
        perror("Error writing to client FIFO");
        close(client_fifo_fd);
        return 1;
    }

    close(client_fifo_fd);
    printf("Connected to Adabank..\n");
    for (int i = 0; i < line_number; i++) {
        requests[i].clientID = client_id + i;
        printf("Client%02d connected..%s %d credits\n", requests[i].clientID,
                requests[i].operation == DEPOSIT ? "depositing" : "withdrawing",
                requests[i].amount);
    }
    printf("..\n");

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
        printf("%s\n", response.message);
    }
    printf("exiting..\n");
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

int wait_for_server_fifo() {
    int server_fifo_fd;
    while (1) {
        // Try to open the server FIFO in non-blocking mode
        server_fifo_fd = open(SERVER_FIFO, O_RDONLY | O_NONBLOCK);
        if (server_fifo_fd != -1) {
            // Successfully opened, no other client is writing
            close(server_fifo_fd);
            break;
        }

        // Check if the error is due to the FIFO being busy
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("Another client is connected. Waiting...\n");
            usleep(100000); // Wait for 100ms before retrying
        } else {
            // Some other error occurred
            perror("Error opening server FIFO");
            return -1;
        }
    }
    return 0;
}
