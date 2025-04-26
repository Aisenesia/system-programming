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

#include <sys/select.h>

#include "common.h"  // Include the Teller library

#define DATABASE "files/adabank.db"

#define TELLER_DEBUG_FILE "files/teller_debug.txt"
FILE* teller_debug_file = NULL;

// Function prototypes
void handle_signal(int sig);
void cleanup_and_exit();
void teller_function(ClientRequest *request);  // Teller function
void setup_shared_memory();
void cleanup_shared_memory();

int find_client_in_db(const char *bankName, off_t *position, char *buffer,
                      size_t buffer_size);
int add_to_db(DatabaseEntry *req);
int update_db(DatabaseEntry *req);
int remove_from_db(const char *bankName);
void process_database_operations();
int get_available_bank_name();

void *deposit(void *arg);
void *withdraw(void *arg);

// Global variables
int server_fifo_fd = -1;
int teller_id_giver = 1;  // both for teller and the client

SharedMemory *shared_mem = NULL;
sem_t *sem_server = NULL;
sem_t *sem_teller = NULL;
sem_t *sem_db_dependency = NULL;

int main() {
    setup_shared_memory();

    // Register signal handler
    signal(SHUTDOWN_SIGNAL, handle_signal);
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE signal

    // Open the debug file
    teller_debug_file = fopen(TELLER_DEBUG_FILE, "a");
    
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

    // Create the client FIFO
    if (mkfifo(CLIENT_FIFO, 0666) == -1) {
        perror("Error creating client FIFO");
        cleanup_and_exit();
    }

    // Main loop to accept client requests and process database operations
    while (1) {
        // Check for pending database operations first
        if (shared_mem->dbQueue.count > 0) {
            printf("Processing pending database operations...\n");
            process_database_operations();
            continue; // Process all DB operations before handling new clients
        }
    
        // Try to handle client requests without blocking
        // Set up for select to check if CLIENT_FIFO is ready for writing
        int client_fifo_fd = open(CLIENT_FIFO, O_WRONLY | O_NONBLOCK);
        if (client_fifo_fd == -1) {
            if (errno == ENXIO) {
                // No readers on the FIFO yet, wait briefly and try again
                usleep(10000); // 10ms
                continue;
            } else {
                perror("Error opening client FIFO");
                cleanup_and_exit();
            }
        }
    
        // Prepare for select on client FIFO
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(client_fifo_fd, &write_fds);
        
        // Use select with a timeout to check if writing would block
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000; // 100ms timeout
        
        int ready = select(client_fifo_fd + 1, NULL, &write_fds, NULL, &tv);
        if (ready == -1) {
            perror("Error in select");
            close(client_fifo_fd);
            cleanup_and_exit();
        } else if (ready == 0) {
            // Timeout occurred, no client ready to read
            close(client_fifo_fd);
            continue;
        }
        
        // If we get here, CLIENT_FIFO is ready for writing
        if (FD_ISSET(client_fifo_fd, &write_fds)) {
            // Write the next client ID
            if (write(client_fifo_fd, &teller_id_giver, sizeof(teller_id_giver)) == -1) {
                perror("Error writing to client FIFO");
                close(client_fifo_fd);
                cleanup_and_exit();
            }
            close(client_fifo_fd);
            
            // Now open the server FIFO to get the client's request
            server_fifo_fd = open(SERVER_FIFO, O_RDONLY);
            if (server_fifo_fd == -1) {
                perror("Error opening server FIFO");
                cleanup_and_exit();
            }
            
            // Read the number of clients
            int number_of_clients = 0;
            if (read(server_fifo_fd, &number_of_clients, sizeof(number_of_clients)) == -1) {
                perror("Error reading from server FIFO");
                close(server_fifo_fd);
                cleanup_and_exit();
            }
            printf("Number of clients: %d\n", number_of_clients);
            
            // Process each client request
            for (int i = 0; i < number_of_clients; i++) {
                ClientRequest request;
                if (read(server_fifo_fd, &request, sizeof(request)) == -1) {
                    perror("Error reading from server FIFO");
                    close(server_fifo_fd);
                    cleanup_and_exit();
                }
                printf("Received request from client %d: %s %s %d\n",
                      request.clientID, request.bankName,
                      request.operation == DEPOSIT ? "deposit" : "withdraw",
                      request.amount);
                teller_id_giver++;
                // Create a new teller process to handle this request
                teller_function(&request);
            }
            
            close(server_fifo_fd);
        }
        
        // Always check for database operations after processing client requests
        if (shared_mem->dbQueue.count > 0) {
            process_database_operations();
        }
    }
    cleanup_and_exit();
    return 0;
}  



// teller main function

void teller_function(ClientRequest *request) {
    // Determine the operation type, call Teller(deposit/withdraw)
    printf("Teller %d: Processing request for %s\n", request->clientID,
           request->bankName);
    if (request->operation == DEPOSIT) {
        // Create a new process for deposit operation
        pid_t teller_pid = Teller(deposit, request);
        if (teller_pid == -1) {
            perror("Error creating deposit teller process");
            return;
        }
        // waitTeller(teller_pid, NULL);  // Wait for the teller process to
        // finish
    } else if (request->operation == WITHDRAW) {
        // Create a new process for withdraw operation
        pid_t teller_pid = Teller(withdraw, request);
        if (teller_pid == -1) {
            perror("Error creating withdraw teller process");
            return;
        }
        // waitTeller(teller_pid, NULL);  // Wait for the teller process to
        // finish
    } else {
        fprintf(stderr, "Invalid operation type\n");
    }
}

void *deposit(void *arg) {
    ClientRequest *request = (ClientRequest *)arg;
    ServerResponse response;
    char teller_fifo[64];
    printf("Teller %d: Processing deposit request for %s\n", request->clientID,
           request->bankName);

    // Create a unique FIFO for this teller
    sprintf(teller_fifo, "%s_%d", CLIENT_FIFO, request->clientID);
    printf("Dumping to fifo: %s\n", teller_fifo);

    // Create the teller's response FIFO
    if (mkfifo(teller_fifo, 0666) == -1) {
        perror("Error creating teller FIFO");
        return NULL;
    }

    // Initialize response values
    response.success = -2;
    response.balance = 0;
    strcpy(response.message, "");

    // Prepare database command
    DatabaseCommand cmd;
    cmd.operation = DEPOSIT;
    strcpy(cmd.entry.bankName, request->bankName);  // this is wrong. TODO.
    cmd.entry.id = request->clientID;
    cmd.entry.balance = request->amount;  // For deposit, this is amount to add

    // Acquire server semaphore to modify shared memory
    if (sem_wait(sem_server) == -1) {
        perror("Error waiting for server semaphore");
        unlink(teller_fifo);
        return NULL;
    }

    // Add command to shared memory
    shared_mem->request = *request;
    shared_mem->processed = 0;

    // Add command to database operation queue
    if (shared_mem->dbQueue.count < MAX_QUEUE_SIZE) {
        int tail = shared_mem->dbQueue.tail;
        shared_mem->dbQueue.commands[tail] = cmd;
        shared_mem->dbQueue.tail = (tail + 1) % MAX_QUEUE_SIZE;
        shared_mem->dbQueue.count++;
    } else {
        strcpy(response.message, "Database operation queue is full");
        sem_post(sem_server);
        goto send_response;
    }

    // Signal the server to process the database operation
    sem_post(sem_server);

    // Wait for server to process the request
    if (sem_wait(sem_teller) == -1) {
        perror("Error waiting for teller semaphore");
        unlink(teller_fifo);
        return NULL;
    }

    // Check if the operation was successful
    // Modify the response handling in the deposit function:
    while (!shared_mem->processed) {
        if (cmd.result != -1) {
            // Success
            fprintf(teller_debug_file, "Teller %d: Deposit successful\n", request->clientID);
            response.bankID = cmd.result;  // Bank ID
            response.success = 1;
            response.balance = shared_mem->request.amount;  // Updated balance
            sprintf(response.message,
                    "%s has been created. %d$ has been added.",
                    request->bankName, request->amount);
        } else {
            // Failed
            fprintf(teller_debug_file,"Teller %d: Deposit failed\n", request->clientID);
            response.success = 0;
            if (strcmp(request->bankName, "N") == 0) {
                sprintf(response.message,
                        "Failed to create new account and deposit %d$",
                        request->amount);
            } else {
                sprintf(response.message, "Failed to deposit %d$ to %s",
                        request->amount, request->bankName);
            }
        }
    }

    // Release teller semaphore
    sem_post(sem_teller);

send_response:
    // Open the teller FIFO to send the response
    printf("Teller %d: Sending response to FIFO %s\n", request->clientID,
           teller_fifo);
    int teller_fd = open(teller_fifo, O_WRONLY);
    if (teller_fd == -1) {
        perror("Error opening teller FIFO");
        unlink(teller_fifo);
        return NULL;
    }
    printf("Teller %d: Writing response to FIFO %s\n", request->clientID,
           teller_fifo);

    // Send the response
    if (write(teller_fd, &response, sizeof(response)) == -1) {
        perror("Error writing to teller FIFO");
    }

    // Cleanup
    close(teller_fd);
    unlink(teller_fifo);

    printf("Teller %d: Processed deposit request for %s\n", request->clientID,
           request->bankName);

    return NULL;
}

void *withdraw(void *arg) {
    ClientRequest *request = (ClientRequest *)arg;
    ServerResponse response;
    char teller_fifo[64];
    printf("Teller %d: Processing withdraw request for %s\n", request->clientID, request->bankName);

    // Create a unique FIFO for this teller
    sprintf(teller_fifo, "%s_%d", CLIENT_FIFO, request->clientID);
    printf("Dumping to fifo: %s\n", teller_fifo);

    // Create the teller's response FIFO
    if (mkfifo(teller_fifo, 0666) == -1) {
        perror("Error creating teller FIFO");
        return NULL;
    }

    // Initialize response values
    response.success = -2;
    response.balance = 0;
    strcpy(response.message, "");

    if(strcmp(request->bankName, "N") == 0) {
        strcpy(response.message, "Cannot withdraw from a new account");
        goto send_response;
    }

    // Prepare database command
    DatabaseCommand cmd;
    cmd.operation = WITHDRAW;
    strcpy(cmd.entry.bankName, request->bankName);
    cmd.entry.id = request->clientID;
    cmd.entry.balance = request->amount;  // For withdraw, this is amount to subtract

    // Acquire server semaphore to modify shared memory
    if (sem_wait(sem_server) == -1) {
        perror("Error waiting for server semaphore");
        unlink(teller_fifo);
        return NULL;
    }

    // Add command to shared memory
    shared_mem->request = *request;
    shared_mem->processed = 0;

    // Add command to database operation queue
    if (shared_mem->dbQueue.count < MAX_QUEUE_SIZE) {
        int tail = shared_mem->dbQueue.tail;
        shared_mem->dbQueue.commands[tail] = cmd;
        shared_mem->dbQueue.tail = (tail + 1) % MAX_QUEUE_SIZE;
        shared_mem->dbQueue.count++;
    } else {
        strcpy(response.message, "Database operation queue is full");
        sem_post(sem_server);
        goto send_response;
    }

    // Signal the server to process the database operation
    sem_post(sem_server);

    // Wait for server to process the request
    if (sem_wait(sem_teller) == -1) {
        perror("Error waiting for teller semaphore");
        unlink(teller_fifo);
        return NULL;
    }

    // Check if the operation was successful
    while (!shared_mem->processed) {
        if (cmd.result != -1) {
            // Success
            fprintf(teller_debug_file, "Teller %d: Withdraw successful\n", request->clientID);
            response.bankID = cmd.result;  // Bank ID
            response.success = 1;
            response.balance = shared_mem->request.amount;  // Updated balance
            sprintf(response.message,
                    "%d$ has been withdrawn from %s. Remaining balance: %d$",
                    request->amount, request->bankName, response.balance);
        } else {
            // Failed
            fprintf(teller_debug_file, "Teller %d: Withdraw failed\n", request->clientID);
            response.success = 0;
            sprintf(response.message, "Failed to withdraw %d$ from %s",
                    request->amount, request->bankName);
        }
    }

    // Release teller semaphore
    sem_post(sem_teller);

send_response:
    // Open the teller FIFO to send the response
    printf("Teller %d: Sending response to FIFO %s\n", request->clientID,
           teller_fifo);
    int teller_fd = open(teller_fifo, O_WRONLY);
    if (teller_fd == -1) {
        perror("Error opening teller FIFO");
        unlink(teller_fifo);
        return NULL;
    }
    printf("Teller %d: Writing response to FIFO %s\n", request->clientID,
           teller_fifo);

    // Send the response
    if (write(teller_fd, &response, sizeof(response)) == -1) {
        perror("Error writing to teller FIFO");
    }

    // Cleanup
    close(teller_fd);
    unlink(teller_fifo);

    printf("Teller %d: Processed withdraw request for %s\n", request->clientID,
           request->bankName);

    return NULL;
}

void process_database_operations() {
    if (sem_wait(sem_server) == -1) {
        perror("Error waiting for server semaphore");
        return;
    }

    // Check if there are any operations in the queue
    if (shared_mem->dbQueue.count > 0) {
        // Get the next command from the queue
        DatabaseCommand cmd =
            shared_mem->dbQueue.commands[shared_mem->dbQueue.head];
        shared_mem->dbQueue.head =
            (shared_mem->dbQueue.head + 1) % MAX_QUEUE_SIZE;
        shared_mem->dbQueue.count--;

        // Process the command based on its operation type
        if (cmd.operation == DEPOSIT) {
            char buffer[256];
            off_t position;
            int found = find_client_in_db(cmd.entry.bankName, &position, buffer,
                                          sizeof(buffer));

            // Check if it's a new account request
            if (strcmp(cmd.entry.bankName, "N") == 0) {
                // Create a new account with a unique ID
                sprintf(cmd.entry.bankName, "BankID_%d",
                        get_available_bank_name());
                printf("Creating new account with ID: %s\n",
                       cmd.entry.bankName);

                DatabaseEntry new_entry;
                strcpy(new_entry.bankName, cmd.entry.bankName);
                new_entry.id = cmd.entry.id;
                new_entry.balance = cmd.entry.balance;

                int res = add_to_db(&new_entry);
                if (res != -1) {
                    printf("DEBUG: SUCESS GIVEN IN ADD, %d\n", new_entry.id);

                    cmd.result = new_entry.id;  // Success
                } else {
                    printf("DEBUG: FAILED GIVEN IN ADD, %d\n", new_entry.id);
                    cmd.result = -1;  // Failed to add
                }
            } else if (found != -1) {
                // Account exists, update the balance
                DatabaseEntry existing_entry;
                sscanf(buffer, "%s %d %d", existing_entry.bankName,
                       &existing_entry.id, &existing_entry.balance);
                existing_entry.balance += cmd.entry.balance;

                int res = update_db(&existing_entry);
                printf("DEBUG: SOMETHING IN UPDATE, %d\n",
                       existing_entry.id);
                cmd.result = (res == 0) ? existing_entry.id : -1;
                shared_mem->request.amount =
                    existing_entry
                        .balance;  // Update the balance in the response
            } else {
                // Account doesn't exist and not a new account request
                printf("DEBUG: DOESNT EXIST, %d\n", cmd.entry.id);
                cmd.result = -1;
            }
        } else if (cmd.operation == WITHDRAW) {
            // For withdraw, check if the account exists and has sufficient
            // balance
            char buffer[256];
            off_t position;
            int found = find_client_in_db(cmd.entry.bankName, &position, buffer,
                                          sizeof(buffer));

            if (found == -1) {
                // Account doesn't exist
                printf("DEBUG: FOUND -1, %d\n", cmd.entry.id);
                cmd.result = -1;
            } else {
                // Account exists, check balance
                DatabaseEntry existing_entry;
                sscanf(buffer, "%s %d %d", existing_entry.bankName,
                       &existing_entry.id, &existing_entry.balance);

                if (existing_entry.balance < cmd.entry.balance) {
                    // Insufficient balance
                    printf("DEBUG: INSUFFICIENT BALANCE, %d\n",
                           existing_entry.id);
                    cmd.result = -1;
                } else {
                    // Sufficient balance, update or remove
                    existing_entry.balance -= cmd.entry.balance;
                    shared_mem->request.amount =
                        existing_entry
                            .balance;  // Update the balance in the response

                    if (existing_entry.balance == 0) {
                        // Remove the account if balance is 0
                        cmd.result =
                            remove_from_db(existing_entry.bankName);
                    } else {
                        // Update the account
                        printf("DEBUG: UPDATE WITHDRAW, %d\n", existing_entry.id);
                        cmd.result = update_db(&existing_entry);
                    }
                }
            }
        }

        // Mark the request as processed
        shared_mem->processed = 1;
    }

    sem_post(sem_server);

    // Signal the teller that the operation has been processed
    sem_post(sem_teller);
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

int get_available_bank_name() {
    FILE *db_file = fopen(DATABASE, "r");
    if (db_file == NULL) {
        perror("Error opening database");
        return 1;  // Default to 1 if file can't be opened
    }

    int last_bank_id = 0;
    char line[256];
    char bank_name[32];
    int id, balance;

    // Read line by line
    while (fgets(line, sizeof(line), db_file) != NULL) {
        if (sscanf(line, "%s %d %d", bank_name, &id, &balance) == 3) {
            if (strncmp(bank_name, "BankID_", 7) == 0) {
                int bank_id = atoi(&bank_name[7]);
                if (bank_id > last_bank_id) {
                    last_bank_id = bank_id;
                }
            }
        }
    }

    fclose(db_file);
    return last_bank_id + 1;  // Return the next available bank ID
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
    printf("DEBUG: Added to database: %d\n", req->id);
    close(db_fd);
    return req->id;  // Success
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
