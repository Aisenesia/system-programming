#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <semaphore.h>
#include "common.h" // Include the Teller library

#define DATABASE "adabank.db"

// Function prototypes
void handle_signal(int sig);
void cleanup_and_exit();
void teller_function(void *arg); // Teller function
void setup_shared_memory();
void cleanup_shared_memory();
int find_client_in_db(const char *clientName, off_t *position, char *buffer, size_t buffer_size);
int add_to_db(DatabaseEntry *req);
int update_db(DatabaseEntry *req);
int remove_from_db(const char *clientName);

// Global variables
int server_fifo_fd = -1;
int teller_id_giver = 0; // both for teller and the client

SharedMemory *shared_mem = NULL;
sem_t *sem_server = NULL;
sem_t *sem_teller = NULL;

int main() {
    setup_shared_memory();

    // Register signal handler
    signal(SHUTDOWN_SIGNAL, handle_signal);

    printf("Adabank is active…\n");

    if (access(LOG_FILE, F_OK) == 0) {
        printf("Previous logs found. Loading the bank database...\n");
        // Load the bank database from the log file
        
    } else {
        printf("No previous logs.. Creating the bank database\n");
        // create the bank database
        
    }

    // Create the server FIFO
    if (mkfifo(SERVER_FIFO, 0666) == -1) {
        perror("Error creating server FIFO");
        exit(ERR_FIFO_FAILURE);
    }
    printf("Waiting for clients @%s…\n", SERVER_FIFO);

    // Open the server FIFO for reading
    server_fifo_fd = open(SERVER_FIFO, O_RDONLY);
    if (server_fifo_fd == -1) {
        perror("Error opening server FIFO");
        cleanup_and_exit();
    }

    // Main loop to accept client requests
    while (1) {
        ClientRequest req;
        ssize_t bytes_read = read(server_fifo_fd, &req, sizeof(ClientRequest));
        if (bytes_read <= 0) {
            perror("Error reading from FIFO");
            break;
        }

        // Assign a unique client ID for the request
        teller_id_giver++;
        req.clientID = teller_id_giver;

        printf("Received request from client ID: %d\n", req.clientID);

        // Handle new accounts (bankID generation)
        if (req.clientName[0] == 'N') {
            snprintf(req.clientName, sizeof(req.clientName), "BankID_%02d", teller_id_giver);
            printf("New bank account created: %s\n", req.clientName);
        }

        // Write the request to shared memory
        sem_wait(sem_server);
        shared_mem->request = req;
        shared_mem->processed = 0;
        sem_post(sem_teller);

        // Execute the teller program
        pid_t teller_pid = fork();
        if (teller_pid == 0) {
            // Child process: Execute the teller program
            execl("./teller", "teller", NULL);
            perror("Error executing teller program");
            exit(1);
        } else if (teller_pid < 0) {
            perror("Error forking teller process");
        }

        // Wait for the teller to process the request
        sem_wait(sem_server);
        if (shared_mem->processed) {
            printf("Processed request for client ID: %d\n", req.clientID);
        }
        sem_post(sem_server);
    }

    cleanup_and_exit();
    return 0;
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
    shared_mem = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
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

// Teller function to handle client requests
void teller_function(void *arg) {
    while (1) {
        sem_wait(sem_teller);

        // Read request from shared memory
        ClientRequest *req = &shared_mem->request;

        printf("Teller PID%02d is processing request for client: %s\n", getpid(), req->clientName);

        if (req->op == DEPOSIT) {
            DatabaseEntry entry;
            strncpy(entry.clientName, req->clientName, sizeof(entry.clientName) - 1);
            entry.clientName[sizeof(entry.clientName) - 1] = '\0'; // Ensure null-termination
            entry.id = req->clientID;
            entry.balance = req->amount;

            printf("%s deposited %d credits… updating log\n", req->clientName, req->amount);
            add_to_db(&entry);
        } else if (req->op == WITHDRAW) {
            DatabaseEntry entry;
            strncpy(entry.clientName, req->clientName, sizeof(entry.clientName) - 1);
            entry.clientName[sizeof(entry.clientName) - 1] = '\0'; // Ensure null-termination
            entry.id = req->clientID;
            entry.balance = 0;

            printf("%s withdraws %d credits… ", req->clientName, req->amount);
            if (find_client_in_db(req->clientName, NULL, NULL, 0) != -1) {
                entry.balance -= req->amount;
                if (entry.balance >= 0) {
                    printf("updating log\n");
                    update_db(&entry);
                } else {
                    printf("operation not permitted.\n");
                }
            } else {
                printf("operation not permitted.\n");
            }
        }

        shared_mem->processed = 1;
        sem_post(sem_server);
    }
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
    unlink(SERVER_FIFO); // Remove the FIFO
    cleanup_shared_memory();
    printf("Removing ServerFIFO… Updating log file…\n");
    printf("Adabank says “Bye”…\n");
    exit(0);
}

// Helper function to find a client in the database and return its position
int find_client_in_db(const char *clientName, off_t *position, char *buffer, size_t buffer_size) {
    int db_fd = open(DATABASE, O_RDWR);
    if (db_fd == -1) {
        perror("Error opening database");
        return -1;
    }

    off_t offset = 0;
    while (read(db_fd, buffer, buffer_size) > 0) {
        if (strstr(buffer, clientName) != NULL) {
            *position = offset;
            close(db_fd);
            return db_fd; // Return the file descriptor for further operations
        }
        offset += strlen(buffer);
    }

    close(db_fd);
    return -1; // Client not found
}

int add_to_db(DatabaseEntry *req) {
    int db_fd = open(DATABASE, O_WRONLY | O_APPEND);
    if (db_fd == -1) {
        perror("Error opening database");
        return -1;
    }

    char buffer[256];
    sprintf(buffer, "%s %d %d\n", req->clientName, req->id, req->balance);
    if (write(db_fd, buffer, strlen(buffer)) == -1) {
        perror("Error writing to database");
        close(db_fd);
        return -1;
    }

    close(db_fd);
    return 0; // Success
}

int update_db(DatabaseEntry *req) {
    char buffer[256];
    off_t position;
    int db_fd = find_client_in_db(req->clientName, &position, buffer, sizeof(buffer));
    if (db_fd == -1) {
        return -1; // Client not found
    }

    lseek(db_fd, position, SEEK_SET);
    sprintf(buffer, "%s %d %d\n", req->clientName, req->id, req->balance);
    if (write(db_fd, buffer, strlen(buffer)) == -1) {
        perror("Error writing to database");
        close(db_fd);
        return -1;
    }

    close(db_fd);
    return 0; // Success
}

int remove_from_db(const char *clientName) {
    char buffer[256];
    off_t position;
    int db_fd = find_client_in_db(clientName, &position, buffer, sizeof(buffer));
    if (db_fd == -1) {
        return -1; // Client not found
    }

    FILE *temp_file = fopen("temp.db", "w");
    if (!temp_file) {
        perror("Error creating temporary file");
        close(db_fd);
        return -1;
    }

    lseek(db_fd, 0, SEEK_SET);
    while (read(db_fd, buffer, sizeof(buffer)) > 0) {
        if (strstr(buffer, clientName) == NULL) {
            fwrite(buffer, 1, strlen(buffer), temp_file);
        }
    }

    fclose(temp_file);
    close(db_fd);

    // Replace the original database with the temporary file
    remove(DATABASE);
    rename("temp.db", DATABASE);

    return 0; // Success
}

