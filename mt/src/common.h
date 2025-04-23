#ifndef COMMON_H
#define COMMON_H

#define MAX_CLIENTS 20
#define SERVER_FIFO "ServerFIFO_Name"
#define SHM_NAME "/AdaBankSharedMemory"
#define LOG_FILE "logs/AdaBank.bankLog"



// Error codes
#define ERR_INVALID_REQUEST -1
#define ERR_FIFO_FAILURE -2
#define ERR_DATABASE_FAILURE -3

// Signal macros
#define SHUTDOWN_SIGNAL SIGINT

// Teller function prototype
typedef void (*TellerFunc)(void *arg); // Function pointer type for Teller

pid_t Teller(TellerFunc func, void *arg); // Creates a Teller process
int waitTeller(pid_t pid, int *status);   // Waits for a Teller process to finish


typedef enum {
    DEPOSIT,
    WITHDRAW
} OperationType;

typedef struct {
    char clientName[32];
    OperationType op;
    int amount;
    int clientID;
} ClientRequest;

typedef struct {
    char clientName[32];
    int id;
    int balance;
} DatabaseEntry;

typedef struct {
    int success;
    int balance;
    char message[128];
} ServerResponse;

typedef struct {
    ClientRequest request;
    int processed; // 0 = not processed, 1 = processed
    int result;    // Result of the operation (e.g., success or failure)
} SharedMemory;

#endif
