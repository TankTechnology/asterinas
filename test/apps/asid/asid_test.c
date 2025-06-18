// SPDX-License-Identifier: MPL-2.0

// This program is used to test the ASID mechanism.
// It creates 8 threads, each thread uses 2MB of memory, and randomly accesses the memory, checking if the access is correct.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>

#define NUM_THREADS 8
#define MEMORY_SIZE (2 * 1024 * 1024)  // 2MB per thread
#define NUM_ACCESSES 10000
#define PATTERN_SEED 0xDEADBEEF

typedef struct {
    int thread_id;
    void *memory;
    size_t size;
    int success;
} thread_data_t;

// Function to get thread ID (gettid syscall)
pid_t gettid(void) {
    return syscall(SYS_gettid);
}

// Thread function that performs memory testing
void* memory_test_thread(void* arg) {
    thread_data_t *data = (thread_data_t*)arg;
    unsigned int seed = PATTERN_SEED ^ data->thread_id ^ time(NULL);
    
    printf("Thread %d (TID: %d) starting memory test with %zu bytes\n", 
           data->thread_id, gettid(), data->size);
    
    // Allocate memory
    data->memory = mmap(NULL, data->size, PROT_READ | PROT_WRITE, 
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (data->memory == MAP_FAILED) {
        fprintf(stderr, "Thread %d: Failed to allocate memory: %s\n", 
                data->thread_id, strerror(errno));
        data->success = 0;
        return NULL;
    }
    
    printf("Thread %d: Memory allocated at %p\n", data->thread_id, data->memory);
    
    // Initialize memory with a pattern
    uint32_t *mem_ptr = (uint32_t*)data->memory;
    size_t num_words = data->size / sizeof(uint32_t);
    
    for (size_t i = 0; i < num_words; i++) {
        mem_ptr[i] = (uint32_t)(PATTERN_SEED ^ data->thread_id ^ i);
    }
    
    printf("Thread %d: Memory initialized with pattern\n", data->thread_id);
    
    // Perform random memory accesses and verify correctness
    int errors = 0;
    for (int access = 0; access < NUM_ACCESSES; access++) {
        // Generate random index
        size_t index = rand_r(&seed) % num_words;
        
        // Expected value at this location
        uint32_t expected = (uint32_t)(PATTERN_SEED ^ data->thread_id ^ index);
        
        // Read and verify
        uint32_t actual = mem_ptr[index];
        if (actual != expected) {
            errors++;
            if (errors <= 10) {  // Only print first 10 errors to avoid spam
                fprintf(stderr, "Thread %d: Memory corruption at index %zu! "
                       "Expected 0x%08x, got 0x%08x\n", 
                       data->thread_id, index, expected, actual);
            }
        }
        
        // Write a new pattern and read it back
        uint32_t new_value = (uint32_t)(expected ^ access);
        mem_ptr[index] = new_value;
        
        if (mem_ptr[index] != new_value) {
            errors++;
            if (errors <= 10) {
                fprintf(stderr, "Thread %d: Write/read mismatch at index %zu! "
                       "Wrote 0x%08x, read 0x%08x\n", 
                       data->thread_id, index, new_value, mem_ptr[index]);
            }
        }
        
        // Restore original pattern
        mem_ptr[index] = expected;
        
        // Occasional yield to allow other threads to run
        if (access % 1000 == 0) {
            sched_yield();
        }
    }
    
    if (errors == 0) {
        printf("Thread %d: PASSED - No memory errors detected in %d accesses\n", 
               data->thread_id, NUM_ACCESSES);
        data->success = 1;
    } else {
        printf("Thread %d: FAILED - %d memory errors detected in %d accesses\n", 
               data->thread_id, errors, NUM_ACCESSES);
        data->success = 0;
    }
    
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
    int successful_threads = 0;
    
    printf("ASID Memory Test Program\n");
    printf("Creating %d threads, each using %d MB of memory\n", 
           NUM_THREADS, MEMORY_SIZE / (1024 * 1024));
    printf("Each thread will perform %d random memory accesses\n", NUM_ACCESSES);
    printf("Main process PID: %d\n\n", getpid());
    
    // Initialize random seed
    srand(time(NULL));
    
    // Create threads
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].size = MEMORY_SIZE;
        thread_data[i].success = 0;
        
        int ret = pthread_create(&threads[i], NULL, memory_test_thread, &thread_data[i]);
        if (ret != 0) {
            fprintf(stderr, "Failed to create thread %d: %s\n", i, strerror(ret));
            exit(EXIT_FAILURE);
        }
    }
    
    printf("All threads created, waiting for completion...\n\n");
    
    // Wait for all threads to complete
    for (int i = 0; i < NUM_THREADS; i++) {
        int ret = pthread_join(threads[i], NULL);
        if (ret != 0) {
            fprintf(stderr, "Failed to join thread %d: %s\n", i, strerror(ret));
        } else if (thread_data[i].success) {
            successful_threads++;
        }
    }
    
    // Print summary
    printf("\n=== ASID Memory Test Results ===\n");
    printf("Successful threads: %d/%d\n", successful_threads, NUM_THREADS);
    
    if (successful_threads == NUM_THREADS) {
        printf("✅ ALL TESTS PASSED - ASID mechanism appears to be working correctly\n");
        return EXIT_SUCCESS;
    } else {
        printf("❌ SOME TESTS FAILED - Potential ASID or memory management issues\n");
        return EXIT_FAILURE;
    }
}

