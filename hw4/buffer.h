#ifndef _BUFFER_H_
#define _BUFFER_H_

#include <pthread.h>

#define MAX_LINE_LENGTH 1024

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

void initBuffer(SharedBuffer *buf, int size);
void destroyBuffer(SharedBuffer *buf);
void addToBuffer(SharedBuffer *buf, char *line);
char *removeFromBuffer(SharedBuffer *buf);



#endif