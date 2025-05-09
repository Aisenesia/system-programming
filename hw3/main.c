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
#define BASE_INCREMENT 16   // Sattellite heap is held in well, heap and when full its size is
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
    SatelliteRequest **items;  // Array of pointers to SatelliteRequest
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

int satelliteBeingHandled[NUM_SATELLITES];
pthread_mutex_t handlingMutex;

// -------------------- Function Declarations --------------------

void *engineer(void *arg);
void *satellite(void *arg);




void increment();  // Increment the size of the queue
void swap(SatelliteRequest *a, SatelliteRequest *b);  // Swap two requests

void insertSatelliteIntoQueue(SatelliteRequest *request);
SatelliteRequest* getHighestPrioritySatellite();  // Get the highest priority satellite
void initializePriorityQueue();
void heapifyUp(PriorityQueue *pq, int index);
void heapifyDown(PriorityQueue *pq, int index);  // Maintain heap property
void swap(SatelliteRequest *a, SatelliteRequest *b);  // Swap two requests

void printHeap();


// Define swap function to swap two SatelliteRequest objects
void swap(SatelliteRequest *a, SatelliteRequest *b) {
    SatelliteRequest temp = *a;
    *a = *b;
    *b = temp;
}

// Define heapifyUp function to maintain heap property during insertion
void heapifyUp(PriorityQueue *pq, int index) {
    if (index &&
        pq->items[(index - 1) / 2]->priority < pq->items[index]->priority) {
        swap(pq->items[(index - 1) / 2], pq->items[index]);
        heapifyUp(pq, (index - 1) / 2);
    }
}

// Define heapifyDown function to maintain heap property during deletion
void heapifyDown(PriorityQueue *pq, int index) {
    int largest = index;
    int left = 2 * index + 1;
    int right = 2 * index + 2;

    if (left < pq->size &&
        pq->items[left]->priority > pq->items[largest]->priority)
        largest = left;

    if (right < pq->size &&
        pq->items[right]->priority > pq->items[largest]->priority)
        largest = right;

    if (largest != index) {
        swap(pq->items[index], pq->items[largest]);
        heapifyDown(pq, largest);
    }
}


void insertSatelliteIntoQueue(SatelliteRequest *request) {
    if (requestQueue.size == requestQueue.max) {
        // Reallocate memory if the queue is full
        increment();
    }

    // Add the new request pointer and heapify up
    requestQueue.items[requestQueue.size++] = request;
    heapifyUp(&requestQueue, requestQueue.size - 1);

    // printHeap();
}


SatelliteRequest *getHighestPrioritySatellite() {
    if (requestQueue.size == 0) {
        return NULL;
    }

    // Get the highest-priority request (root of the heap)
    SatelliteRequest *highestPriority = requestQueue.items[0];

    // Replace root with the last element and heapify down
    requestQueue.items[0] = requestQueue.items[--requestQueue.size];
    heapifyDown(&requestQueue, 0);

    return highestPriority;
}

void initializePriorityQueue() {
    requestQueue.items = malloc(sizeof(SatelliteRequest *) * BASE_INCREMENT);
    if (!requestQueue.items) {
        perror("Failed to allocate memory for priority queue");
        exit(EXIT_FAILURE);
    }
    requestQueue.size = 0;
    requestQueue.max = BASE_INCREMENT;
}

int hasTimedOut(SatelliteRequest *request) {
    time_t currentTime = time(NULL);
    return difftime(currentTime, request->arrival_time) > MAX_CONNECTION_WINDOW;
}

void increment() {
    int newMax = requestQueue.max + BASE_INCREMENT;
    SatelliteRequest **newItems = malloc(sizeof(SatelliteRequest *) * newMax);
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
    initializePriorityQueue();

    pthread_mutex_init(&engineerMutex, NULL);
    pthread_mutex_init(&handlingMutex, NULL);

    sem_init(&newRequest, 0, 0);
    sem_init(&requestHandled, 0, 0);

    // Create engineer threads
    for (int i = 0; i < NUM_ENGINEERS; i++) {
        pthread_create(&engineerThreads[i], NULL, engineer, (void *)(long)i);
    }

    // Create satellite threads
    for (int i = 0; i < NUM_SATELLITES; i++) {
        satelliteBeingHandled[i] = 0;
        pthread_create(&satelliteThreads[i], NULL, satellite, (void *)(long)i);
    }

    // Wait for all satellite threads to finish
    for (int i = 0; i < NUM_SATELLITES; i++) {
        pthread_join(satelliteThreads[i], NULL);
    }

    for (int i = 0; i < NUM_ENGINEERS; i++) {
        pthread_cancel(engineerThreads[i]);
        pthread_join(engineerThreads[i], NULL);
    }

    // Destroy mutex and semaphores
    pthread_mutex_destroy(&engineerMutex);
    sem_destroy(&newRequest);
    sem_destroy(&requestHandled);

    // Free all remaining requests in the queue
    for (int i = 0; i < requestQueue.size; i++) {
        free(requestQueue.items[i]);
    }
    free(requestQueue.items);
    requestQueue.items = NULL;  // Avoid dangling pointer, not really needed

    return 0;
}

// -------------------- Function Definitions --------------------

void *engineer(void *arg) {
    int id = (int)(long)arg; 

    while (1) {
        sem_wait(&newRequest); // Wait for a new request to arrive

        pthread_mutex_lock(&engineerMutex);
        SatelliteRequest *request = getHighestPrioritySatellite();
        if (request == NULL) {
            pthread_mutex_unlock(&engineerMutex);
            continue; // No requests to process
        }

        pthread_mutex_lock(&handlingMutex);
        if (satelliteBeingHandled[request->satellite_id] == -1) {
            // Request has timed out, skip processing
            pthread_mutex_unlock(&handlingMutex);
            pthread_mutex_unlock(&engineerMutex);
            free(request);
            continue;
        }
        satelliteBeingHandled[request->satellite_id] = 1; // Mark as being handled
        pthread_mutex_unlock(&handlingMutex);

        availableEngineers--; // Decrement available engineers
        printf("[ENGINEER %d] Handling Satellite %d (Priority %d)\n", id, request->satellite_id, request->priority);
        pthread_mutex_unlock(&engineerMutex);

        sleep(10 + 1); // Simulate work

        printf("[ENGINEER %d] Finished Satellite %d\n", id, request->satellite_id);
        pthread_mutex_lock(&handlingMutex);
        satelliteBeingHandled[request->satellite_id] = 0; // Mark as completed
        pthread_mutex_unlock(&handlingMutex);

        pthread_mutex_lock(&engineerMutex);
        availableEngineers++; // Increment available engineers
        pthread_mutex_unlock(&engineerMutex);

        sem_post(&requestHandled); // Signal that the request has been handled
        free(request); // Free the request memory
    }

    return NULL;
}

void *satellite(void *arg) {
    int id = (int)(long)arg; 
    
    SatelliteRequest *request = malloc(sizeof(SatelliteRequest));
    if (!request) {
        perror("Failed to allocate memory for SatelliteRequest");
        return NULL;
    }
    request->satellite_id = id;
    request->priority = rand() % 4 + 1; // Random priority between 1 and 4
    request->arrival_time = time(NULL);

    printf("[SATELLITE] Satellite %d requesting (priority %d)\n", id, request->priority);

    pthread_mutex_lock(&engineerMutex);
    insertSatelliteIntoQueue(request); // Add request to the priority queue
    pthread_mutex_unlock(&engineerMutex);

    sem_post(&newRequest); // Notify engineers of a new request

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += MAX_CONNECTION_WINDOW; // Set timeout window

    while (1) {
        pthread_mutex_lock(&engineerMutex);
        pthread_mutex_lock(&handlingMutex);

        int isBeingHandled = satelliteBeingHandled[id];
        if (isBeingHandled == 1) {
            // Request is being handled by an engineer
            pthread_mutex_unlock(&handlingMutex);
            pthread_mutex_unlock(&engineerMutex);
            sem_wait(&requestHandled); // Wait for request to be handled
            break;
        }

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec >= ts.tv_sec) {
            printf("[TIMEOUT] Satellite %d timeout %d seconds.\n", id, MAX_CONNECTION_WINDOW);

            // Remove the request from the queue if it hasn't been handled
            for (int i = 0; i < requestQueue.size; i++) {
                if (requestQueue.items[i]->satellite_id == id) {
                    SatelliteRequest *timedOutRequest = requestQueue.items[i];
                    requestQueue.items[i] = requestQueue.items[--requestQueue.size];
                    heapifyDown(&requestQueue, i);

                    if (satelliteBeingHandled[id] == 0) {
                        // Only free if not being handled
                        free(timedOutRequest);
                    }

                    satelliteBeingHandled[id] = -1; // Mark as timed out
                    break;
                }
            }

            pthread_mutex_unlock(&handlingMutex);
            pthread_mutex_unlock(&engineerMutex);
            return NULL;
        }

        pthread_mutex_unlock(&handlingMutex);
        pthread_mutex_unlock(&engineerMutex);
        sleep(1); // Avoid busy-waiting, using usleep and sleeping for 100ms is far better idea
    }

    return NULL;
}
