#ifndef BUFFER_H
#define BUFFER_H

#include <pthread.h>
#include <semaphore.h>
#include "protocol.h"

#define BUFFER_SIZE 10

typedef struct {
    int client_id;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} connection_request_t;

typedef struct {
    connection_request_t requests[BUFFER_SIZE];
    int in; // Next position to insert
    int out; // Next position to remove
    int count;
    pthread_mutex_t mutex;
    sem_t *empty; // Counts empty slots
    sem_t *full; // Counts full slots
} request_buffer_t;


void buffer_init(request_buffer_t *buf); 
void buffer_insert(request_buffer_t *buf, connection_request_t req); 
connection_request_t buffer_remove(request_buffer_t *buf);
void buffer_destroy(request_buffer_t *buf);

#endif
