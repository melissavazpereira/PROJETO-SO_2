#include "buffer.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

// Initialize the request buffer
void buffer_init(request_buffer_t *buf) {
    buf->in = 0;
    buf->out = 0;
    buf->count = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    
    char sem_empty_name[64];
    char sem_full_name[64];
    // Create unique semaphore names
    snprintf(sem_empty_name, sizeof(sem_empty_name), "/pacman_empty_%d", getpid()); 
    snprintf(sem_full_name, sizeof(sem_full_name), "/pacman_full_%d", getpid()); 
    
    // Remove old semaphores with the same names if they exist
    sem_unlink(sem_empty_name); 
    sem_unlink(sem_full_name);
    
    buf->empty = sem_open(sem_empty_name, O_CREAT | O_EXCL, 0644, BUFFER_SIZE); // Initially, all slots are empty
    buf->full = sem_open(sem_full_name, O_CREAT | O_EXCL, 0644, 0); // Initially, no full slots
    
    // Check for semaphore creation errors
    if (buf->empty == SEM_FAILED || buf->full == SEM_FAILED) {
        perror("sem_open failed");
        exit(1);
    }
}

// Insert a request into the buffer
void buffer_insert(request_buffer_t *buf, connection_request_t req) {
    sem_wait(buf->empty); // Wait for an empty slot
    pthread_mutex_lock(&buf->mutex); 
    
    buf->requests[buf->in] = req; // Insert the request
    buf->in = (buf->in + 1) % BUFFER_SIZE; // Update the in index circularly
    buf->count++; // Increment the count of requests
    
    pthread_mutex_unlock(&buf->mutex);
    sem_post(buf->full); // Signal that a new request is available for a new consumer
}

// Remove a request from the buffer
connection_request_t buffer_remove(request_buffer_t *buf) {
    sem_wait(buf->full); // Wait for a full slot
    pthread_mutex_lock(&buf->mutex);
    
    connection_request_t req = buf->requests[buf->out]; // Remove the request
    buf->out = (buf->out + 1) % BUFFER_SIZE; // Update the out index circularly
    buf->count--; // Decrement the count of requests
    
    pthread_mutex_unlock(&buf->mutex);
    sem_post(buf->empty); // Signal that a slot is now empty for a new producer
    
    return req; // Return the removed request
}

void buffer_destroy(request_buffer_t *buf) {
    pthread_mutex_destroy(&buf->mutex); // Destroy the mutex
    
    sem_close(buf->empty); // Close the semaphores
    sem_close(buf->full);  // Close the semaphores
    
    char sem_empty_name[64];
    char sem_full_name[64];
    // Recover semaphore names
    snprintf(sem_empty_name, sizeof(sem_empty_name), "/pacman_empty_%d", getpid()); 
    snprintf(sem_full_name, sizeof(sem_full_name), "/pacman_full_%d", getpid()); 
    
    // Remove the semaphores
    sem_unlink(sem_empty_name); 
    sem_unlink(sem_full_name); 
}