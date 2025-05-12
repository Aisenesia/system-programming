#ifndef _BUFFER_H_
#define _BUFFER_H_
#define _POSIX_C_SOURCE 200809L  // just for vscode to see, pthread_barrier_t


#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_LINE_LENGTH 4096

typedef struct {
    char **buffer;
    int size;
    int start;
    int end;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} SharedBuffer;
    
extern SharedBuffer sharedBuffer;
extern pthread_barrier_t barrier;
extern int num_workers;
extern char *search_term;

extern volatile sig_atomic_t terminate;


void initBuffer(SharedBuffer *buf, int size);
void destroyBuffer(SharedBuffer *buf);
void addToBuffer(SharedBuffer *buf, char *line);
char *removeFromBuffer(SharedBuffer *buf);



#endif