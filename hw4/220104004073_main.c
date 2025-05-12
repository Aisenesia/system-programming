#include "buffer.h"

#define DEBUG 0  // Set to 1 to enable debug logs, 0 to disable

#if DEBUG
#define DEBUG_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)  // No-op
#endif

// Function prototypes
void *managerThread(void *arg);
void *workerThread(void *arg);

// -------------------- Global Variables --------------------
int num_workers;
char *search_term;
SharedBuffer sharedBuffer;
pthread_barrier_t barrier;
volatile unsigned long total_matches = 0;
pthread_mutex_t match_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int eof_reached = 0;

volatile sig_atomic_t terminate = 0;  // Flag to indicate termination

pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
int workers_ready = 0;

// Initialize workers pointer to NULL
pthread_t *workers = NULL;
pthread_t manager;

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

    // Validate numeric arguments (what if they are not integers?, negative?)
    char *endptr;

    // Validate buffer_size
    long buffer_size = strtol(argv[1], &endptr,
                              10);  // not more than 10 characters (1000000000)
    if (*endptr != '\0' || buffer_size <= 0) {
        printf("Error: buffer_size must be a positive integer.\n");
        return 1;
    }

    // Validate num_workers
    long num_workers_long = strtol(argv[2], &endptr, 10);
    if (*endptr != '\0' || num_workers_long <= 0) {
        printf("Error: num_workers must be a positive integer.\n");
        return 1;
    }

    num_workers = (int)num_workers_long;

    char *log_file = argv[3];
    search_term = argv[4];

    if (buffer_size <= 0 || num_workers <= 0) {
        printf(
            "Error: buffer_size and num_workers must be positive integers.\n");
        return 1;
    }

    // Open the log file, i do not do it in the manager thread since manager can
    // run after the worker, which causes a lot of problems and edge cases and i
    // do not want to deal with them

    int fd = open(log_file, O_RDONLY);
    if (fd == -1) {
        perror("Failed to open log file");
        return 1;
    }

    // Initialize shared buffer and barrier
    initBuffer(&sharedBuffer, buffer_size);
    pthread_barrier_init(&barrier, NULL,
                         num_workers + 1);  // +1 for main thread

    // Set up signal handler
    struct sigaction sa;
    sa.sa_handler = handleSigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    // Create manager and worker threads
    workers = malloc(num_workers * sizeof(pthread_t));

    if (pthread_create(&manager, NULL, managerThread, (void *)(long)fd) != 0) {
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

    pthread_mutex_lock(&start_mutex);
    while (workers_ready < num_workers) {
        pthread_cond_wait(&start_cond, &start_mutex);
    }
    pthread_mutex_unlock(&start_mutex);

    DEBUG_PRINT("[DEBUG] Main thread: Waiting at barrier.\n");
    pthread_barrier_wait(&barrier);
    DEBUG_PRINT("[DEBUG] Main thread: Passed the barrier.\n");

    // Print final summary
    if (terminate)
        printf(
            "Termination signal received, workers may not have completed. "
            "Current Total Matches: %ld\n",
            total_matches);

    else
        printf("All workers have finished. Total matches found: %ld\n",
               total_matches);

    // Now wait for threads to complete
    DEBUG_PRINT("[DEBUG] Main thread: Waiting for manager to finish.\n");
    pthread_join(manager, NULL);
    DEBUG_PRINT("[DEBUG] Main thread: Manager has finished.\n");
    for (int i = 0; i < num_workers; i++) {
        DEBUG_PRINT("[DEBUG] Main thread: Waiting for worker %d to finish.\n",
                    i);
        pthread_join(workers[i], NULL);
        DEBUG_PRINT("[DEBUG] Main thread: Worker %d has finished.\n", i);
    }

    // Destroy resources
    DEBUG_PRINT("[DEBUG] Main thread: Destroying shared buffer.\n");
    destroyBuffer(&sharedBuffer);
    pthread_barrier_destroy(&barrier);
    DEBUG_PRINT("[DEBUG] Main thread: Destroying mutex.\n");
    pthread_mutex_destroy(&match_mutex);
    free(workers);

    return 0;
}

// Manager thread function
void *managerThread(void *arg) {
    int fd = (int)(long)arg;

    char buffer[MAX_LINE_LENGTH];
    char *line_start = buffer;
    ssize_t bytes_read;

    while ((bytes_read = read(fd, line_start, 1)) > 0) {
        if (terminate) {
            DEBUG_PRINT("[DEBUG] Manager: Termination signal received.\n");
            break;
        }

        if (*line_start == '\n' || bytes_read == MAX_LINE_LENGTH - 1) {
            *line_start = '\0';  // Null-terminate the line
            char *line_copy = strdup(buffer);
            if (!line_copy) {
                perror("Memory allocation failed");
                continue;
            }

            addToBuffer(&sharedBuffer, line_copy);
            line_start = buffer;  // Reset for the next line
        } else {
            line_start++;
        }
    }

    if (bytes_read == -1) {
        perror("Error reading from file descriptor");
    }

    // Ensure all EOF markers are added even if terminate is set
    for (int i = 0; i < num_workers; i++) {
        if (terminate) {
            DEBUG_PRINT(
                "[DEBUG] Manager: Adding EOF marker for worker %d due to "
                "termination.\n",
                i);
        } else {
            DEBUG_PRINT("[DEBUG] Manager: Adding EOF marker for worker %d.\n",
                        i);
        }
        addToBuffer(&sharedBuffer, NULL);
    }

    // Add debug logs to track when the manager thread finishes
    DEBUG_PRINT("[DEBUG] Manager thread: Finished processing log file.\n");

    close(fd);
    pthread_exit(NULL);
}

// Worker thread function
void *workerThread(void *arg) {
    pthread_mutex_lock(&start_mutex);
    workers_ready++;
    if (workers_ready == num_workers) {
        pthread_cond_signal(&start_cond);
    }
    pthread_mutex_unlock(&start_mutex);

    int id = (int)(long)arg;
    int match_count = 0;

    printf("Worker %d started.\n", id);

    // Add debug statements to track thread progress
    DEBUG_PRINT("[DEBUG] Worker %d: Starting thread.\n", id);

    while (1) {
        if (terminate) {
            DEBUG_PRINT("[DEBUG] Worker %d: Termination signal received.\n",
                        id);
            break;
        }

        DEBUG_PRINT("[DEBUG] Worker %d: Attempting to remove from buffer.\n",
                    id);
        char *line = removeFromBuffer(&sharedBuffer);

        if (line == NULL) {
            DEBUG_PRINT("[DEBUG] Worker %d: EOF marker found. Exiting loop.\n",
                        id);
            break;
        }

        DEBUG_PRINT("[DEBUG] Worker %d: Processing line.\n", id);
        if (strstr(line,
                   search_term)) {  // this wont detect multiple matches, but im
                                    // not sure whether that is what was wanted.
            match_count++;
        }
        free(line);
    }

    DEBUG_PRINT("[DEBUG] Worker %d: Finished processing. Matches found: %d.\n",
                id, match_count);

    // Update the total match count
    pthread_mutex_lock(&match_mutex);
    total_matches += match_count;
    pthread_mutex_unlock(&match_mutex);

    printf("Worker %d found %d matches.\n", id, match_count);

    DEBUG_PRINT("[DEBUG] Worker %d: Waiting at barrier.\n", id);
    pthread_barrier_wait(&barrier);
    DEBUG_PRINT("[DEBUG] Worker %d: Passed the barrier.\n", id);

    pthread_exit(NULL);
}

// Signal handler function
void handleSigint(int sig) { terminate = 1; }
