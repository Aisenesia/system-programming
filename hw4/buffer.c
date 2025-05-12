#include "buffer.h"


#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


// Function to initialize the shared buffer
void initBuffer(SharedBuffer *buf, int size) {
    buf->buffer = malloc(size * sizeof(char *));
    if (buf->buffer == NULL) {
        perror("Buffer allocation failed");
        exit(1);
    }

    buf->size = size;
    buf->start = 0;
    buf->end = 0;
    buf->count = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    pthread_cond_init(&buf->not_full, NULL);
    pthread_cond_init(&buf->not_empty, NULL);
}

// Function to destroy the shared buffer
void destroyBuffer(SharedBuffer *buf) {
    pthread_mutex_lock(&buf->mutex);
    for (int i = 0; i < buf->count; i++) {
        char *line = buf->buffer[(buf->start + i) % buf->size];
        if (line != NULL) {
            free(line);
        }
    }
    pthread_mutex_unlock(&buf->mutex);

    free(buf->buffer);
    pthread_mutex_destroy(&buf->mutex);
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
}

// Add a line to the buffer
void addToBuffer(SharedBuffer *buf, char *line) {
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == buf->size && !terminate) {
        pthread_cond_wait(&buf->not_full, &buf->mutex);
    }
    if (terminate) {
        pthread_cond_broadcast(&buf->not_full);
        pthread_cond_broadcast(&buf->not_empty);
        pthread_mutex_unlock(&buf->mutex);
        free(line);
        return;
    }

    buf->buffer[buf->end] = line;
    buf->end = (buf->end + 1) % buf->size;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
}

char *removeFromBuffer(SharedBuffer *buf) {
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !terminate) {
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    }

    if (terminate && buf->count == 0) {
        pthread_cond_broadcast(&buf->not_full);
        pthread_cond_broadcast(&buf->not_empty);
        pthread_mutex_unlock(&buf->mutex);
        return NULL;
    }

    char *line = buf->buffer[buf->start];
    buf->buffer[buf->start] = NULL;
    buf->start = (buf->start + 1) % buf->size;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);

    return line;
}
