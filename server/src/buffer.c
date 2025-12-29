#include "buffer.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

void buffer_init(request_buffer_t *buf) {
    buf->in = 0;
    buf->out = 0;
    buf->count = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    
    char sem_empty_name[64];
    char sem_full_name[64];
    snprintf(sem_empty_name, sizeof(sem_empty_name), "/pacman_empty_%d", getpid());
    snprintf(sem_full_name, sizeof(sem_full_name), "/pacman_full_%d", getpid());
    
    sem_unlink(sem_empty_name);
    sem_unlink(sem_full_name);
    
    buf->empty = sem_open(sem_empty_name, O_CREAT | O_EXCL, 0644, BUFFER_SIZE);
    buf->full = sem_open(sem_full_name, O_CREAT | O_EXCL, 0644, 0);
    
    if (buf->empty == SEM_FAILED || buf->full == SEM_FAILED) {
        perror("sem_open failed");
        exit(1);
    }
}

void buffer_insert(request_buffer_t *buf, connection_request_t req) {
    sem_wait(buf->empty);
    pthread_mutex_lock(&buf->mutex);
    
    buf->requests[buf->in] = req;
    buf->in = (buf->in + 1) % BUFFER_SIZE;
    buf->count++;
    
    pthread_mutex_unlock(&buf->mutex);
    sem_post(buf->full);
}

connection_request_t buffer_remove(request_buffer_t *buf) {
    sem_wait(buf->full);
    pthread_mutex_lock(&buf->mutex);
    
    connection_request_t req = buf->requests[buf->out];
    buf->out = (buf->out + 1) % BUFFER_SIZE;
    buf->count--;
    
    pthread_mutex_unlock(&buf->mutex);
    sem_post(buf->empty);
    
    return req;
}

void buffer_destroy(request_buffer_t *buf) {
    pthread_mutex_destroy(&buf->mutex);
    
    sem_close(buf->empty);
    sem_close(buf->full);
    
    char sem_empty_name[64];
    char sem_full_name[64];
    snprintf(sem_empty_name, sizeof(sem_empty_name), "/pacman_empty_%d", getpid());
    snprintf(sem_full_name, sizeof(sem_full_name), "/pacman_full_%d", getpid());
    
    sem_unlink(sem_empty_name);
    sem_unlink(sem_full_name);
}