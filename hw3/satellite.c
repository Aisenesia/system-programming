// satellite_engineer.c

#define _POSIX_C_SOURCE 200809L // just for vscode to see, CLOCK_REALTIME

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h> // For sleep and random simulation
#include <time.h>

// -------------------- Constants --------------------

// Number of engineers available
#define NUM_ENGINEERS 3

#define BASE_INCREMENT 16 // Sattellite heap is held in well, heap and when full its size is incremented by this
#define MAX_SATELLITES 1024
// Number of satellites
#define NUM_SATELLITES 5

// Maximum connection window for satellites (in seconds)
#define MAX_CONNECTION_WINDOW 5

// -------------------- Data Structures --------------------


// Structure to represent a satellite request
typedef struct {
    int satellite_id; // Unique ID of the satellite
    int priority;     // Priority of the satellite
    time_t arrival_time; // Time when the request was made (for timeout checks)
} SatelliteRequest;


typedef struct {
    SatelliteRequest* items;
    int size;
    int max;
} PriorityQueue;



// -------------------- Global Variables --------------------

// Counter for available engineers
__sig_atomic_t availableEngineers = NUM_ENGINEERS;

// Priority queue to manage satellite requests (simulate manually)
PriorityQueue requestQueue; // Array-based for simplicity
int queueSize = 0; // Current number of satellites in the queue

// Mutex to protect shared resources
pthread_mutex_t engineerMutex;

// Semaphores for synchronization
sem_t newRequest;     // Signaled by satellites when they create a new request
sem_t requestHandled; // Signaled by engineers once they pick up a satellite request

// Thread arrays
pthread_t engineerThreads[NUM_ENGINEERS];
pthread_t satelliteThreads[NUM_SATELLITES];

// -------------------- Function Declarations --------------------

// Function for engineer threads
void *engineer(void *arg);

// Function for satellite threads
void *satellite(void *arg);

// Helper to insert satellite into priority queue (placeholder)
void insertSatelliteIntoQueue(SatelliteRequest *request);

// Helper to get the highest priority satellite from the queue (placeholder)
SatelliteRequest* getHighestPrioritySatellite();

// Helper to simulate satellite timeout checking (placeholder)
int hasTimedOut(SatelliteRequest *request);

void increment(); // Increment the size of the queue
void swap(SatelliteRequest* a, SatelliteRequest* b); // Swap two requests


// Define swap function to swap two SatelliteRequest objects
void swap(SatelliteRequest* a, SatelliteRequest* b) {
    SatelliteRequest temp = *a;
    *a = *b;
    *b = temp;
}

// Define heapifyUp function to maintain heap property during insertion
void heapifyUp(PriorityQueue* pq, int index) {
    if (index && pq->items[(index - 1) / 2].priority < pq->items[index].priority) {
        swap(&pq->items[(index - 1) / 2], &pq->items[index]);
        heapifyUp(pq, (index - 1) / 2);
    }
}

// Define heapifyDown function to maintain heap property during deletion
void heapifyDown(PriorityQueue* pq, int index) {
    int largest = index;
    int left = 2 * index + 1;
    int right = 2 * index + 2;

    if (left < pq->size && pq->items[left].priority > pq->items[largest].priority)
        largest = left;

    if (right < pq->size && pq->items[right].priority > pq->items[largest].priority)
        largest = right;

    if (largest != index) {
        swap(&pq->items[index], &pq->items[largest]);
        heapifyDown(pq, largest);
    }
}

// Define insertSatelliteIntoQueue function to add a SatelliteRequest to the queue
void insertSatelliteIntoQueue(SatelliteRequest* request) {
    if (requestQueue.size == requestQueue.max) {
        // Reallocate memory if the queue is full
        increment();
    }

    // Add the new request and heapify up
    requestQueue.items[requestQueue.size++] = *request;
    heapifyUp(&requestQueue, requestQueue.size - 1);
}

// Define getHighestPrioritySatellite function to remove and return the highest priority SatelliteRequest
SatelliteRequest* getHighestPrioritySatellite() {
    if (requestQueue.size == 0) {
        printf("Priority queue is empty\n");
        return NULL;
    }

    // Remove the highest priority request (root of the heap)
    SatelliteRequest* highestPriority = malloc(sizeof(SatelliteRequest));
    *highestPriority = requestQueue.items[0];

    // Replace root with the last element and heapify down
    requestQueue.items[0] = requestQueue.items[--requestQueue.size];
    heapifyDown(&requestQueue, 0);

    return highestPriority;
}

// -------------------- Main Function --------------------

int main() {
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

        // Decrement available engineers
        availableEngineers--;

        // Unlock mutex
        pthread_mutex_unlock(&engineerMutex);

        // Process the satellite (simulate work)
        // sleep(random_time);

        // After work, increment available engineers
        pthread_mutex_lock(&engineerMutex);
        availableEngineers++;
        pthread_mutex_unlock(&engineerMutex);

        // Signal that the request has been handled
        sem_post(&requestHandled);
    }
    return NULL;
}

// Satellite thread function
void *satellite(void *arg) {
    int id = (int)(long)arg;

    // Create a SatelliteRequest structure
    SatelliteRequest *request = malloc(sizeof(SatelliteRequest));
    request->satellite_id = id;
    request->priority = rand() % 10 + 1; // Random priority between 1 and 10
    request->arrival_time = time(NULL);

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
    ts.tv_sec += MAX_CONNECTION_WINDOW; // Timeout after MAX_CONNECTION_WINDOW seconds

    if (sem_timedwait(&requestHandled, &ts) == -1) {
        // Timeout occurred: satellite leaves
        printf("Satellite %d timed out and left.\n", id);
    } else {
        // Request was handled
        printf("Satellite %d was served.\n", id);
    }

    // Free the allocated request
    free(request);

    return NULL;
}

// Insert a satellite into the priority queue (higher priority first)
// Placeholder: just a stub



void increment(){

    // copy and realloc
    int max = requestQueue.max + BASE_INCREMENT;
    SatelliteRequest* new = malloc(sizeof(SatelliteRequest) * max);
    for(int i = 0; i<requestQueue.max; i++){
        new[i] = requestQueue.items[i];
    }
    free(requestQueue.items);
    requestQueue.items = new;

}

// Get the satellite with the highest priority
// Placeholder: just a stub


// Check if a satellite has timed out
// Placeholder: just a stub
int hasTimedOut(SatelliteRequest *request) {
    // This will check if the satellite's allowed connection window expired
    return 0;
}
