#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"  // Include the Teller library

#define DATABASE "files/adabank.db"

#define DEBUG_MODE 0

#define TELLER_DEBUG_FILE "files/teller_debug.txt"
FILE *teller_debug_file = NULL;

// Function prototypes
void handle_signal(int sig);
void cleanup_and_exit();
void teller_function(ClientRequest *request);  // Teller function
void setup_shared_memory();
void cleanup_shared_memory();

int find_client_in_db(const char *bankName, off_t *position, char *buffer,
                      size_t buffer_size);
DatabaseEntry *get_client_from_db(const char *bankName);
int add_to_db(DatabaseEntry *req);
int update_db(DatabaseEntry *req);
int remove_from_db(const char *bankName);
void process_database_operations();
int get_available_bank_name();

void setup_transaction_manager();
int process_deposit(DatabaseCommand *cmd, int *new_balance);
int process_withdraw(DatabaseCommand *cmd, int *new_balance);
int allocate_transaction();
int wait_for_transaction(int transaction_id);

int add_db_command(CommandQueue *queue, DatabaseCommand *cmd);

int get_db_command(CommandQueue *queue, DatabaseCommand *cmd);

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
    // Debuggind Database operations.
    if (DEBUG_MODE) {
        DatabaseEntry *entry;

        entry = get_client_from_db("BankID_02");

        if (entry != NULL) {
            printf("BankID_02 found: %s %d %d\n", entry->bankName, entry->id,
                   entry->balance);
            free(entry);
        } else {
            printf("BankID_02 not found\n");
        }

        // remove_from_db("BankID_01");

        return 0;
    }
    setup_shared_memory();
    setup_transaction_manager();  // Initialize transaction system

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

    // Main server loop
    while (1) {
        // First, process any pending database operations
        process_database_operations();

        // Handle client connections similar to original code
        // Try to open client FIFO non-blocking
        int client_fifo_fd = open(CLIENT_FIFO, O_WRONLY | O_NONBLOCK);
        if (client_fifo_fd == -1) {
            if (errno == ENXIO) {
                // No readers on the FIFO yet, wait briefly and try again
                usleep(10000);  // 10ms
                continue;
            } else {
                perror("Error opening client FIFO");
                cleanup_and_exit();
            }
        }

        // Use select to check if client FIFO is ready for writing
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(client_fifo_fd, &write_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  // 10ms timeout

        int ready = select(client_fifo_fd + 1, NULL, &write_fds, NULL, &tv);
        if (ready <= 0) {
            // Error or timeout, close and continue
            close(client_fifo_fd);
            continue;
        }

        // Write the next client ID and process client requests
        if (write(client_fifo_fd, &teller_id_giver, sizeof(teller_id_giver)) ==
            -1) {
            perror("Error writing to client FIFO");
            close(client_fifo_fd);
            continue;
        }
        close(client_fifo_fd);

        // Open server FIFO to read client requests
        server_fifo_fd = open(SERVER_FIFO, O_RDONLY);
        if (server_fifo_fd == -1) {
            perror("Error opening server FIFO");
            continue;
        }

        // Read and process client requests
        int number_of_clients = 0;
        if (read(server_fifo_fd, &number_of_clients,
                 sizeof(number_of_clients)) == -1) {
            perror("Error reading from server FIFO");
            close(server_fifo_fd);
            continue;
        }

        // Process each client request
        for (int i = 0; i < number_of_clients; i++) {
            ClientRequest request;
            if (read(server_fifo_fd, &request, sizeof(request)) == -1) {
                perror("Error reading from server FIFO");
                break;
            }

            printf("Received request from client %d: %s %s %d\n",
                   request.clientID, request.bankName,
                   request.operation == DEPOSIT ? "deposit" : "withdraw",
                   request.amount);

            teller_id_giver++;
            teller_function(&request);
        }

        close(server_fifo_fd);

        // Check again for database operations before next loop iteration
        process_database_operations();
    }

    cleanup_and_exit();
    return 0;
}

void setup_transaction_manager() {
    // Initialize the transaction manager mutex
    sem_init(&shared_mem->transactionManager.mutex, 1, 1);

    // Initialize all transaction semaphores
    for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
        sem_init(&shared_mem->transactionManager.transactions[i].sem, 1, 0);
        shared_mem->transactionManager.transactions[i].completed =
            1;  // Mark as available
    }

    shared_mem->transactionManager.transaction_count = 0;
}

// Allocate a transaction record
int allocate_transaction() {
    int transaction_id = -1;

    sem_wait(&shared_mem->transactionManager.mutex);

    // Find an available transaction slot
    for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
        if (shared_mem->transactionManager.transactions[i].completed) {
            shared_mem->transactionManager.transactions[i].completed = 0;
            shared_mem->transactionManager.transactions[i].result =
                -2;  // Pending
            transaction_id = i;
            break;
        }
    }

    sem_post(&shared_mem->transactionManager.mutex);
    return transaction_id;
}

// Complete a transaction
void complete_transaction(int transaction_id, int result) {
    if (transaction_id < 0 || transaction_id >= MAX_QUEUE_SIZE) return;

    sem_wait(&shared_mem->transactionManager.mutex);

    shared_mem->transactionManager.transactions[transaction_id].result = result;
    shared_mem->transactionManager.transactions[transaction_id].completed = 1;

    // Signal that the transaction is complete
    sem_post(&shared_mem->transactionManager.transactions[transaction_id].sem);

    sem_post(&shared_mem->transactionManager.mutex);
}

// Wait for a transaction to complete
int wait_for_transaction(int transaction_id) {
    if (transaction_id < 0 || transaction_id >= MAX_QUEUE_SIZE) return -1;

    // Wait for the transaction to complete
    sem_wait(&shared_mem->transactionManager.transactions[transaction_id].sem);

    // Get the result
    int result =
        shared_mem->transactionManager.transactions[transaction_id].result;

    return result;
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

    // Create a unique FIFO for this teller
    sprintf(teller_fifo, "%s_%d", CLIENT_FIFO, request->clientID);

    // Create the teller's response FIFO
    if (mkfifo(teller_fifo, 0666) == -1) {
        perror("Error creating teller FIFO");
        return NULL;
    }

    // Initialize response values
    response.success = -2;
    response.balance = 0;
    strcpy(response.message, "");

    // Allocate a transaction record
    int transaction_id = allocate_transaction();
    if (transaction_id == -1) {
        strcpy(response.message, "System busy, try again later");
        goto send_response;
    }

    // Prepare database command
    DatabaseCommand cmd;
    cmd.operation = DEPOSIT;
    cmd.transaction_id = transaction_id;
    strcpy(cmd.entry.bankName, request->bankName);
    cmd.entry.id = request->clientID;
    cmd.entry.balance = request->amount;


    // Add the command to the queue
    if (!add_db_command(&shared_mem->dbQueue, &cmd)) {
        strcpy(response.message, "Database operation queue is full");
        // Fix the sem_getvalue call by providing a valid pointer to an integer
        int sem_value;
        printf("DEBUG: Queue full. Semaphore spaces value: %d\n",
               sem_getvalue(&shared_mem->dbQueue.spaces, &sem_value));
        complete_transaction(transaction_id, -1);  // Mark as failed
        goto send_response;
    }

    // Wait for the operation to complete
    int result = wait_for_transaction(transaction_id);

    // Check if operation was successful
    if (result != -1) {
        // Success
        response.success = 1;
        response.balance = result;  // Updated balance
        // from where to get the bankID?
        sprintf(response.message, "Client%02d served.. BankID_%02d", request->clientID,
                result);
    } else {
        // Failed
        response.success = 0;
        sprintf(response.message, "Client%02d something went WRONG",
                request->clientID);
    }

send_response:
    // Send the response through the FIFO
    int teller_fd = open(teller_fifo, O_WRONLY);
    if (teller_fd != -1) {
        write(teller_fd, &response, sizeof(response));
        close(teller_fd);
    }

    // Cleanup
    unlink(teller_fifo);
    return NULL;
}

void *withdraw(void *arg) {
    ClientRequest *request = (ClientRequest *)arg;
    ServerResponse response;
    char teller_fifo[64];

    // Create a unique FIFO for this teller
    sprintf(teller_fifo, "%s_%d", CLIENT_FIFO, request->clientID);

    // Create the teller's response FIFO
    if (mkfifo(teller_fifo, 0666) == -1) {
        perror("Error creating teller FIFO");
        return NULL;
    }

    // Initialize response values
    response.success = -2;
    response.balance = 0;
    strcpy(response.message, "");

    // Allocate a transaction record
    int transaction_id = allocate_transaction();
    if (transaction_id == -1) {
        strcpy(response.message, "System busy, try again later");
        goto send_response;
    }

    // Prepare database command
    DatabaseCommand cmd;
    cmd.operation = WITHDRAW;
    cmd.transaction_id = transaction_id;
    strcpy(cmd.entry.bankName, request->bankName);
    cmd.entry.id = request->clientID;
    cmd.entry.balance = request->amount;

    // Add debug log to track semaphore and queue state

    // Add the command to the queue

    if (!add_db_command(&shared_mem->dbQueue, &cmd)) {
        strcpy(response.message, "Database operation queue is full");
        // Fix the sem_getvalue call by providing a valid pointer to an integer
        int sem_value;
        printf("DEBUG: Queue full. Semaphore spaces value: %d\n",
               sem_getvalue(&shared_mem->dbQueue.spaces, &sem_value));
        complete_transaction(transaction_id, -1);  // Mark as failed
        goto send_response;
    }

    // Wait for the operation to complete
    int result = wait_for_transaction(transaction_id);
    printf("DEBUG: Transaction %d completed with result %d\n",
           transaction_id, result);
    // Check if operation was successful
    if (result != -1) {
        // Success
        response.success = 1;
        response.balance = result;  // Updated balance
        sprintf(response.message, "Client%02d served.. BankID_%d", request->clientID,
                result);
    } else {
        // Failed
        response.success = 0;
        sprintf(response.message, "Client%02d something went WRONG",
                request->clientID);
    }

send_response:
    // Send the response through the FIFO
    int teller_fd = open(teller_fifo, O_WRONLY);
    if (teller_fd != -1) {
        write(teller_fd, &response, sizeof(response));
        close(teller_fd);
    }

    // Cleanup
    unlink(teller_fifo);
    return NULL;
}

// Add a command to the database queue
// Returns 1 on success, 0 on failure
int add_db_command(CommandQueue *queue, DatabaseCommand *cmd) {
    // Try to acquire the spaces semaphore
    if (sem_trywait(&queue->spaces) == -1) {
        if (errno == EAGAIN) {
            return 0;  // Queue is full
        }
        perror("Error waiting for spaces semaphore");
        return 0;  // Error occurred
    }

    // Acquire the mutex
    sem_wait(&queue->mutex);

    // Add the command to the queue
    queue->commands[queue->tail] = *cmd;
    queue->tail = (queue->tail + 1) % MAX_QUEUE_SIZE;
    queue->count++;

    // Release the mutex
    sem_post(&queue->mutex);

    // Signal that an item is available
    sem_post(&queue->items);

    return 1;  // Successfully added command
}

int get_db_command(CommandQueue *queue, DatabaseCommand *cmd) {
    if (sem_trywait(&queue->items) == -1) {
        if (errno == EAGAIN) {
            return 0;  // No items available
        }
        perror("Error waiting for items semaphore");
        return -1;
    }

    sem_wait(&queue->mutex);  // Lock the queue

    *cmd = queue->commands[queue->head];
    queue->head = (queue->head + 1) % MAX_QUEUE_SIZE;
    queue->count--;

    sem_post(&queue->mutex);   // Unlock the queue
    sem_post(&queue->spaces);  // Signal a space is available

    return 1;  // Successfully got a command
}

void process_database_operations() {
    DatabaseCommand cmd;

    // Process all commands in the queue
    while (get_db_command(&shared_mem->dbQueue, &cmd)) {
        int result = -1;
        int bankID = 0;

        // Process based on operation type
        if (cmd.operation == DEPOSIT) {
            result = process_deposit(&cmd, &bankID);
        } else if (cmd.operation == WITHDRAW) {
            result = process_withdraw(&cmd, &bankID);
        }

        // Complete the transaction with the result
        printf("DEBUG: ProcDB ts: %d with result %d\n",
               cmd.transaction_id, result);
               result = result == -1 ? -1 : bankID;
        complete_transaction(cmd.transaction_id, result);
    }
}

int process_deposit(DatabaseCommand *cmd, int *bankID) {
    char buffer[256];
    off_t position;
    int found = find_client_in_db(cmd->entry.bankName, &position, buffer,
                                  sizeof(buffer));

    // Check if it's a new account request
    if (strcmp(cmd->entry.bankName, "N") == 0) {
        // Create a new account with a unique ID
        *bankID = get_available_bank_name();
        sprintf(cmd->entry.bankName, "BankID_%02d",
                *bankID);  // 1 as 01

        DatabaseEntry new_entry;
        strcpy(new_entry.bankName, cmd->entry.bankName);
        new_entry.id = cmd->entry.id;
        new_entry.balance = cmd->entry.balance;

        int res = add_to_db(&new_entry);
        if (res != -1) {
            return res;  // Success
        }
        return -1;  // Failed
    } else if (found != -1) {
        // Account exists, update the balance
        DatabaseEntry existing_entry;
        sscanf(buffer, "%s %d %d", existing_entry.bankName, &existing_entry.id,
               &existing_entry.balance);
        existing_entry.balance += cmd->entry.balance;

        int res = update_db(&existing_entry);
        if (res != -1) {
            sscanf(existing_entry.bankName, "BankID_%d", bankID);
            return existing_entry.id;  // Success
        }
    }

    return -1;  // Failed
}

int process_withdraw(DatabaseCommand *cmd, int *bankID) {
    char buffer[256];
    off_t position;
    printf("DEBUG: Processing withdrawal for %s\n", cmd->entry.bankName);
    int found = find_client_in_db(cmd->entry.bankName, &position, buffer,
                                  sizeof(buffer));

    printf("DEBUG: Found client at position %d, bankname: %s\n", found,
           cmd->entry.bankName);
    if (found == -1) {
        printf("Account not found\n");
        return -1;  // Account not found
    }

    // Account exists, check balance
    DatabaseEntry existing_entry;
    sscanf(buffer, "%s %d %d", existing_entry.bankName, &existing_entry.id,
           &existing_entry.balance);

    if (existing_entry.balance < cmd->entry.balance) {
        return -1;  // Insufficient balance
    }

    // Sufficient balance, update or remove
    existing_entry.balance -= cmd->entry.balance;
    sscanf(existing_entry.bankName, "BankID_%d", bankID);
    printf("DEBUG: WITHDRAW - EE: %s\n", existing_entry.bankName);
    printf("DEBUG: WITHDRAW - BankID: %d\n", *bankID);

    if (existing_entry.balance == 0) {
        // Remove the account if balance is 0
        return remove_from_db(existing_entry.bankName);
    } else {
        // Update the account
        return update_db(&existing_entry);
    }
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

    sem_init(&shared_mem->dbQueue.mutex, 1, 1);  // Mutex with initial value 1
    sem_init(&shared_mem->dbQueue.items, 1,
             0);  // Items semaphore with initial value 0
    sem_init(&shared_mem->dbQueue.spaces, 1,
             MAX_QUEUE_SIZE);  // Spaces semaphore with i
}

void cleanup_shared_memory() {
    // Destroy the transaction manager
    for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
        sem_destroy(&shared_mem->transactionManager.transactions[i].sem);
    }
    sem_destroy(&shared_mem->transactionManager.mutex);

    // Destroy queue semaphores
    sem_destroy(&shared_mem->dbQueue.mutex);
    sem_destroy(&shared_mem->dbQueue.items);
    sem_destroy(&shared_mem->dbQueue.spaces);

    // Close and unlink other semaphores
    sem_close(sem_server);
    sem_close(sem_teller);
    sem_unlink("/sem_server");
    sem_unlink("/sem_teller");

    // Unmap and unlink shared memory
    munmap(shared_mem, sizeof(SharedMemory));
    shm_unlink(SHM_NAME);
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
    if (bankName == NULL || position == NULL || buffer == NULL) {
        fprintf(stderr, "Error: Invalid arguments to find_client_in_db\n");
        return -1;
    }

    int db_fd = open(DATABASE, O_RDONLY);
    if (db_fd == -1) {
        perror("Error opening database");
        return -1;
    }

    off_t offset = 0;
    ssize_t bytes_read;
    size_t leftover = 0;

    while ((bytes_read = read(db_fd, buffer + leftover,
                              buffer_size - leftover - 1)) > 0) {
        buffer[bytes_read + leftover] = '\0';  // Null-terminate the buffer
        char *line_start = buffer;
        char *newline_pos;

        while ((newline_pos = strchr(line_start, '\n')) != NULL) {
            *newline_pos = '\0';  // Null-terminate the current line

            // Check if the line contains the bankName
            if (strstr(line_start, bankName) != NULL) {
                *position = offset + (line_start - buffer);  // Set the position
                close(db_fd);
                strncpy(buffer, line_start,
                        buffer_size - 1);  // Copy the matched line to buffer
                buffer[buffer_size - 1] = '\0';  // Ensure null-termination
                return 0;                        // Success
            }

            line_start = newline_pos + 1;  // Move to the next line
        }

        // Handle leftover data (partial line at the end of the buffer)
        leftover = strlen(line_start);
        memmove(buffer, line_start, leftover);
        offset += bytes_read;
    }

    if (bytes_read == -1) {
        perror("Error reading database");
    }

    close(db_fd);
    return -1;  // Client not found
}

DatabaseEntry *get_client_from_db(const char *bankName) {
    char buffer[256];
    off_t position;
    int fd = find_client_in_db(bankName, &position, buffer, sizeof(buffer));
    if (fd == -1) {
        return NULL;  // Client not found
    }

    DatabaseEntry *entry = malloc(sizeof(DatabaseEntry));
    sscanf(buffer, "%s %d %d", entry->bankName, &entry->id, &entry->balance);
    close(fd);
    return entry;
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
    FILE *db_file = fopen(DATABASE, "a");
    if (db_file == NULL) {
        perror("Error opening database for appending");
        return -1;
    }

    // Write the entry in the format "bankName id balance"
    int result =
        fprintf(db_file, "%s %d %d\n", req->bankName, req->id, req->balance);
    fclose(db_file);

    if (result < 0) {
        perror("Error writing to database");
        return -1;
    }

    int ret;
    sscanf(req->bankName, "%*s_%d",
           &ret);  // Parse the entry back to get the ID
    printf("DEBUG: Added to db returnID %d\n", ret);
    return ret;  // Return ID on success
}

int update_db(DatabaseEntry *updated_client) {
    // Open a temporary file for writing
    char temp_file[256];
    sprintf(temp_file, "%s.tmp", DATABASE);
    FILE *temp = fopen(temp_file, "w");
    if (temp == NULL) {
        perror("Error creating temporary file");
        return -1;
    }

    // Open the original database file for reading
    FILE *db_file = fopen(DATABASE, "r");
    if (db_file == NULL) {
        perror("Error opening database");
        fclose(temp);
        unlink(temp_file);
        return -1;
    }

    char line[256];
    int found = 0;

    // Process each line in the database
    while (fgets(line, sizeof(line), db_file) != NULL) {
        char bank_name[64];
        int id, balance;

        // Parse the line
        if (sscanf(line, "%s %d %d", bank_name, &id, &balance) == 3) {
            // Check if this is the record we want to update
            if (strcmp(bank_name, updated_client->bankName) == 0) {
                // Write the updated record instead
                fprintf(temp, "%s %d %d\n", updated_client->bankName,
                        updated_client->id, updated_client->balance);
                found = 1;
            } else {
                // Write the original record
                fprintf(temp, "%s", line);
            }
        } else {
            // Write the line as is if it doesn't match our expected format
            fprintf(temp, "%s", line);
        }
    }

    // Close both files
    fclose(db_file);
    fclose(temp);

    // If the record was found and updated, replace the original file
    if (found) {
        if (rename(temp_file, DATABASE) != 0) {
            perror("Error replacing database file");
            unlink(temp_file);
            return -1;
        }
        printf("DEBUG: Updated record for %s\n", updated_client->bankName);
        int ret;
        sscanf(updated_client->bankName, "%*s_%d",
               &ret);  // Parse the entry back to get the ID
        printf("DEBUG: Updatedb returnID %d\n", ret);
        return ret;  // Return ID on success
    } else {
        // Record not found, remove the temporary file
        unlink(temp_file);
        return -1;
    }
}

int remove_from_db(const char *bankName) {
    // Open a temporary file for writing
    char temp_file[256];
    sprintf(temp_file, "%s.tmp", DATABASE);
    FILE *temp = fopen(temp_file, "w");
    if (temp == NULL) {
        perror("Error creating temporary file");
        return -1;
    }

    // Open the original database file for reading
    FILE *db_file = fopen(DATABASE, "r");
    if (db_file == NULL) {
        perror("Error opening database");
        fclose(temp);
        unlink(temp_file);
        return -1;
    }

    char line[256];
    int found = 0;
    int removed_id = -1;

    // Process each line in the database
    while (fgets(line, sizeof(line), db_file) != NULL) {
        char bank_name[64];
        int id, balance;

        // Parse the line
        if (sscanf(line, "%s %d %d", bank_name, &id, &balance) == 3) {
            // Check if this is the record we want to remove
            if (strcmp(bank_name, bankName) == 0) {
                found = 1;
                removed_id = id;
                // Skip this line (don't write it to the temp file)
            } else {
                // Write the record to the temp file
                fprintf(temp, "%s", line);
            }
        } else {
            // Write the line as is if it doesn't match our expected format
            fprintf(temp, "%s", line);
        }
    }

    // Close both files
    fclose(db_file);
    fclose(temp);

    // If the record was found and removed, replace the original file
    if (found) {
        if (rename(temp_file, DATABASE) != 0) {
            perror("Error replacing database file");
            unlink(temp_file);
            return -1;
        }
        int ret;
        sscanf(bankName, "%*s_%d", &ret);  // Parse the entry back to get the ID
        printf("DEBUG: Removed returnID %d\n", ret);
        return ret;  // Return ID on success
    } else {
        // Record not found, remove the temporary file
        unlink(temp_file);
        return -1;
    }
}