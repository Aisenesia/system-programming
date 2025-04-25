#ifndef COMMON_H
#define COMMON_H

#include <semaphore.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define MAX_CLIENTS 20
#define SERVER_FIFO "ServerFIFO_Name"
#define SHM_NAME "AdaBankSharedMemory"
#define LOG_FILE "logs/AdaBank.bankLog"
#define CLIENT_FIFO "client_fifo" 


#define MAX_QUEUE_SIZE 10

// Error codes
#define ERR_INVALID_REQUEST -1
#define ERR_FIFO_FAILURE -2
#define ERR_DATABASE_FAILURE -3

// Signal macros
#define SHUTDOWN_SIGNAL SIGINT


pid_t Teller(void *func, void *arg);
int waitTeller(pid_t pid, int* status);

typedef enum {
    DEPOSIT,
    WITHDRAW
} OperationType;

typedef struct {
    char bankName[32]; // bank
    int id;
    int balance;
} DatabaseEntry;

typedef struct {
    OperationType operation;
    DatabaseEntry entry; // Database entry for the operation
} DatabaseCommand;


typedef struct {
    DatabaseCommand commands[MAX_QUEUE_SIZE]; // Array of commands
    int head; // Index of the first command
    int tail; // Index of the next free slot
    int count; // Number of commands in the queue
    sem_t mutex; // Mutex for synchronization
    sem_t items; // Semaphore to track available items
    sem_t spaces; // Semaphore to track available spaces
} CommandQueue;



typedef struct {
    char bankName[32]; // can be "N", or BankID_01 etc..
    OperationType operation;
    int amount;
    int clientID;
} ClientRequest;



typedef struct {
    int success;
    int balance;
    char message[128];
} ServerResponse;


typedef struct {
    ClientRequest request;
    int processed; // 0 = not processed, 1 = processed
    int result;    // Result of the operation (e.g., success or failure)
    CommandQueue dbQueue; // Queue for database operations
} SharedMemory;


#endif
