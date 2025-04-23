#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>
#include "common.h"

// Shared memory and semaphore
SharedMemory *shared_mem = NULL;
sem_t *sem_server = NULL;
sem_t *sem_teller = NULL;

// Teller function to process requests
void teller() {
    while (1) {
        sem_wait(sem_teller); // Wait for a request from the server

        // Read request from shared memory
        ClientRequest *req = &shared_mem->request;

        printf("Teller PID%02d is processing request for client ID: %d (BankID: %s)\n", getpid(), req->clientID, req->clientName);

        // Simulate processing (e.g., validate request)
        sleep(1);

        // If it's a new account, notify the server to add an entry to the database
        if (req->op == DEPOSIT) {
            printf("Requesting server to add/update entry for BankID: %s with deposit of %d credits\n", req->clientName, req->amount);
        } else if (req->op == WITHDRAW) {
            printf("Requesting server to process withdrawal of %d credits for BankID: %s\n", req->amount, req->clientName);
        }

        // Mark the request as processed and notify the server
        shared_mem->processed = 1;
        sem_post(sem_server);
    }
}

// Waits for a Teller process to finish
int wait_teller(pid_t pid, int *status) {
    return waitpid(pid, status, 0); // Wait for the specific child process
}

int main() {
    // Open the shared memory object
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Error opening shared memory");
        exit(1);
    }

    // Map the shared memory
    shared_mem = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_mem == MAP_FAILED) {
        perror("Error attaching to shared memory");
        exit(1);
    }

    // Attach to semaphores
    sem_server = sem_open("/sem_server", 0);
    sem_teller = sem_open("/sem_teller", 0);
    if (sem_server == SEM_FAILED || sem_teller == SEM_FAILED) {
        perror("Error attaching to semaphores");
        exit(1);
    }

    // Run the teller function
    teller();

    return 0;
}