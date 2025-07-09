// SPDX-License-Identifier: MPL-2.0

// ASID Efficiency Test - Clean Performance Measurement
// This test measures the raw performance of the new ASID implementation
// WITHOUT monitoring overhead to get baseline performance numbers.
//
// This version:
// - Minimizes overhead from profiling and monitoring
// - Focuses on pure throughput and latency measurements
// - Provides clean baseline for comparison with monitored tests
// - Measures only essential metrics for performance evaluation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>
#include <sys/wait.h>

#define MAX_THREADS 64
#define MEMORY_SIZE (8 * 1024 * 1024)  // 8MB per thread  
#define NUM_OPERATIONS_PER_BURST 10000
#define DEFAULT_TEST_DURATION 20

// Minimal configuration for clean testing
typedef struct {
    int num_threads;
    int num_processes;
    int test_duration_seconds;
    int memory_intensity;  // 1-10 scale
    int enable_context_switches;  // 0 or 1
} clean_test_config_t;

// Minimal thread data - only essential performance metrics
typedef struct {
    int thread_id;
    int process_id;
    void *memory;
    size_t memory_size;
    
    // Essential performance metrics only
    uint64_t operations_completed;
    uint64_t total_time_ns;
    uint64_t memory_bandwidth_bytes;
    
    // Control
    volatile int running;
    clean_test_config_t *config;
} clean_thread_data_t;

static volatile int global_test_running = 1;

// Minimal timing function
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Signal handler to cleanly stop test
void clean_signal_handler(int sig) {
    global_test_running = 0;
}

// High-performance memory workload thread with minimal overhead
void* clean_memory_workload(void* arg) {
    clean_thread_data_t *data = (clean_thread_data_t*)arg;
    unsigned int seed = time(NULL) ^ data->thread_id ^ getpid();
    
    // Allocate memory
    data->memory = mmap(NULL, data->memory_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (data->memory == MAP_FAILED) {
        return NULL;
    }
    
    uint32_t *mem_ptr = (uint32_t*)data->memory;
    size_t num_words = data->memory_size / sizeof(uint32_t);
    
    // Quick initialization
    for (size_t i = 0; i < num_words; i++) {
        mem_ptr[i] = (uint32_t)(data->thread_id * 0x87654321 + i);
    }
    
    // Wait for all threads to be ready (simple barrier)
    usleep(100000);  // 100ms startup delay
    
    uint64_t start_time = get_time_ns();
    data->operations_completed = 0;
    data->memory_bandwidth_bytes = 0;
    
    // Main high-performance loop - minimal overhead
    while (data->running && global_test_running) {
        // Intensive memory operations in bursts
        for (int burst = 0; burst < 100 && data->running; burst++) {
            // High-intensity memory access pattern
            for (int op = 0; op < NUM_OPERATIONS_PER_BURST; op++) {
                // Generate multiple random indices for maximum TLB pressure
                size_t idx1 = rand_r(&seed) % num_words;
                size_t idx2 = (idx1 + 1024 + (rand_r(&seed) % 2048)) % num_words;
                size_t idx3 = (idx2 + 2048 + (rand_r(&seed) % 1024)) % num_words;
                
                // Memory operations designed to stress TLB and memory subsystem
                uint32_t val1 = mem_ptr[idx1];
                uint32_t val2 = mem_ptr[idx2];
                uint32_t val3 = mem_ptr[idx3];
                
                // Compute to prevent optimization
                uint32_t result = val1 ^ val2 ^ val3 ^ data->operations_completed;
                
                // Write back to different locations
                mem_ptr[idx1] = result;
                mem_ptr[idx2] = result >> 1;
                mem_ptr[idx3] = result << 1;
                
                data->operations_completed += 3;  // 3 reads + 3 writes
                data->memory_bandwidth_bytes += 6 * sizeof(uint32_t);
            }
            
            // Optional context switch trigger
            if (data->config->enable_context_switches && (burst % 50 == 0)) {
                sched_yield();
            }
        }
    }
    
    uint64_t end_time = get_time_ns();
    data->total_time_ns = end_time - start_time;
    
    munmap(data->memory, data->memory_size);
    return NULL;
}

// Run clean performance test
void run_clean_test(clean_test_config_t *config) {
    printf("\n=== Clean ASID Performance Test ===\n");
    printf("Configuration:\n");
    printf("  - Threads per process: %d\n", config->num_threads);
    printf("  - Number of processes: %d\n", config->num_processes);
    printf("  - Test duration: %d seconds\n", config->test_duration_seconds);
    printf("  - Memory intensity: %d/10\n", config->memory_intensity);
    printf("  - Context switches: %s\n", config->enable_context_switches ? "Enabled" : "Disabled");
    printf("  - Memory per thread: %d MB\n", MEMORY_SIZE / (1024*1024));
    printf("\n");
    
    pid_t *pids = calloc(config->num_processes, sizeof(pid_t));
    uint64_t test_start_time = get_time_ns();
    
    // Start processes
    for (int p = 0; p < config->num_processes; p++) {
        pid_t pid = fork();
        
        if (pid == 0) {
            // Child process
            printf("Process %d: Starting with %d threads\n", p, config->num_threads);
            pthread_t *threads = calloc(config->num_threads, sizeof(pthread_t));
            clean_thread_data_t *thread_data = calloc(config->num_threads, sizeof(clean_thread_data_t));
            
            // Initialize and start threads
            for (int t = 0; t < config->num_threads; t++) {
                thread_data[t].thread_id = t;
                thread_data[t].process_id = p;
                thread_data[t].memory_size = MEMORY_SIZE;
                thread_data[t].running = 1;
                thread_data[t].config = config;
                
                pthread_create(&threads[t], NULL, clean_memory_workload, &thread_data[t]);
            }
            printf("Process %d: All threads created\n", p);
            
            // Run for specified duration
            printf("Process %d: Running for %d seconds...\n", p, config->test_duration_seconds);
            sleep(config->test_duration_seconds);
            
            // Stop all threads
            printf("Process %d: Stopping threads...\n", p);
            for (int t = 0; t < config->num_threads; t++) {
                thread_data[t].running = 0;
            }
            printf("Process %d: All threads signaled to stop\n", p);
            
            // Collect results
            printf("Process %d: Waiting for threads to finish...\n", p);
            uint64_t total_operations = 0;
            uint64_t total_bandwidth = 0;
            uint64_t min_time = UINT64_MAX;
            uint64_t max_time = 0;
            uint64_t total_time = 0;
            
            for (int t = 0; t < config->num_threads; t++) {
                printf("Process %d: Joining thread %d...\n", p, t);
                pthread_join(threads[t], NULL);
                printf("Process %d: Thread %d joined (ops: %lu)\n", p, t, thread_data[t].operations_completed);
                
                total_operations += thread_data[t].operations_completed;
                total_bandwidth += thread_data[t].memory_bandwidth_bytes;
                total_time += thread_data[t].total_time_ns;
                
                if (thread_data[t].total_time_ns < min_time) {
                    min_time = thread_data[t].total_time_ns;
                }
                if (thread_data[t].total_time_ns > max_time) {
                    max_time = thread_data[t].total_time_ns;
                }
            }
            printf("Process %d: All threads joined\n", p);
            
            // Calculate process-level metrics
            double avg_time_ms = (total_time / config->num_threads) / 1000000.0;
            double min_time_ms = min_time / 1000000.0;
            double max_time_ms = max_time / 1000000.0;
            double total_ops_per_sec = (double)total_operations * 1000000000.0 / (total_time / config->num_threads);
            double bandwidth_mb_per_sec = (double)total_bandwidth / (1024.0 * 1024.0) * 1000000000.0 / (total_time / config->num_threads);
            
            printf("Process %d Results:\n", p);
            printf("  Total operations: %lu\n", total_operations);
            printf("  Average time per thread: %.2f ms\n", avg_time_ms);
            printf("  Thread time range: %.2f - %.2f ms\n", min_time_ms, max_time_ms);
            printf("  Operations per second: %.0f\n", total_ops_per_sec);
            printf("  Memory bandwidth: %.1f MB/sec\n", bandwidth_mb_per_sec);
            
            free(threads);
            free(thread_data);
            printf("Process %d: Cleaned up, exiting\n", p);
            exit(0);
            
        } else if (pid > 0) {
            pids[p] = pid;
        } else {
            printf("Failed to fork process %d\n", p);
        }
    }
    
    // Wait for all processes with timeout
    printf("Waiting for %d processes to complete...\n", config->num_processes);
    for (int p = 0; p < config->num_processes; p++) {
        int status;
        printf("Waiting for process %d (PID: %d)...\n", p, pids[p]);
        
        // Use WNOHANG to avoid indefinite blocking
        pid_t result = waitpid(pids[p], &status, WNOHANG);
        int wait_time = 0;
        const int max_wait_seconds = 25;  // Maximum wait time per process
        
        while (result == 0 && wait_time < max_wait_seconds) {
            sleep(1);
            wait_time++;
            result = waitpid(pids[p], &status, WNOHANG);
            if (wait_time % 5 == 0) {
                printf("  Still waiting for process %d (%d seconds)...\n", p, wait_time);
            }
        }
        
        if (result == pids[p]) {
            printf("Process %d completed successfully\n", p);
        } else if (result == 0) {
            printf("Process %d timed out after %d seconds - terminating\n", p, max_wait_seconds);
            kill(pids[p], SIGTERM);
            sleep(1);
            waitpid(pids[p], &status, 0);  // Clean up zombie
        } else {
            printf("Error waiting for process %d\n", p);
        }
    }
    
    uint64_t test_end_time = get_time_ns();
    global_test_running = 0;
    
    printf("\n=== Overall Test Results ===\n");
    printf("Total test time: %.2f seconds\n", (test_end_time - test_start_time) / 1000000000.0);
    printf("Total processes: %d\n", config->num_processes);
    printf("Total threads: %d\n", config->num_processes * config->num_threads);
    
    free(pids);
}

// Compare multiple configurations
void run_performance_comparison(void) {
    printf("=== ASID Performance Comparison Suite ===\n");
    printf("Testing different workload configurations for performance impact\n\n");
    
    clean_test_config_t configs[] = {
        // Baseline: Single process, multiple threads
        {
            .num_threads = 8,
            .num_processes = 1,
            .test_duration_seconds = 10,
            .memory_intensity = 5,
            .enable_context_switches = 0
        },
        
        // Multi-process, moderate load
        {
            .num_threads = 4,
            .num_processes = 4,
            .test_duration_seconds = 10,
            .memory_intensity = 5,
            .enable_context_switches = 0
        },
        
        // High context switch rate
        {
            .num_threads = 8,
            .num_processes = 2,
            .test_duration_seconds = 10,
            .memory_intensity = 5,
            .enable_context_switches = 1
        },
        
        // High intensity workload
        {
            .num_threads = 6,
            .num_processes = 3,
            .test_duration_seconds = 15,
            .memory_intensity = 9,
            .enable_context_switches = 1
        },
        
        // Maximum stress test
        {
            .num_threads = 16,
            .num_processes = 4,
            .test_duration_seconds = 20,
            .memory_intensity = 10,
            .enable_context_switches = 1
        }
    };
    
    int num_configs = sizeof(configs) / sizeof(configs[0]);
    
    for (int i = 0; i < num_configs; i++) {
        printf("\n============================================================\n");
        printf("Test Configuration %d/%d\n", i + 1, num_configs);
        run_clean_test(&configs[i]);
        
        // Brief pause between tests
        if (i < num_configs - 1) {
            printf("\nPausing 2 seconds before next test...\n");
            sleep(2);
        }
    }
}

// Latency-focused micro-benchmark
void run_latency_test(void) {
    printf("\n=== Memory Access Latency Test ===\n");
    printf("Measuring memory access latency patterns under ASID management\n\n");
    
    const int num_iterations = 1000000;
    const size_t test_memory_size = 4 * 1024 * 1024;  // 4MB
    
    void *memory = mmap(NULL, test_memory_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (memory == MAP_FAILED) {
        printf("Failed to allocate memory for latency test\n");
        return;
    }
    
    uint32_t *mem_ptr = (uint32_t*)memory;
    size_t num_words = test_memory_size / sizeof(uint32_t);
    
    // Initialize memory
    for (size_t i = 0; i < num_words; i++) {
        mem_ptr[i] = (uint32_t)i;
    }
    
    printf("Memory initialized. Running latency measurements...\n");
    
    // Test different access patterns
    struct {
        const char *name;
        int stride;
        const char *description;
    } patterns[] = {
        {"Sequential", 1, "Linear memory access"},
        {"Random", 0, "Random memory access"},
        {"Strided-64", 16, "64-byte stride (cache line)"},
        {"Strided-4K", 1024, "4KB stride (page size)"},
        {"Scattered", -1, "Scattered access pattern"}
    };
    
    for (int p = 0; p < 5; p++) {
        printf("\nTesting %s access pattern: %s\n", patterns[p].name, patterns[p].description);
        
        uint64_t start_time = get_time_ns();
        uint32_t checksum = 0;
        unsigned int seed = 12345;
        
        for (int i = 0; i < num_iterations; i++) {
            size_t index;
            
            switch (patterns[p].stride) {
                case 0:  // Random
                    index = rand_r(&seed) % num_words;
                    break;
                case -1:  // Scattered
                    index = (i * 1009 + i * i * 7) % num_words;  // Pseudo-random pattern
                    break;
                default:  // Strided
                    index = (i * patterns[p].stride) % num_words;
                    break;
            }
            
            checksum ^= mem_ptr[index];
        }
        
        uint64_t end_time = get_time_ns();
        uint64_t total_time = end_time - start_time;
        
        double avg_latency_ns = (double)total_time / num_iterations;
        double ops_per_sec = (double)num_iterations * 1000000000.0 / total_time;
        
        printf("  Average latency: %.1f ns per access\n", avg_latency_ns);
        printf("  Throughput: %.0f accesses/sec\n", ops_per_sec);
        printf("  Checksum: 0x%08x (prevents optimization)\n", checksum);
    }
    
    munmap(memory, test_memory_size);
    printf("\nLatency test completed.\n");
}

int main(int argc, char *argv[]) {
    printf("ASID Clean Performance Test\n");
    printf("==========================\n");
    printf("Pure performance measurement without monitoring overhead\n\n");
    
    // Setup signal handlers
    signal(SIGINT, clean_signal_handler);
    signal(SIGTERM, clean_signal_handler);
    
    if (argc > 1) {
        if (strcmp(argv[1], "compare") == 0) {
            run_performance_comparison();
        } else if (strcmp(argv[1], "latency") == 0) {
            run_latency_test();
        } else {
            printf("Usage: %s [compare|latency]\n", argv[0]);
            printf("  compare  - Run multiple configuration comparison\n");
            printf("  latency  - Run memory access latency tests\n");
            printf("  (no arg) - Run single default test\n");
            return 1;
        }
    } else {
        // Default single test
        clean_test_config_t default_config = {
            .num_threads = 8,
            .num_processes = 4,
            .test_duration_seconds = 10,  // Reduced from 15 to avoid hangs
            .memory_intensity = 7,
            .enable_context_switches = 1
        };
        
        run_clean_test(&default_config);
    }
    
    printf("\n=== Clean Performance Test Complete ===\n");
    printf("This test provides baseline performance without monitoring overhead.\n");
    printf("Compare results with asid_efficiency_monitor to measure monitoring cost.\n");
    
    return 0;
} 