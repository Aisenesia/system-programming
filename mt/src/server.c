#include <ctype.h>  // isdigit
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>    //
#include <sys/mman.h>  // shared memory
#include <sys/select.h>  // select, fd_set, in main loop to do non-blocking server op
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>  // log timestamps
#include <unistd.h>

#include "common.h"  // Include the Teller library

#define DATABASE "files/adabank.db"

#define DEBUG_MODE 0

// Function prototypes
void handle_signal(int sig);
void cleanup_and_exit();
pid_t teller_function(ClientRequest *request);  // Teller function
void setup_shared_memory();
void cleanup_shared_memory();

// Database operations

int add_to_db(DatabaseEntry entry);
int update_db(DatabaseEntry entry, char OP, int amount);
int remove_from_db(const char *bankName);
int find_client_in_db(const char *bankName, off_t *position, char *buffer,
                      size_t bufSize);
int get_available_bank_name();
DatabaseEntry get_client_from_db(const char *bankName);
int get_balance(const char *line, int *pos);
int update_db_timestamp(int fd);

void process_database_operations();

void setup_transaction_manager();
int process_deposit(DatabaseCommand *cmd, int *bankID, int *status);
int process_withdraw(DatabaseCommand *cmd, int *bankID, int *status);
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
        off_t position;
        char buffer[256];

        int res =
            find_client_in_db("BankID_02", &position, buffer, sizeof(buffer));
        if (res == 0) {
            printf("Found client: %s\n", buffer);
        } else {
            printf("Client not found\n");
        }

        DatabaseEntry entry = get_client_from_db("BankID_02");
        if (entry.id != -1) {
            printf("Client found: %s, Balance: %d\n", entry.bankName,
                   entry.balance);
        } else {
            printf("Client not found\n");
        }

        return 0;
    }
    setup_shared_memory();
    setup_transaction_manager();  // Initialize transaction system

    // Register signal handler
    signal(SHUTDOWN_SIGNAL, handle_signal);
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE signal

    // Open the debug file

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

        printf("- Received %d clients from %s\n", number_of_clients,
               CLIENT_FIFO);  // me when i spread misinformation

        // Process each client request
        for (int i = 0; i < number_of_clients; i++) {
            ClientRequest request;
            if (read(server_fifo_fd, &request, sizeof(request)) == -1) {
                perror("Error reading from server FIFO");
                break;
            }

            printf("-- Teller PID%d is active serving Client%02d\n",
                   teller_function(&request), request.clientID);
            teller_id_giver++;
        }
        printf("...\n");

        close(server_fifo_fd);

        // Check again for database operations before next loop iteration
        process_database_operations();
        printf("...\n");
        printf("Waiting for clients @%s…\n", SERVER_FIFO);
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
        if (shared_mem->transactionManager.transactions[i].completed && shared_mem->transactionManager.transactions[i].result == 0) {
            shared_mem->transactionManager.transactions[i].completed = 0;
            shared_mem->transactionManager.transactions[i].result =
                -2;  // Pending
                printf("DEBUG: Allocating transaction %d\n", i);
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

    printf("DEBUG: Completing transaction %d with result %d\n",
           transaction_id, result);

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
    shared_mem->transactionManager.transactions[transaction_id].result = 0;

    return result;
}

// teller main function

pid_t teller_function(ClientRequest *request) {
    // Determine the operation type, call Teller(deposit/withdraw)

    if (request->operation == DEPOSIT) {
        // Create a new process for deposit operation
        pid_t teller_pid = Teller(deposit, request);
        if (teller_pid == -1) {
            perror("Error creating deposit teller process");
            return -1;
        }
        // waitTeller(teller_pid, NULL);  // Wait for the teller process to
        // finish
        return teller_pid;
    } else if (request->operation == WITHDRAW) {
        // Create a new process for withdraw operation
        pid_t teller_pid = Teller(withdraw, request);
        if (teller_pid == -1) {
            perror("Error creating withdraw teller process");
            return -1;
        }
        // waitTeller(teller_pid, NULL);  // Wait for the teller process to
        // finish
        return teller_pid;
    } else {
        perror("Error: Invalid operation type\n");
        return -1;  // Invalid operation
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

    if (result == -2) {
        printf("DEBUG: Transaction %d is still pending\n",
               transaction_id);  // shouldnt happen afaik
    }

    // Check if operation was successful
    if (result != -1) {
        // Success
        response.success = 1;
        // from where to get the bankID?
        sprintf(response.message, "Client%02d served.. BankID_%02d",
                request->clientID, result);
    } else {
        // Failed
        response.success = 0;
        sprintf(response.message, "Client%02d something went WRONG",
                request->clientID);
    }

send_response:;
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
        sprintf(response.message, "Client%02d served.. BankID_%02d",
                request->clientID, result);
    } else {
        // Failed
        response.success = 0;
        sprintf(response.message, "Client%02d something went WRONG",
                request->clientID);
    }

send_response:;  // empty statement, C sucks
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
        int status = -1;  // to check if the account is removed or not etc

        // Process based on operation type
        if (cmd.operation == DEPOSIT) {
            result = process_deposit(&cmd, &bankID, &status);
        } else if (cmd.operation == WITHDRAW) {
            result = process_withdraw(&cmd, &bankID, &status);
        }

        char buffer[256];
        sprintf(buffer, "Client%02d %s %d credits…", cmd.entry.id,
                cmd.operation == DEPOSIT ? "deposits" : "withdraws",
                cmd.entry.balance);
        printf("%s", buffer);
        if (result == -1) {
            printf("operation not permitted.");
        } else {
            printf("updating log...");
        }

        if (status == STATUS_DELETED) {
            printf("Bye Client%02d", cmd.entry.id);
        }

        printf("\n");  // here if the operation caused the account to be
                       // removed, printt Bye Client01

        // Complete the transaction with the result

        result = result == -1 ? -1 : bankID;
        complete_transaction(cmd.transaction_id, result);
    }
}

int process_deposit(DatabaseCommand *cmd, int *bankID, int *status) {
    char buffer[256];
    off_t position;
    int found = find_client_in_db(cmd->entry.bankName, &position, buffer,
                                  sizeof(buffer));

    // Check if it's a new account request
    if (strcmp(cmd->entry.bankName, "N") == 0) {
        *status = STATUS_NEW;
        // Create a new account with a unique ID
        *bankID = get_available_bank_name();
        sprintf(cmd->entry.bankName, "BankID_%02d",
                *bankID);  // 1 as 01

        DatabaseEntry new_entry;
        strcpy(new_entry.bankName, cmd->entry.bankName);
        new_entry.id = cmd->entry.id;
        new_entry.balance = cmd->entry.balance;

        int res = add_to_db(new_entry);
        if (res != -1) {
            return res;  // Success
        }
        return -1;  // Failed
    } else if (found != -1) {
        *status = STATUS_EXISTING;
        // Account exists, update the balance
        DatabaseEntry existing_entry;
        sscanf(buffer, "%s %d %d", existing_entry.bankName, &existing_entry.id,
               &existing_entry.balance);
        existing_entry.balance += cmd->entry.balance;

        int res = update_db(existing_entry, 'D', cmd->entry.balance);
        if (res != -1) {
            sscanf(existing_entry.bankName, "BankID_%d", bankID);
            return existing_entry.id;  // Success
        }
    }

    return -1;  // Failed
}

int process_withdraw(DatabaseCommand *cmd, int *bankID, int *status) {
    DatabaseEntry existing_entry = get_client_from_db(cmd->entry.bankName);
    int found = existing_entry.id;

    if (found == -1) {
        return -1;  // Account not found
    }

    // Account exists, check balance

    if (existing_entry.balance < cmd->entry.balance) {
        return -1;  // Insufficient balance
    }

    // Sufficient balance, update or remove
    existing_entry.balance -= cmd->entry.balance;
    sscanf(existing_entry.bankName, "BankID_%d", bankID);

    if (existing_entry.balance == 0) {
        // Remove the account if balance is 0
        *status = STATUS_DELETED;
        char bankName[32];
        strcpy(bankName, existing_entry.bankName);
        return remove_from_db(bankName);
    } else {
        // Update the account
        *status = STATUS_EXISTING;
        return update_db(existing_entry, 'W', cmd->entry.balance);
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
/*
DB Structure:
# Adabank Log file updated @10:37 April 18 2025 // first line is timestamp, #
are comments # BankID_01 D 300 W 300 0 // there has been a deposit of 300 and a
withdraw of 300, resulting in a balance of 0 and causing the account to be
removed, thus it is commented out BankID_02 D 2000 2000 // there has been a
deposit of 200 and no withdraw, resulting in a balance of 2000 BankID_03 D 20 20
// there has been a deposit of 20 and no withdraw, resulting in a balance of 20

*/

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

    off_t offset = 0;  // Tracks the total offset in the file
    ssize_t bytes_read;
    size_t leftover = 0;

    while ((bytes_read = read(db_fd, buffer + leftover,
                              buffer_size - leftover - 1)) > 0) {
        buffer[bytes_read + leftover] = '\0';  // Null-terminate the buffer
        char *line_start = buffer;
        char *newline_pos;

        while ((newline_pos = strchr(line_start, '\n')) != NULL) {
            *newline_pos = '\0';  // Null-terminate the current line

            // Skip comment lines (lines starting with #)
            if (line_start[0] != '#' &&
                strstr(line_start, bankName) == line_start) {
                // We found the bank ID at the start of a non-comment line
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
        offset += bytes_read - leftover;  // Update offset to exclude leftover
    }

    if (bytes_read == -1) {
        perror("Error reading database");
    }

    close(db_fd);
    return -1;  // Client not found
}
DatabaseEntry get_client_from_db(const char *bankName) {
    char buffer[256];
    off_t position;
    DatabaseEntry entry;
    int fd = find_client_in_db(bankName, &position, buffer, sizeof(buffer));
    if (fd == -1) {
        entry.id = -1;
        return entry;  // Client not found
    }

    // line format: "bankName D int1 int2 int3... W int1 int2 int3... balance",
    // indeterminate amount of ints Parse the entry

    sscanf(buffer, "%s", entry.bankName);

    // Find the last digit in the string by traversing backward
    int len = strlen(buffer);
    int i = len - 1;

    // Skip non-digit characters from the end
    while (i >= 0 && !isdigit(buffer[i])) {
        i--;
    }

    // Find the start of the last number
    int end = i;
    while (i >= 0 && isdigit(buffer[i])) {
        i--;
    }

    // Extract the last number (balance)
    if (end >= 0) {
        sscanf(&buffer[i + 1], "%d", &entry.balance);
    } else {
        entry.balance = 0;  // Default if no number is found
    }

    // Print the results
    close(fd);
    return entry;
}

int get_available_bank_name() {
    int db_fd = open(DATABASE, O_RDONLY);
    if (db_fd == -1) {
        perror("Error opening database");
        return 1;  // Default to 1 if the file can't be opened
    }

    int last_bank_id = 0;
    char buffer[256];
    ssize_t bytes_read;
    size_t leftover = 0;

    while ((bytes_read = read(db_fd, buffer + leftover,
                              sizeof(buffer) - leftover - 1)) > 0) {
        buffer[bytes_read + leftover] = '\0';  // Null-terminate the buffer
        char *line_start = buffer;
        char *newline_pos;

        while ((newline_pos = strchr(line_start, '\n')) != NULL) {
            *newline_pos = '\0';  // Null-terminate the current line

            // Skip comment lines (lines starting with #)
            if (line_start[0] != '#') {
                int current_id = 0;
                if (sscanf(line_start, "BankID_%d", &current_id) == 1) {
                    if (current_id > last_bank_id) {
                        last_bank_id = current_id;  // Update the last bank ID
                    }
                }
            }

            line_start = newline_pos + 1;  // Move to the next line
        }

        // Handle leftover data (partial line at the end of the buffer)
        leftover = strlen(line_start);
        memmove(buffer, line_start, leftover);
    }

    if (bytes_read == -1) {
        perror("Error reading database");
    }

    close(db_fd);
    return last_bank_id + 1;  // Return the next available bank ID
}
int add_to_db(DatabaseEntry req) {
    int db_fd = open(DATABASE, O_WRONLY | O_APPEND);
    if (db_fd == -1) {
        perror("Error opening database");
        return -1;
    }
    // Write the entry in the format "bankName D balance"
    char line[256];
    sprintf(line, "%s D %d %d\n", req.bankName, req.balance,
            req.balance);  // since its add, it can only have a deposit
    ssize_t result = write(db_fd, line, strlen(line));
    if (result == -1) {
        perror("Error writing to database");
        close(db_fd);
        return -1;
    }
    close(db_fd);  // Close the database file
    return 0;      // Return 0
}

int update_db(DatabaseEntry entry, char OP, int amount) {
    if (OP != 'D' && OP != 'W') {
        fprintf(stderr, "Error: Invalid operation '%c'\n", OP);
        return -1;
    }

    char *bankName = entry.bankName;
    char buffer[256];
    off_t position;
    int found = find_client_in_db(bankName, &position, buffer, sizeof(buffer));

    if (found == -1) {
        fprintf(stderr, "Error: Entry not found for %s\n", bankName);
        return -1;  // Entry not found
    }

    int d_pos = -1;
    int w_pos = -1;
    int comment_pos = -1;

    for (int i = 0; i < sizeof(buffer); i++) {
        if (buffer[i] == '#') {
            comment_pos = i;
            buffer[i] = '\0';
            break;
        } else if (buffer[i] == 'D') {
            d_pos = i;
        } else if (buffer[i] == 'W') {
            w_pos = i;
        }
    }

    // Parse the existing entry
    char working_buffer[256];
    strncpy(working_buffer, buffer, sizeof(working_buffer) - 1);
    working_buffer[sizeof(working_buffer) - 1] = '\0';

    char *tokens[64];  // Array to hold pointers to tokens
    int token_count = 0;
    char *token = strtok(working_buffer, " \t\n");

    while (token != NULL && token_count < 64) {
        tokens[token_count++] = token;
        token = strtok(NULL, " \t\n");
    }

    if (token_count < 2) {
        fprintf(stderr, "Error: Invalid entry format\n");
        return -1;
    }

    // Extract the balance
    int balance_pos = 0;
    int balance = get_balance(buffer, &balance_pos);

    // Update the balance
    if (OP == 'D') {
        balance += amount;
    } else if (OP == 'W') {
        if (amount > balance) {
            fprintf(stderr, "Error: Insufficient balance for withdrawal\n");
            return -1;
        }
        balance -= amount;
    }

    // Build the updated entry
    char updated_entry[256];
    int pos = 0;

    // Start with the bank ID
    pos += snprintf(updated_entry + pos, sizeof(updated_entry) - pos, "%s",
                    tokens[0]);

    // Copy all the middle tokens (transaction history)
    for (int i = 1; i < token_count - 1; i++) {
        pos += snprintf(updated_entry + pos, sizeof(updated_entry) - pos, " %s",
                        tokens[i]);
    }

    char comment[256];
    if (comment_pos != -1) {
        // Copy the comment part
        comment[0] = '#';
        strcpy(comment + 1, buffer + comment_pos + 1);
        comment[sizeof(comment) - 1] = '\0';
    } else {
        comment[0] = '\0';  // No comment
    }

    // Add new transaction
    int c_pos = OP == 'D' ? d_pos : w_pos;
    if (c_pos == -1) {
        pos += snprintf(updated_entry + pos, sizeof(updated_entry) - pos,
                        " %c %d", OP, amount);
    } else {
        // if operation is deposit add it just before the w_pos
        if (OP == 'D') {
            char rest[256];
            strncpy(rest, updated_entry + w_pos, sizeof(updated_entry) - w_pos);
            pos +=
                snprintf(updated_entry + w_pos, sizeof(updated_entry) - w_pos,
                         "%d %s", amount, rest);
            pos = strlen(updated_entry);
        } else {
            // if operation is withdraw we can add it after the pos.
            pos += snprintf(updated_entry + pos, sizeof(updated_entry) - pos,
                            " %c %d", OP, amount);
        }
    }
    // Add updated balance
    pos += snprintf(updated_entry + pos, sizeof(updated_entry) - pos, " %d",
                    balance);

    // Add the comment if it exists
    if (comment_pos != -1) {
        pos += snprintf(updated_entry + pos, sizeof(updated_entry) - pos, " %s",
                        comment);
    }
    pos += snprintf(updated_entry + pos, sizeof(updated_entry) - pos, "\n");

    // Ensure null-termination
    updated_entry[sizeof(updated_entry) - 1] = '\0';

    // Rewrite the entire file
    FILE *db_file = fopen(DATABASE, "r");
    if (db_file == NULL) {
        perror("Error opening database for reading");
        return -1;
    }

    FILE *temp_file = fopen("temp.db", "w");
    if (temp_file == NULL) {
        perror("Error creating temporary file");
        fclose(db_file);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), db_file) != NULL) {
        // Check if this is the line to update
        if (strstr(line, bankName) == line) {
            fprintf(temp_file, "%s", updated_entry);  // Write the updated entry
        } else {
            fprintf(temp_file, "%s", line);  // Write the original line
        }
    }

    fclose(db_file);
    fclose(temp_file);

    // Replace the original file with the updated file
    if (rename("temp.db", DATABASE) == -1) {
        perror("Error replacing database file");
        return -1;
    }

    return 0;  // Success
}
int get_balance(const char *line, int *pos) {
    int balance = 0;

    // Find the last digit in the string by traversing backward
    int len = strlen(line);
    int i = len - 1;

    // Skip non-digit characters from the end
    while (i >= 0 && !isdigit(line[i])) {
        i--;
    }

    // Find the start of the last number
    int end = i;
    while (i >= 0 && isdigit(line[i])) {
        i--;
    }

    // Extract the last number (balance)
    if (end >= 0) {
        sscanf(&line[i + 1], "%d", &balance);
        *pos = i + 1;  // Set the position to the start of the last number
    } else {
        balance = 0;  // Default if no number is found
    }
    return balance;
}

int remove_from_db(const char *bankName) {
    if (bankName == NULL) {
        fprintf(stderr, "Error: bankName is NULL\n");
        return -1;
    }

    // Open the database file for reading
    FILE *db_file = fopen(DATABASE, "r");
    if (db_file == NULL) {
        perror("Error opening database for reading");
        return -1;
    }

    // Open a temporary file for writing
    FILE *temp_file = fopen("temp.db", "w");
    if (temp_file == NULL) {
        perror("Error creating temporary file");
        fclose(db_file);
        return -1;
    }

    char line[256];
    int found = 0;

    // Read the database line by line
    while (fgets(line, sizeof(line), db_file) != NULL) {
        // Check if this is the line to remove
        if (strstr(line, bankName) == line) {
            // Comment out the line
            fprintf(temp_file, "# %s", line);
            found = 1;
        } else {
            // Write the original line
            fprintf(temp_file, "%s", line);
        }
    }

    fclose(db_file);
    fclose(temp_file);

    if (!found) {
        // If no matching record was found, remove the temporary file
        unlink("temp.db");
        fprintf(stderr, "Error: Entry not found for %s\n", bankName);
        return -1;  // Entry not found
    }

    // Replace the original file with the updated file
    if (rename("temp.db", DATABASE) == -1) {
        perror("Error replacing database file");
        return -1;
    }

    return 0;  // Success
}