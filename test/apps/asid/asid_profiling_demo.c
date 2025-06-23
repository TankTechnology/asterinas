// SPDX-License-Identifier: MPL-2.0

// This program demonstrates ASID profiling by creating multiple processes and threads
// to trigger various ASID operations and showcase the profiling capabilities.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>

#define NUM_PROCESSES 8
#define NUM_THREADS_PER_PROCESS 8
#define MEMORY_SIZE (1 * 1024 * 1024)  // 1MB per thread
#define NUM_MEMORY_OPERATIONS 5000
#define STRESS_DURATION_SECONDS 10

typedef struct {
    int process_id;
    int thread_id;
    void *memory;
    size_t size;
    volatile int *stop_flag;
} worker_data_t;

volatile int global_stop_flag = 0;

// Signal handler to stop the test
void signal_handler(int sig) {
    global_stop_flag = 1;
    printf("\n[DEMO] Received signal %d, stopping test...\n", sig);
}

// Function to perform memory-intensive work to trigger ASID operations
void* memory_worker(void* arg) {
    worker_data_t *data = (worker_data_t*)arg;
    unsigned int seed = (unsigned int)(time(NULL) ^ data->process_id ^ data->thread_id);
    
    printf("[P%d-T%d] Starting memory worker\n", data->process_id, data->thread_id);
    
    // Allocate memory
    data->memory = mmap(NULL, data->size, PROT_READ | PROT_WRITE, 
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (data->memory == MAP_FAILED) {
        fprintf(stderr, "[P%d-T%d] Failed to allocate memory: %s\n", 
                data->process_id, data->thread_id, strerror(errno));
        return NULL;
    }
    
    uint32_t *mem_ptr = (uint32_t*)data->memory;
    size_t num_words = data->size / sizeof(uint32_t);
    
    // Initialize memory
    for (size_t i = 0; i < num_words; i++) {
        mem_ptr[i] = (uint32_t)(data->process_id * 0x1000 + data->thread_id * 0x100 + i);
    }
    
    printf("[P%d-T%d] Memory initialized, starting stress test\n", 
           data->process_id, data->thread_id);
    
    int operations = 0;
    while (!*(data->stop_flag) && operations < NUM_MEMORY_OPERATIONS) {
        // Random memory accesses to stress the ASID system
        for (int i = 0; i < 100 && !*(data->stop_flag); i++) {
            size_t index = rand_r(&seed) % num_words;
            
            // Read access
            volatile uint32_t value = mem_ptr[index];
            
            // Write access
            mem_ptr[index] = value ^ operations;
            
            // Another read
            volatile uint32_t verify = mem_ptr[index];
            (void)verify;
        }
        
        operations++;
        
        // Occasionally yield to trigger context switches
        if (operations % 10 == 0) {
            sched_yield();
        }
        
        // Occasionally sleep to trigger more context switches
        if (operations % 50 == 0) {
            usleep(1000); // 1ms
        }
    }
    
    printf("[P%d-T%d] Completed %d operations\n", 
           data->process_id, data->thread_id, operations);
    
    // Clean up memory
    if (munmap(data->memory, data->size) != 0) {
        fprintf(stderr, "[P%d-T%d] Failed to unmap memory: %s\n", 
                data->process_id, data->thread_id, strerror(errno));
    }
    
    return NULL;
}

// Function to run in each child process
void child_process_main(int process_id) {
    pthread_t threads[NUM_THREADS_PER_PROCESS];
    worker_data_t thread_data[NUM_THREADS_PER_PROCESS];
    volatile int process_stop_flag = 0;
    
    printf("[P%d] Child process started (PID: %d)\n", process_id, getpid());
    
    // Create threads in this process
    for (int i = 0; i < NUM_THREADS_PER_PROCESS; i++) {
        thread_data[i].process_id = process_id;
        thread_data[i].thread_id = i;
        thread_data[i].size = MEMORY_SIZE;
        thread_data[i].stop_flag = &process_stop_flag;
        
        int ret = pthread_create(&threads[i], NULL, memory_worker, &thread_data[i]);
        if (ret != 0) {
            fprintf(stderr, "[P%d] Failed to create thread %d: %s\n", 
                    process_id, i, strerror(ret));
            exit(EXIT_FAILURE);
        }
        
        // Stagger thread creation
        usleep(10000); // 10ms
    }
    
    printf("[P%d] All threads created, waiting for completion...\n", process_id);
    
    // Wait for global stop signal or timeout
    time_t start_time = time(NULL);
    while (!global_stop_flag && (time(NULL) - start_time) < STRESS_DURATION_SECONDS) {
        sleep(1);
    }
    
    // Signal threads to stop
    process_stop_flag = 1;
    
    // Wait for all threads to complete
    for (int i = 0; i < NUM_THREADS_PER_PROCESS; i++) {
        int ret = pthread_join(threads[i], NULL);
        if (ret != 0) {
            fprintf(stderr, "[P%d] Failed to join thread %d: %s\n", 
                    process_id, i, strerror(ret));
        }
    }
    
    printf("[P%d] All threads completed, process exiting\n", process_id);
}

int main(int argc, char *argv[]) {
    pid_t child_pids[NUM_PROCESSES];
    int status;
    
    printf("=== ASID Profiling Demonstration ===\n");
    printf("This program will create %d processes with %d threads each\n", 
           NUM_PROCESSES, NUM_THREADS_PER_PROCESS);
    printf("Each thread will allocate %d MB and perform memory operations\n", 
           MEMORY_SIZE / (1024 * 1024));
    printf("This will stress the ASID allocation and TLB management systems\n");
    printf("Duration: %d seconds\n", STRESS_DURATION_SECONDS);
    printf("Main process PID: %d\n\n", getpid());
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Fork child processes
    for (int i = 0; i < NUM_PROCESSES; i++) {
        pid_t pid = fork();
        
        if (pid == 0) {
            // Child process
            child_process_main(i);
            exit(EXIT_SUCCESS);
        } else if (pid > 0) {
            // Parent process
            child_pids[i] = pid;
            printf("[DEMO] Created child process %d (PID: %d)\n", i, pid);
            
            // Stagger process creation
            usleep(100000); // 100ms
        } else {
            // Fork failed
            fprintf(stderr, "[DEMO] Failed to fork process %d: %s\n", i, strerror(errno));
            
            // Kill already created children
            for (int j = 0; j < i; j++) {
                kill(child_pids[j], SIGTERM);
            }
            exit(EXIT_FAILURE);
        }
    }
    
    printf("[DEMO] All processes created, monitoring...\n\n");
    
    // Monitor and wait for completion
    time_t start_time = time(NULL);
    int completed_processes = 0;
    
    while (completed_processes < NUM_PROCESSES && !global_stop_flag) {
        // Check if any child has completed
        for (int i = 0; i < NUM_PROCESSES; i++) {
            if (child_pids[i] > 0) {
                pid_t result = waitpid(child_pids[i], &status, WNOHANG);
                if (result > 0) {
                    printf("[DEMO] Process %d (PID: %d) completed with status %d\n", 
                           i, child_pids[i], status);
                    child_pids[i] = 0; // Mark as completed
                    completed_processes++;
                }
            }
        }
        
        // Check for timeout
        if ((time(NULL) - start_time) >= STRESS_DURATION_SECONDS) {
            printf("[DEMO] Timeout reached, signaling all processes to stop\n");
            global_stop_flag = 1;
            
            // Send SIGTERM to all remaining children
            for (int i = 0; i < NUM_PROCESSES; i++) {
                if (child_pids[i] > 0) {
                    kill(child_pids[i], SIGTERM);
                }
            }
        }
        
        sleep(1);
    }
    
    // Wait for any remaining children
    printf("[DEMO] Waiting for remaining processes to complete...\n");
    for (int i = 0; i < NUM_PROCESSES; i++) {
        if (child_pids[i] > 0) {
            waitpid(child_pids[i], &status, 0);
            printf("[DEMO] Process %d (PID: %d) terminated\n", i, child_pids[i]);
        }
    }
    
    time_t end_time = time(NULL);
    printf("\n=== ASID Profiling Demo Completed ===\n");
    printf("Total runtime: %ld seconds\n", end_time - start_time);
    printf("This test has exercised:\n");
    printf("- ASID allocation/deallocation across multiple processes\n");
    printf("- Context switching between processes and threads\n");
    printf("- TLB operations during memory access patterns\n");
    printf("- ASID reuse and generation management\n");
    printf("\nTo view ASID profiling statistics, check the kernel logs or\n");
    printf("use the kernel's ASID profiling interfaces if available.\n");
    
    return EXIT_SUCCESS;
} 