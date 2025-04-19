// satellite_engineer.c

#define _POSIX_C_SOURCE 200809L  // just for vscode to see, CLOCK_REALTIME

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>  // For sleep and random simulation

// -------------------- Constants --------------------

// Number of engineers available
#define NUM_ENGINEERS 3

#define BASE_INCREMENT \
    16  // Sattellite heap is held in well, heap and when full its size is
        // incremented by this
#define MAX_SATELLITES 1024
// Number of satellites
#define NUM_SATELLITES 5

// Maximum connection window for satellites (in seconds)
#define MAX_CONNECTION_WINDOW 5

// -------------------- Data Structures --------------------

// Structure to represent a satellite request
typedef struct {
    int satellite_id;     // Unique ID of the satellite
    int priority;         // Priority of the satellite
    time_t arrival_time;  // Time when the request was made (for timeout checks)
} SatelliteRequest;

typedef struct {
    SatelliteRequest *items;
    int size;
    int max;
} PriorityQueue;

// -------------------- Global Variables --------------------

// Counter for available engineers
__sig_atomic_t availableEngineers = NUM_ENGINEERS;

// Priority queue to manage satellite requests (simulate manually)
PriorityQueue requestQueue;  //
int queueSize = 0;           // Current number of satellites in the queue

// Mutex to protect shared resources
pthread_mutex_t engineerMutex;

// Semaphores for synchronization
sem_t newRequest;      // Signaled by satellites when they create a new request
sem_t requestHandled;  // Signaled by engineers once they pick up a satellite
                       // request

// Thread arrays
pthread_t engineerThreads[NUM_ENGINEERS];
pthread_t satelliteThreads[NUM_SATELLITES];

// -------------------- Function Declarations --------------------

// Function for engineer threads
void *engineer(void *arg);

// Function for satellite threads
void *satellite(void *arg);

// Helper to insert satellite into priority queue
void insertSatelliteIntoQueue(SatelliteRequest *request);

// Helper to debug heap
void printHeap();

// Helper to get the highest priority satellite from the queue
SatelliteRequest *getHighestPrioritySatellite();

// Helper to simulate satellite timeout checking
int hasTimedOut(SatelliteRequest *request);

void increment();  // Increment the size of the queue
void swap(SatelliteRequest *a, SatelliteRequest *b);  // Swap two requests

// Initialize the priority queue
void initializePriorityQueue();

// Define swap function to swap two SatelliteRequest objects
void swap(SatelliteRequest *a, SatelliteRequest *b) {
    SatelliteRequest temp = *a;
    *a = *b;
    *b = temp;
}

// Define heapifyUp function to maintain heap property during insertion
void heapifyUp(PriorityQueue *pq, int index) {
    if (index &&
        pq->items[(index - 1) / 2].priority < pq->items[index].priority) {
        swap(&pq->items[(index - 1) / 2], &pq->items[index]);
        heapifyUp(pq, (index - 1) / 2);
    }
}

// Define heapifyDown function to maintain heap property during deletion
void heapifyDown(PriorityQueue *pq, int index) {
    int largest = index;
    int left = 2 * index + 1;
    int right = 2 * index + 2;

    if (left < pq->size &&
        pq->items[left].priority > pq->items[largest].priority)
        largest = left;

    if (right < pq->size &&
        pq->items[right].priority > pq->items[largest].priority)
        largest = right;

    if (largest != index) {
        swap(&pq->items[index], &pq->items[largest]);
        heapifyDown(pq, largest);
    }
}

// Define insertSatelliteIntoQueue function to add a SatelliteRequest to the
// queue
void insertSatelliteIntoQueue(SatelliteRequest *request) {
    if (requestQueue.size == requestQueue.max) {
        // Reallocate memory if the queue is full
        increment();
    }

    // Add the new request and heapify up
    requestQueue.items[requestQueue.size++] = *request;
    heapifyUp(&requestQueue, requestQueue.size - 1);

    // printHeap();
}

// Define getHighestPrioritySatellite function to remove and return the highest
// priority SatelliteRequest
SatelliteRequest *getHighestPrioritySatellite() {
    if (requestQueue.size == 0) {
        printf("Priority queue is empty\n");
        return NULL;
    }

    // Remove the highest priority request (root of the heap)
    SatelliteRequest *highestPriority = malloc(sizeof(SatelliteRequest));
    *highestPriority = requestQueue.items[0];

    // Replace root with the last element and heapify down
    requestQueue.items[0] = requestQueue.items[--requestQueue.size];
    heapifyDown(&requestQueue, 0);

    // printHeap();

    return highestPriority;
}

// Initialize the priority queue
void initializePriorityQueue() {
    requestQueue.items = malloc(sizeof(SatelliteRequest) * BASE_INCREMENT);
    if (!requestQueue.items) {
        perror("Failed to allocate memory for priority queue");
        exit(EXIT_FAILURE);
    }
    requestQueue.size = 0;
    requestQueue.max = BASE_INCREMENT;
}

// Check if a satellite has timed out
int hasTimedOut(SatelliteRequest *request) {
    time_t currentTime = time(NULL);
    return difftime(currentTime, request->arrival_time) > MAX_CONNECTION_WINDOW;
}

// Increment the size of the queue
void increment() {
    int newMax = requestQueue.max + BASE_INCREMENT;
    SatelliteRequest *newItems = malloc(sizeof(SatelliteRequest) * newMax);
    if (!newItems) {
        perror("Failed to allocate memory for priority queue expansion");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < requestQueue.size; i++) {
        newItems[i] = requestQueue.items[i];
    }
    free(requestQueue.items);
    requestQueue.items = newItems;
    requestQueue.max = newMax;
}

/*
void printHeap() {
    printf("[DEBUG] Current Heap State:\n");
    for (int i = 0; i < requestQueue.size; i++) {
        printf("[DEBUG] Satellite ID: %d, Priority: %d, Arrival Time: %ld\n",
               requestQueue.items[i].satellite_id,
               requestQueue.items[i].priority,
               requestQueue.items[i].arrival_time);
    }
    printf("[DEBUG] End of Heap\n");
}
*/

// -------------------- Main Function --------------------

int main() {
    // Initialize priority queue
    initializePriorityQueue();

    // Initialize mutex and semaphores
    pthread_mutex_init(&engineerMutex, NULL);
    sem_init(&newRequest, 0, 0);
    sem_init(&requestHandled, 0, 0);

    // Create engineer threads
    for (int i = 0; i < NUM_ENGINEERS; i++) {
        pthread_create(&engineerThreads[i], NULL, engineer, (void *)(long)i);
    }

    // Create satellite threads
    for (int i = 0; i < NUM_SATELLITES; i++) {
        pthread_create(&satelliteThreads[i], NULL, satellite, (void *)(long)i);
    }

    // Wait for all satellite threads to finish
    for (int i = 0; i < NUM_SATELLITES; i++) {
        pthread_join(satelliteThreads[i], NULL);
    }

    // Optional: Cancel or join engineer threads if desired
    for (int i = 0; i < NUM_ENGINEERS; i++) {
        pthread_cancel(engineerThreads[i]);
        pthread_join(engineerThreads[i], NULL);
    }

    // Destroy mutex and semaphores
    pthread_mutex_destroy(&engineerMutex);
    sem_destroy(&newRequest);
    sem_destroy(&requestHandled);

    return 0;
}

// -------------------- Function Definitions --------------------

// Engineer thread function
void *engineer(void *arg) {
    int id = (int)(long)arg;

    while (1) {
        // Wait for a new request to arrive
        sem_wait(&newRequest);

        // Lock mutex to access shared resources
        pthread_mutex_lock(&engineerMutex);

        // Pick the satellite with the highest priority
        SatelliteRequest *request = getHighestPrioritySatellite();
        if (request == NULL) {
            pthread_mutex_unlock(&engineerMutex);
            continue;
        }

        // Decrement available engineers
        availableEngineers--;

        // Log handling of the satellite
        printf("[ENGINEER %d] Handling Satellite %d (Priority %d)\n", id,
               request->satellite_id, request->priority);

        // Unlock mutex
        pthread_mutex_unlock(&engineerMutex);

        // Process the satellite (simulate work)
        int randomTime =
            rand() % 5 + 1;  // Random sleep between 1 and 5 seconds
        sleep(randomTime);

        // Log completion of the satellite
        printf("[ENGINEER %d] Finished Satellite %d\n", id,
               request->satellite_id);

        // After work, increment available engineers
        pthread_mutex_lock(&engineerMutex);
        availableEngineers++;
        pthread_mutex_unlock(&engineerMutex);

        // Signal that the request has been handled
        sem_post(&requestHandled);

        // Free the request memory
        free(request);
    }

    // Log engineer exiting
    printf("[ENGINEER %d] Exiting...\n", id);
    return NULL;
}

// Satellite thread function
void *satellite(void *arg) {
    int id = (int)(long)arg;

    // Create a SatelliteRequest structure
    SatelliteRequest *request = malloc(sizeof(SatelliteRequest));
    if (!request) {
        perror("Failed to allocate memory for SatelliteRequest");
        return NULL;
    }
    request->satellite_id = id;
    request->priority = rand() % 4 + 1;  // Random priority between 1 and 10
    request->arrival_time = time(NULL);

    // Log satellite requesting
    printf("[SATELLITE] Satellite %d requesting (priority %d)\n", id,
           request->priority);

    // Lock mutex to access shared resources
    pthread_mutex_lock(&engineerMutex);

    // Insert the satellite request into the priority queue
    insertSatelliteIntoQueue(request);

    // Unlock mutex
    pthread_mutex_unlock(&engineerMutex);

    // Signal engineers that a new request has arrived
    sem_post(&newRequest);

    // Wait for an engineer to handle the request within the timeout
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec +=
        MAX_CONNECTION_WINDOW;  // Timeout after MAX_CONNECTION_WINDOW seconds

    if (sem_timedwait(&requestHandled, &ts) == -1) {
        // Timeout occurred: satellite leaves
        printf("[TIMEOUT] Satellite %d timeout %d seconds.\n", id,
               MAX_CONNECTION_WINDOW);
    }

    return NULL;
}
