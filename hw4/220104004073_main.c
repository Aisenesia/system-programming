#define _POSIX_C_SOURCE 200809L  // just for vscode to see, pthread_barrier_t
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>  // For sleep and random simulation

#include "buffer.h"
#define MAX_LINE_LENGTH 1024

// Function prototypes
void *managerThread(void *arg);
void *workerThread(void *arg);

// -------------------- Global Variables --------------------
int num_workers;
char *search_term;
SharedBuffer sharedBuffer;
pthread_barrier_t barrier;
volatile int total_matches = 0;
pthread_mutex_t match_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int eof_reached = 0;

// -------------------- Function Declarations --------------------
void handleSigint(int sig);

// -------------------- Main Function --------------------
int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf(
            "Usage: ./LogAnalyzer <buffer_size> <num_workers> <log_file> "
            "<search_term>\n");
        return 1;
    }

    int buffer_size = atoi(argv[1]);
    num_workers = atoi(argv[2]);
    char *log_file = argv[3];
    search_term = argv[4];

    if (buffer_size <= 0 || num_workers <= 0) {
        printf(
            "Error: buffer_size and num_workers must be positive integers.\n");
        return 1;
    }

    // Initialize shared buffer and barrier
    initBuffer(&sharedBuffer, buffer_size);
    pthread_barrier_init(&barrier, NULL,
                         num_workers + 1);  // +1 for main thread

    // Set up signal handler
    signal(SIGINT, handleSigint);

    // Create manager and worker threads
    pthread_t manager;
    pthread_t workers[num_workers];

    if (pthread_create(&manager, NULL, managerThread, log_file) != 0) {
        perror("Failed to create manager thread");
        return 1;
    }

    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&workers[i], NULL, workerThread, (void *)(long)i) !=
            0) {
            perror("Failed to create worker thread");
            return 1;
        }
    }

    // Wait for all worker threads to finish processing
    pthread_barrier_wait(&barrier);

    // Print final summary
    printf("All workers have finished. Total matches found: %d\n",
           total_matches);

    // Now wait for threads to complete
    pthread_join(manager, NULL);
    for (int i = 0; i < num_workers; i++) {
        pthread_join(workers[i], NULL);
    }

    // Destroy resources
    destroyBuffer(&sharedBuffer);
    pthread_barrier_destroy(&barrier);
    pthread_mutex_destroy(&match_mutex);

    return 0;
}

// Manager thread function
void *managerThread(void *arg) {
    char *log_file = (char *)arg;
    FILE *file = fopen(log_file, "r");
    if (!file) {
        perror("Failed to open log file");
        pthread_exit(NULL);
    }

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), file)) {
        char *line_copy = strdup(line);
        if (!line_copy) {
            perror("Memory allocation failed");
            continue;
        }
        addToBuffer(&sharedBuffer, line_copy);
    }

    // Mark EOF for all workers by adding NULL markers for each worker
    for (int i = 0; i < num_workers; i++) {
        addToBuffer(&sharedBuffer, NULL);
    }

    eof_reached = 1;
    fclose(file);
    pthread_exit(NULL);
}

// Worker thread function
void *workerThread(void *arg) {
    int id = (int)(long)arg;
    int match_count = 0;

    printf("Worker %d started.\n", id);

    while (1) {
        char *line = removeFromBuffer(&sharedBuffer);
        if (line == NULL) {
            // Found EOF marker
            break;
        }

        if (strstr(line, search_term)) {
            match_count++;
        }
        free(line);
    }

    // Update the total match count
    pthread_mutex_lock(&match_mutex);
    total_matches += match_count;
    pthread_mutex_unlock(&match_mutex);

    printf("Worker %d found %d matches.\n", id, match_count);

    // Wait at the barrier to synchronize with main
    pthread_barrier_wait(&barrier);

    pthread_exit(NULL);
}

// Signal handler function
void handleSigint(int sig) {
    printf("\nCaught signal %d. Exiting...\n", sig);
    destroyBuffer(&sharedBuffer);
    pthread_barrier_destroy(&barrier);
    pthread_mutex_destroy(&match_mutex);
    exit(0);
}