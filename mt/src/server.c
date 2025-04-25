#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"  // Include the Teller library

#define DATABASE "files/adabank.db"

// Function prototypes
void handle_signal(int sig);
void cleanup_and_exit();
void teller_function();  // Teller function
void setup_shared_memory();
void cleanup_shared_memory();
int find_client_in_db(const char *bankName, off_t *position, char *buffer,
                      size_t buffer_size);
int add_to_db(DatabaseEntry *req);
int update_db(DatabaseEntry *req);
int remove_from_db(const char *bankName);
void process_database_operations();
void *deposit(void *arg);
void *withdraw(void *arg);


// Global variables
int server_fifo_fd = -1;
int teller_id_giver = 1;  // both for teller and the client

SharedMemory *shared_mem = NULL;
sem_t *sem_server = NULL;
sem_t *sem_teller = NULL;

int main() {
    // setup_shared_memory();

    // Register signal handler
    signal(SHUTDOWN_SIGNAL, handle_signal);
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE signal

    printf("Adabank is active…\n");

    if (access(DATABASE, F_OK) == 0) {
        printf("Previous database found. Loading the bank database...\n");

    } else {
        printf("No previous logs.. Creating the bank database\n");
        FILE *db_file = fopen(DATABASE, "w");
        if (db_file == NULL) {
            perror("Error creating database file");
            cleanup_and_exit();
        }
        fclose(db_file);
    }

    // Create the server FIFO
    if (mkfifo(SERVER_FIFO, 0666) == -1) {
        perror("Error creating server FIFO");
        exit(ERR_FIFO_FAILURE);
    }
    printf("Waiting for clients @%s…\n", SERVER_FIFO);

    // Open the server FIFO for writing, always write next client id

    // Create the client FIFO
    if (mkfifo(CLIENT_FIFO, 0666) == -1) {
        perror("Error creating client FIFO");
        cleanup_and_exit();
    }

    // Main loop to accept client requests and process database operations
    while (1) {
        // write the client id to the client fifo
        int client_fifo_fd = open(CLIENT_FIFO, O_WRONLY);
        if (client_fifo_fd == -1) {
            perror("Error opening client FIFO");
            cleanup_and_exit();
        }
        if (write(client_fifo_fd, &teller_id_giver, sizeof(teller_id_giver)) ==
            -1) {
            perror("Error writing to server FIFO");
            close(client_fifo_fd);
            cleanup_and_exit();
        }
        int number_of_clients = 0;
        // Read the client request from the FIFO
        server_fifo_fd = open(SERVER_FIFO, O_RDONLY);
        if (server_fifo_fd == -1) {
            perror("Error opening server FIFO");
            cleanup_and_exit();
        }
        if (read(server_fifo_fd, &number_of_clients,
                 sizeof(number_of_clients)) == -1) {
            perror("Error reading from server FIFO");
            close(client_fifo_fd);
            cleanup_and_exit();
        }
        printf("Number of clients: %d\n", number_of_clients);

        for (int i = 0; i < number_of_clients; i++) {
            ClientRequest request;
            if (read(server_fifo_fd, &request, sizeof(request)) == -1) {
                perror("Error reading from server FIFO");
                close(client_fifo_fd);
                cleanup_and_exit();
            }
            printf("Received request from client %d: %s %s %d\n",
                   request.clientID, request.bankName,
                   request.operation == DEPOSIT ? "deposit" : "withdraw",
                   request.amount);
                   teller_id_giver++;
            // Create a new teller process
            teller_function(&request);

        }
    }

    cleanup_and_exit();
    return 0;
}

// teller main function

void teller_function(ClientRequest *request) {
    // Determine the operation type, check db for the client, if N create a new client, call Teller(deposit/withdraw)
    if (request->operation == DEPOSIT) {
        // Create a new process for deposit operation
        pid_t teller_pid = Teller(deposit, request);
        if (teller_pid == -1) {
            perror("Error creating deposit teller process");
            return;
        }
        waitTeller(teller_pid, NULL);  // Wait for the teller process to finish
    } else if (request->operation == WITHDRAW) {
        // Create a new process for withdraw operation
        pid_t teller_pid = Teller(withdraw, request);
        if (teller_pid == -1) {
            perror("Error creating withdraw teller process");
            return;
        }
        waitTeller(teller_pid, NULL);  // Wait for the teller process to finish
    } else {
        fprintf(stderr, "Invalid operation type\n");
    }
}

void* deposit(void* arg) {
    // Cases: bankName is N, bankName is a valid bank name that exists in the database, bankName is a valid bank name that does not exist in the database
    // Flow: using shared memory, 
}
void* withdraw(void* arg) {
    // cases:
    // 1. bankName is N or another Invalid name - Fail (new client cannot withdraw so might as well just give error)
    // 2. bankName is a valid bank name that exists in the database with sufficient balance - Success
    // 3. bankName is a valid bank name that exists in the database with insufficient balance - Fail
    // 4. balance after withdraw is 0 - remove the client from the database
    // Flow: using shared memory, 
}


void setup_shared_memory() {
    // Create shared memory object
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Error creating shared memory");
        exit(1);
    }

    // Set the size of the shared memory
    if (ftruncate(shm_fd, sizeof(SharedMemory)) == -1) {
        perror("Error setting size of shared memory");
        exit(1);
    }

    // Map the shared memory
    shared_mem = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE,
                      MAP_SHARED, shm_fd, 0);
    if (shared_mem == MAP_FAILED) {
        perror("Error mapping shared memory");
        exit(1);
    }

    // Initialize semaphores
    sem_server = sem_open("/sem_server", O_CREAT | O_EXCL, 0666, 1);
    sem_teller = sem_open("/sem_teller", O_CREAT | O_EXCL, 0666, 0);
    if (sem_server == SEM_FAILED || sem_teller == SEM_FAILED) {
        perror("Error creating semaphores");
        exit(1);
    }
}

void cleanup_shared_memory() {
    // Unlink and close semaphores
    sem_unlink("/sem_server");
    sem_unlink("/sem_teller");
    sem_close(sem_server);
    sem_close(sem_teller);

    // Unlink and unmap shared memory
    shm_unlink(SHM_NAME);
    munmap(shared_mem, sizeof(SharedMemory));
}

// Signal handler for graceful shutdown
void handle_signal(int sig) {
    printf("Signal received closing active Tellers\n");
    cleanup_and_exit();
}

// Cleanup resources and exit
void cleanup_and_exit() {
    if (server_fifo_fd != -1) {
        close(server_fifo_fd);
    }
    unlink(SERVER_FIFO);  // Remove the FIFO
    unlink(CLIENT_FIFO);  // Remove the FIFO
    cleanup_shared_memory();
    printf("Removing ServerFIFO… Updating log file…\n");
    printf("Adabank says “Bye”…\n");
    exit(0);
}

// Helper function to find a client in the database and return its position
int find_client_in_db(const char *bankName, off_t *position, char *buffer,
                      size_t buffer_size) {
    int db_fd = open(DATABASE, O_RDWR);
    if (db_fd == -1) {
        perror("Error opening database");
        return -1;
    }

    off_t offset = 0;
    while (read(db_fd, buffer, buffer_size) > 0) {
        if (strstr(buffer, bankName) != NULL) {
            *position = offset;
            close(db_fd);
            return db_fd;  // Return the file descriptor for further operations
        }
        offset += strlen(buffer);
    }

    close(db_fd);
    return -1;  // Client not found
}

int add_to_db(DatabaseEntry *req) {
    int db_fd = open(DATABASE, O_WRONLY | O_APPEND);
    if (db_fd == -1) {
        perror("Error opening database");
        return -1;
    }

    char buffer[256];
    sprintf(buffer, "%s %d %d\n", req->bankName, req->id, req->balance);
    if (write(db_fd, buffer, strlen(buffer)) == -1) {
        perror("Error writing to database");
        close(db_fd);
        return -1;
    }

    close(db_fd);
    return 0;  // Success
}

int update_db(DatabaseEntry *req) {
    char buffer[256];
    off_t position;
    int db_fd =
        find_client_in_db(req->bankName, &position, buffer, sizeof(buffer));
    if (db_fd == -1) {
        return -1;  // Client not found
    }

    lseek(db_fd, position, SEEK_SET);
    sprintf(buffer, "%s %d %d\n", req->bankName, req->id, req->balance);
    if (write(db_fd, buffer, strlen(buffer)) == -1) {
        perror("Error writing to database");
        close(db_fd);
        return -1;
    }

    close(db_fd);
    return 0;  // Success
}

int remove_from_db(const char *bankName) {
    char buffer[256];
    off_t position;
    int db_fd = find_client_in_db(bankName, &position, buffer, sizeof(buffer));
    if (db_fd == -1) {
        return -1;  // Client not found
    }

    FILE *temp_file = fopen("temp.db", "w");
    if (!temp_file) {
        perror("Error creating temporary file");
        close(db_fd);
        return -1;
    }

    lseek(db_fd, 0, SEEK_SET);
    while (read(db_fd, buffer, sizeof(buffer)) > 0) {
        if (strstr(buffer, bankName) == NULL) {
            fwrite(buffer, 1, strlen(buffer), temp_file);
        }
    }

    fclose(temp_file);
    close(db_fd);

    // Replace the original database with the temporary file
    remove(DATABASE);
    rename("temp.db", DATABASE);

    return 0;  // Success
}
