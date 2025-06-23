// SPDX-License-Identifier: MPL-2.0

// This program measures the time it takes for 16 threads to randomly access 4MB of memory each.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>

#define NUM_THREADS 32
#define MEMORY_SIZE (4 * 1024 * 1024) 
#define NUM_ACCESSES 100000  // Number of random accesses per thread
#define WARMUP_ACCESSES 1000  // Warmup accesses to avoid cold cache effects

typedef struct {
    int thread_id;
    void *memory;
    size_t size;
    uint64_t access_time_ns;  // Time taken for memory accesses in nanoseconds
    uint64_t total_accesses;
} thread_data_t;

// Helper function to get time in nanoseconds
uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Thread function that performs timed memory accesses
void* memory_access_thread(void* arg) {
    thread_data_t *data = (thread_data_t*)arg;
    unsigned int seed = (unsigned int)(time(NULL) ^ data->thread_id);
    
    printf("Thread %d: Starting memory access benchmark\n", data->thread_id);
    
    // Allocate memory
    data->memory = mmap(NULL, data->size, PROT_READ | PROT_WRITE, 
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (data->memory == MAP_FAILED) {
        fprintf(stderr, "Thread %d: Failed to allocate memory: %s\n", 
                data->thread_id, strerror(errno));
        data->access_time_ns = 0;
        return NULL;
    }
    
    uint32_t *mem_ptr = (uint32_t*)data->memory;
    size_t num_words = data->size / sizeof(uint32_t);
    
    // Initialize memory with some data
    for (size_t i = 0; i < num_words; i++) {
        mem_ptr[i] = (uint32_t)(data->thread_id * 0x12345678 + i);
    }
    
    printf("Thread %d: Memory initialized, starting warmup\n", data->thread_id);
    
    // Warmup phase - don't count this time
    for (int i = 0; i < WARMUP_ACCESSES; i++) {
        size_t index = rand_r(&seed) % num_words;
        volatile uint32_t dummy = mem_ptr[index];  // Read access
        mem_ptr[index] = dummy + 1;  // Write access
        (void)dummy;  // Suppress unused variable warning
    }
    
    printf("Thread %d: Starting timed memory access test\n", data->thread_id);
    
    // Start timing
    uint64_t start_time = get_time_ns();
    
    // Perform timed memory accesses
    for (int access = 0; access < NUM_ACCESSES; access++) {
        size_t index = rand_r(&seed) % num_words;
        
        // Random read access
        volatile uint32_t value = mem_ptr[index];
        
        // Random write access
        mem_ptr[index] = value ^ access;
        
        // Another read to ensure write completion
        volatile uint32_t verify = mem_ptr[index];
        (void)verify;  // Suppress unused variable warning
    }
    
    // End timing
    uint64_t end_time = get_time_ns();
    data->access_time_ns = end_time - start_time;
    data->total_accesses = NUM_ACCESSES * 2;  // Count both reads and writes
    
    printf("Thread %d: Completed %d memory operations in %.2f ms\n", 
           data->thread_id, (int)data->total_accesses, 
           data->access_time_ns / 1000000.0);
    
    // Clean up memory
    if (munmap(data->memory, data->size) != 0) {
        fprintf(stderr, "Thread %d: Failed to unmap memory: %s\n", 
                data->thread_id, strerror(errno));
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    pthread_t threads[NUM_THREADS];
    thread_data_t thread_data[NUM_THREADS];
    
    printf("Memory Access Performance Test\n");
    printf("===============================\n");
    printf("Threads: %d\n", NUM_THREADS);
    printf("Memory per thread: %d MB\n", MEMORY_SIZE / (1024 * 1024));
    printf("Memory operations per thread: %d\n", NUM_ACCESSES * 2);
    printf("Total memory operations: %d\n", NUM_THREADS * NUM_ACCESSES * 2);
    printf("Warmup operations per thread: %d\n", WARMUP_ACCESSES * 2);
    printf("\n");
    
    // Record overall start time
    uint64_t overall_start = get_time_ns();
    
    // Create threads
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].size = MEMORY_SIZE;
        thread_data[i].access_time_ns = 0;
        thread_data[i].total_accesses = 0;
        
        int ret = pthread_create(&threads[i], NULL, memory_access_thread, &thread_data[i]);
        if (ret != 0) {
            fprintf(stderr, "Failed to create thread %d: %s\n", i, strerror(ret));
            exit(EXIT_FAILURE);
        }
        
        // Small delay to stagger thread creation
        usleep(1000);  // 1ms delay
    }
    
    printf("All threads created, waiting for completion...\n\n");
    
    // Wait for all threads to complete
    for (int i = 0; i < NUM_THREADS; i++) {
        int ret = pthread_join(threads[i], NULL);
        if (ret != 0) {
            fprintf(stderr, "Failed to join thread %d: %s\n", i, strerror(ret));
        }
    }
    
    uint64_t overall_end = get_time_ns();
    uint64_t overall_time = overall_end - overall_start;
    
    // Calculate statistics
    uint64_t total_operations = 0;
    uint64_t min_time = UINT64_MAX;
    uint64_t max_time = 0;
    uint64_t total_time = 0;
    
    printf("=== Per-Thread Results ===\n");
    for (int i = 0; i < NUM_THREADS; i++) {
        if (thread_data[i].access_time_ns > 0) {
            total_operations += thread_data[i].total_accesses;
            total_time += thread_data[i].access_time_ns;
            
            if (thread_data[i].access_time_ns < min_time) {
                min_time = thread_data[i].access_time_ns;
            }
            if (thread_data[i].access_time_ns > max_time) {
                max_time = thread_data[i].access_time_ns;
            }
            
            double ops_per_sec = (double)thread_data[i].total_accesses * 1000000000.0 / thread_data[i].access_time_ns;
            double ns_per_op = (double)thread_data[i].access_time_ns / thread_data[i].total_accesses;
            
            printf("Thread %d: %.2f ms, %.0f ops/sec, %.1f ns/op\n", 
                   i, thread_data[i].access_time_ns / 1000000.0, ops_per_sec, ns_per_op);
        }
    }
    
    printf("\n=== Summary Statistics ===\n");
    printf("Overall execution time: %.2f ms\n", overall_time / 1000000.0);
    printf("Total memory operations: %lu\n", total_operations);
    printf("Average time per thread: %.2f ms\n", (total_time / NUM_THREADS) / 1000000.0);
    printf("Fastest thread: %.2f ms\n", min_time / 1000000.0);
    printf("Slowest thread: %.2f ms\n", max_time / 1000000.0);
    printf("Thread time variance: %.2f ms\n", (max_time - min_time) / 1000000.0);
    
    if (total_operations > 0) {
        double total_ops_per_sec = (double)total_operations * 1000000000.0 / overall_time;
        double avg_ns_per_op = (double)total_time / total_operations;
        
        printf("Overall throughput: %.0f ops/sec\n", total_ops_per_sec);
        printf("Average latency: %.1f ns/op\n", avg_ns_per_op);
        
        // Calculate memory bandwidth (assuming each operation touches 4 bytes)
        double bytes_per_sec = total_ops_per_sec * sizeof(uint32_t);
        printf("Estimated memory bandwidth: %.1f MB/sec\n", bytes_per_sec / (1024 * 1024));
    }
    
    return EXIT_SUCCESS;
}
