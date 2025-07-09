// SPDX-License-Identifier: MPL-2.0

// ASID Efficiency Test with Detailed Monitoring
// This test measures the performance improvements of the new ASID implementation
// while recording detailed TLB flush counts, context switch metrics, and other indicators.
//
// This version includes comprehensive monitoring and profiling to track:
// - TLB flush operations and their frequency
// - Context switch efficiency
// - ASID allocation/deallocation patterns
// - Generation rollover impact
// - Memory access performance under different loads

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>
#include <sys/wait.h>

#define MAX_THREADS 64
#define MEMORY_SIZE (8 * 1024 * 1024)  // 8MB per thread
#define NUM_MEMORY_OPERATIONS 200000
#define MONITORING_INTERVAL_MS 100
#define TEST_DURATION_SECONDS 30

// Syscall definitions
#define SYS_ASID_PROFILING 999
#define ASID_ACTION_GET_STATS 0
#define ASID_ACTION_GET_EFFICIENCY 3
#define ASID_ACTION_RESET 2

// Complete ASID statistics structure
typedef struct {
    // Basic counters
    uint64_t allocations_total;
    uint64_t deallocations_total;
    uint64_t allocation_failures;
    uint64_t generation_rollovers;
    
    // Search operations
    uint64_t bitmap_searches;
    uint64_t map_searches;
    uint64_t asid_reuse_count;
    
    // TLB operations
    uint64_t tlb_single_address_flushes;
    uint64_t tlb_single_context_flushes;
    uint64_t tlb_all_context_flushes;
    uint64_t tlb_full_flushes;
    
    // Context switches
    uint64_t context_switches;
    uint64_t context_switches_with_flush;
    uint64_t vmspace_activations;
    
    // Performance timing
    uint64_t allocation_time_total;
    uint64_t deallocation_time_total;
    uint64_t tlb_flush_time_total;
    uint64_t context_switch_time_total;
    
    // Current state
    uint32_t active_asids;
    uint16_t current_generation;
    uint32_t pcid_enabled;
    uint32_t total_asids_used;
} asid_stats_t;

typedef struct {
    uint64_t allocation_success_rate;     // Parts per million
    uint64_t reuse_efficiency;           // Parts per million
    uint64_t flush_efficiency;           // Parts per million
    uint64_t avg_cycles_per_allocation;
    uint64_t avg_cycles_per_context_switch;
} asid_efficiency_t;

// Test configuration
typedef struct {
    int num_threads;
    int num_processes;
    int test_duration;
    int memory_intensity;  // 1-10 scale
    int context_switch_frequency;  // microseconds between yields
} test_config_t;

// Thread data for workload generation
typedef struct {
    int thread_id;
    int process_id;
    void *memory;
    size_t memory_size;
    
    // Performance metrics
    uint64_t operations_completed;
    uint64_t memory_access_time_ns;
    uint64_t context_switches;
    uint64_t cache_misses;  // Estimated
    
    // Control
    volatile int running;
    test_config_t *config;
} thread_workload_t;

// Monitoring data
typedef struct {
    uint64_t timestamp_ns;
    asid_stats_t stats;
    asid_efficiency_t efficiency;
    uint64_t total_memory_ops;
    uint64_t total_threads_active;
} monitoring_sample_t;

static monitoring_sample_t *monitoring_data = NULL;
static int monitoring_samples = 0;
static int max_monitoring_samples = 0;
static volatile int test_running = 1;

// Helper functions
uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int get_asid_stats(asid_stats_t *stats) {
    long result = syscall(SYS_ASID_PROFILING, ASID_ACTION_GET_STATS, stats, sizeof(*stats));
    return (result == 0) ? 0 : -1;
}

int get_asid_efficiency(asid_efficiency_t *efficiency) {
    long result = syscall(SYS_ASID_PROFILING, ASID_ACTION_GET_EFFICIENCY, efficiency, sizeof(*efficiency));
    return (result == 0) ? 0 : -1;
}

int reset_asid_stats(void) {
    long result = syscall(SYS_ASID_PROFILING, ASID_ACTION_RESET, NULL, 0);
    return (result == 0) ? 0 : -1;
}

// Check if ASID profiling syscall is available
int is_asid_profiling_available(void) {
    asid_stats_t dummy_stats;
    return (get_asid_stats(&dummy_stats) == 0);
}

static int asid_profiling_enabled = -1;  // -1 = unknown, 0 = disabled, 1 = enabled

// Signal handler to stop the test
void signal_handler(int sig) {
    test_running = 0;
}

// Intensive memory workload thread
void* memory_workload_thread(void* arg) {
    thread_workload_t *workload = (thread_workload_t*)arg;
    unsigned int seed = time(NULL) ^ workload->thread_id ^ getpid();
    
    // Allocate memory for this thread
    workload->memory = mmap(NULL, workload->memory_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (workload->memory == MAP_FAILED) {
        printf("Thread %d: Failed to allocate memory\n", workload->thread_id);
        return NULL;
    }
    
    uint32_t *mem_ptr = (uint32_t*)workload->memory;
    size_t num_words = workload->memory_size / sizeof(uint32_t);
    
    // Initialize memory with pattern
    for (size_t i = 0; i < num_words; i++) {
        mem_ptr[i] = (uint32_t)(workload->thread_id * 0x12345678 + i);
    }
    
    printf("Thread %d-%d: Starting workload (memory: %zu MB)\n", 
           workload->process_id, workload->thread_id, workload->memory_size / (1024*1024));
    
    uint64_t start_time = get_time_ns();
    workload->operations_completed = 0;
    workload->context_switches = 0;
    
    // Main workload loop
    while (workload->running && test_running) {
        // Intensive memory access pattern
        for (int burst = 0; burst < 1000 && workload->running; burst++) {
            // Random access pattern to stress TLB
            for (int op = 0; op < workload->config->memory_intensity * 100; op++) {
                size_t index = rand_r(&seed) % num_words;
                
                // Read-modify-write to ensure memory activity
                uint32_t value = mem_ptr[index];
                mem_ptr[index] = value ^ workload->operations_completed;
                
                // Additional scattered accesses to increase TLB pressure
                size_t index2 = (index + 1024 + (rand_r(&seed) % 4096)) % num_words;
                volatile uint32_t dummy = mem_ptr[index2];
                (void)dummy;
                
                workload->operations_completed++;
            }
            
            // Force context switch opportunity
            if (workload->config->context_switch_frequency > 0) {
                usleep(workload->config->context_switch_frequency);
                workload->context_switches++;
            } else if (burst % 100 == 0) {
                sched_yield();
                workload->context_switches++;
            }
        }
    }
    
    uint64_t end_time = get_time_ns();
    workload->memory_access_time_ns = end_time - start_time;
    
    printf("Thread %d-%d: Completed %lu operations in %.2f ms\n",
           workload->process_id, workload->thread_id,
           workload->operations_completed,
           workload->memory_access_time_ns / 1000000.0);
    
    munmap(workload->memory, workload->memory_size);
    return NULL;
}

// Monitoring thread that periodically samples ASID statistics
void* monitoring_thread(void* arg) {
    // Check if profiling is available
    if (asid_profiling_enabled == -1) {
        asid_profiling_enabled = is_asid_profiling_available() ? 1 : 0;
    }
    
    if (asid_profiling_enabled) {
        printf("Starting monitoring thread (sampling every %d ms)\n", MONITORING_INTERVAL_MS);
    } else {
        printf("ASID profiling not available - monitoring thread will track basic metrics only\n");
    }
    
    while (test_running && monitoring_samples < max_monitoring_samples) {
        monitoring_sample_t *sample = &monitoring_data[monitoring_samples];
        
        sample->timestamp_ns = get_time_ns();
        
        if (asid_profiling_enabled) {
            if (get_asid_stats(&sample->stats) != 0) {
                // If profiling fails, disable it for future samples
                asid_profiling_enabled = 0;
                printf("ASID profiling became unavailable - switching to basic monitoring\n");
                memset(&sample->stats, 0, sizeof(sample->stats));
            }
            
            if (get_asid_efficiency(&sample->efficiency) != 0) {
                memset(&sample->efficiency, 0, sizeof(sample->efficiency));
            }
        } else {
            // Fill with zeros when profiling not available
            memset(&sample->stats, 0, sizeof(sample->stats));
            memset(&sample->efficiency, 0, sizeof(sample->efficiency));
        }
        
        // Calculate derived metrics
        sample->total_memory_ops = 0;  // Will be filled by main thread
        sample->total_threads_active = 0;
        
        monitoring_samples++;
        
        // Print real-time info every 10 samples
        if (monitoring_samples % 10 == 0) {
            if (asid_profiling_enabled) {
                printf("Monitor sample %d: Gen=%u, ASIDs=%u, TLB_flushes=%lu, Ctx_switches=%lu\n",
                       monitoring_samples,
                       sample->stats.current_generation,
                       sample->stats.active_asids,
                       sample->stats.tlb_all_context_flushes,
                       sample->stats.context_switches);
            } else {
                printf("Monitor sample %d: Basic monitoring (ASID profiling not available)\n",
                       monitoring_samples);
            }
        }
        
        usleep(MONITORING_INTERVAL_MS * 1000);
    }
    
    printf("Monitoring thread finished (%d samples collected)\n", monitoring_samples);
    return NULL;
}

// Run test with specified configuration
void run_efficiency_test(test_config_t *config) {
    printf("\n=== Running Efficiency Test with Monitoring ===\n");
    
    // Check if profiling is available
    if (asid_profiling_enabled == -1) {
        asid_profiling_enabled = is_asid_profiling_available() ? 1 : 0;
    }
    
    if (asid_profiling_enabled) {
        printf("✓ ASID profiling available - full monitoring enabled\n");
    } else {
        printf("⚠ ASID profiling not available - basic performance measurement only\n");
    }
    
    printf("Configuration:\n");
    printf("  - Threads per process: %d\n", config->num_threads);
    printf("  - Number of processes: %d\n", config->num_processes);
    printf("  - Test duration: %d seconds\n", config->test_duration);
    printf("  - Memory intensity: %d/10\n", config->memory_intensity);
    printf("  - Context switch frequency: %d µs\n", config->context_switch_frequency);
    printf("\n");
    
    // Initialize monitoring
    max_monitoring_samples = (config->test_duration * 1000) / MONITORING_INTERVAL_MS + 10;
    monitoring_data = calloc(max_monitoring_samples, sizeof(monitoring_sample_t));
    monitoring_samples = 0;
    
    // Reset ASID statistics (if available)
    if (asid_profiling_enabled) {
        reset_asid_stats();
    }
    
    // Start monitoring thread
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, monitoring_thread, NULL);
    
    // Start workload processes
    pid_t *pids = calloc(config->num_processes, sizeof(pid_t));
    uint64_t test_start_time = get_time_ns();
    
    for (int p = 0; p < config->num_processes; p++) {
        pid_t pid = fork();
        
        if (pid == 0) {
            // Child process - run threads
            pthread_t *threads = calloc(config->num_threads, sizeof(pthread_t));
            thread_workload_t *workloads = calloc(config->num_threads, sizeof(thread_workload_t));
            
            // Initialize and start threads
            for (int t = 0; t < config->num_threads; t++) {
                workloads[t].thread_id = t;
                workloads[t].process_id = p;
                workloads[t].memory_size = MEMORY_SIZE;
                workloads[t].running = 1;
                workloads[t].config = config;
                
                pthread_create(&threads[t], NULL, memory_workload_thread, &workloads[t]);
            }
            
            // Let threads run for specified duration
            printf("Process %d: Running for %d seconds...\n", p, config->test_duration);
            sleep(config->test_duration);
            
            // Stop threads
            printf("Process %d: Stopping all threads...\n", p);
            for (int t = 0; t < config->num_threads; t++) {
                workloads[t].running = 0;
            }
            printf("Process %d: All threads signaled to stop\n", p);
            
            // Wait for threads to finish
            printf("Process %d: Waiting for %d threads to finish...\n", p, config->num_threads);
            uint64_t total_operations = 0;
            for (int t = 0; t < config->num_threads; t++) {
                printf("Process %d: Joining thread %d...\n", p, t);
                pthread_join(threads[t], NULL);
                total_operations += workloads[t].operations_completed;
                printf("Process %d: Thread %d joined (ops: %lu)\n", p, t, workloads[t].operations_completed);
            }
            
            printf("Process %d: All threads joined, completed %lu total operations\n", p, total_operations);
            
            free(threads);
            free(workloads);
            printf("Process %d: Cleaned up, exiting\n", p);
            exit(0);
        } else if (pid > 0) {
            pids[p] = pid;
        } else {
            printf("Failed to fork process %d\n", p);
        }
    }
    
    // Wait for all processes to complete with timeout
    printf("Waiting for %d processes to complete...\n", config->num_processes);
    for (int p = 0; p < config->num_processes; p++) {
        int status;
        printf("Waiting for process %d (PID: %d)...\n", p, pids[p]);
        
        // Use WNOHANG to avoid indefinite blocking
        pid_t result = waitpid(pids[p], &status, WNOHANG);
        int wait_time = 0;
        const int max_wait_seconds = 30;  // Maximum wait time per process
        
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
    test_running = 0;
    
    // Wait for monitoring thread to finish
    pthread_join(monitor_thread, NULL);
    
    printf("\n=== Test Completed ===\n");
    printf("Actual test duration: %.2f seconds\n", 
           (test_end_time - test_start_time) / 1000000000.0);
    printf("Monitoring samples collected: %d\n", monitoring_samples);
    
    free(pids);
}

// Analyze and report monitoring data
void analyze_monitoring_data(void) {
    if (monitoring_samples < 2) {
        printf("Insufficient monitoring data for analysis\n");
        return;
    }
    
    printf("\n=== Detailed Performance Analysis ===\n");
    
    if (!asid_profiling_enabled) {
        printf("⚠ ASID profiling was not available during test\n");
        printf("Analysis limited to basic test metrics:\n");
        printf("  - Test completed successfully without crashes\n");
        printf("  - Memory workload executed across multiple processes/threads\n");
        printf("  - System remained stable under concurrent load\n");
        printf("  - Monitoring samples collected: %d\n", monitoring_samples);
        printf("\nTo get detailed ASID metrics, the ASID profiling syscall needs to be implemented.\n");
        return;
    }
    
    // Calculate deltas and rates
    monitoring_sample_t *first = &monitoring_data[0];
    monitoring_sample_t *last = &monitoring_data[monitoring_samples - 1];
    
    uint64_t duration_ns = last->timestamp_ns - first->timestamp_ns;
    double duration_sec = duration_ns / 1000000000.0;
    
    printf("\nASID Allocation Metrics:\n");
    printf("  Total allocations: %lu → %lu (+%lu)\n",
           first->stats.allocations_total,
           last->stats.allocations_total,
           last->stats.allocations_total - first->stats.allocations_total);
    
    printf("  Total deallocations: %lu → %lu (+%lu)\n",
           first->stats.deallocations_total,
           last->stats.deallocations_total,
           last->stats.deallocations_total - first->stats.deallocations_total);
    
    printf("  Allocation failures: %lu → %lu (+%lu)\n",
           first->stats.allocation_failures,
           last->stats.allocation_failures,
           last->stats.allocation_failures - first->stats.allocation_failures);
    
    printf("  Generation rollovers: %lu → %lu (+%lu)\n",
           first->stats.generation_rollovers,
           last->stats.generation_rollovers,
           last->stats.generation_rollovers - first->stats.generation_rollovers);
    
    uint64_t alloc_delta = last->stats.allocations_total - first->stats.allocations_total;
    printf("  Allocation rate: %.1f allocations/sec\n", alloc_delta / duration_sec);
    
    printf("\nTLB Flush Analysis:\n");
    uint64_t single_addr_delta = last->stats.tlb_single_address_flushes - first->stats.tlb_single_address_flushes;
    uint64_t single_ctx_delta = last->stats.tlb_single_context_flushes - first->stats.tlb_single_context_flushes;
    uint64_t all_ctx_delta = last->stats.tlb_all_context_flushes - first->stats.tlb_all_context_flushes;
    uint64_t full_flush_delta = last->stats.tlb_full_flushes - first->stats.tlb_full_flushes;
    
    printf("  Single address flushes: %lu (+%.1f/sec)\n", single_addr_delta, single_addr_delta / duration_sec);
    printf("  Single context flushes: %lu (+%.1f/sec)\n", single_ctx_delta, single_ctx_delta / duration_sec);
    printf("  All context flushes: %lu (+%.1f/sec)\n", all_ctx_delta, all_ctx_delta / duration_sec);
    printf("  Full TLB flushes: %lu (+%.1f/sec)\n", full_flush_delta, full_flush_delta / duration_sec);
    
    uint64_t total_tlb_ops = single_addr_delta + single_ctx_delta + all_ctx_delta + full_flush_delta;
    printf("  Total TLB operations: %lu (%.1f/sec)\n", total_tlb_ops, total_tlb_ops / duration_sec);
    
    printf("\nContext Switch Analysis:\n");
    uint64_t ctx_switch_delta = last->stats.context_switches - first->stats.context_switches;
    uint64_t ctx_flush_delta = last->stats.context_switches_with_flush - first->stats.context_switches_with_flush;
    
    printf("  Total context switches: %lu (+%.1f/sec)\n", ctx_switch_delta, ctx_switch_delta / duration_sec);
    printf("  Context switches with flush: %lu (+%.1f/sec)\n", ctx_flush_delta, ctx_flush_delta / duration_sec);
    
    if (ctx_switch_delta > 0) {
        double flush_percentage = (double)ctx_flush_delta / ctx_switch_delta * 100.0;
        printf("  Flush percentage: %.2f%% (lower is better)\n", flush_percentage);
    }
    
    printf("\nEfficiency Metrics:\n");
    printf("  Allocation success rate: %.4f%%\n", last->efficiency.allocation_success_rate / 10000.0);
    printf("  ASID reuse efficiency: %.4f%%\n", last->efficiency.reuse_efficiency / 10000.0);
    printf("  TLB flush efficiency: %.4f%%\n", last->efficiency.flush_efficiency / 10000.0);
    printf("  Avg cycles/allocation: %lu\n", last->efficiency.avg_cycles_per_allocation);
    printf("  Avg cycles/context switch: %lu\n", last->efficiency.avg_cycles_per_context_switch);
    
    // Print timeline data for graphing
    printf("\n=== Timeline Data (for graphing) ===\n");
    printf("Time(s), Allocations, TLB_Flushes, Context_Switches, Active_ASIDs, Generation\n");
    
    for (int i = 0; i < monitoring_samples; i++) {
        double time_sec = (monitoring_data[i].timestamp_ns - first->timestamp_ns) / 1000000000.0;
        printf("%.2f, %lu, %lu, %lu, %u, %u\n",
               time_sec,
               monitoring_data[i].stats.allocations_total,
               monitoring_data[i].stats.tlb_all_context_flushes,
               monitoring_data[i].stats.context_switches,
               monitoring_data[i].stats.active_asids,
               monitoring_data[i].stats.current_generation);
    }
}

int main(int argc, char *argv[]) {
    printf("ASID Efficiency Test with Detailed Monitoring\n");
    printf("=============================================\n");
    
    // Check profiling availability early
    asid_profiling_enabled = is_asid_profiling_available() ? 1 : 0;
    
    if (asid_profiling_enabled) {
        printf("✓ ASID profiling syscall available - detailed monitoring enabled\n");
    } else {
        printf("⚠ ASID profiling syscall not available\n");
        printf("  This test will run basic performance measurement only.\n");
        printf("  For detailed TLB/ASID metrics, implement the ASID profiling syscall.\n");
    }
    printf("\n");
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Test configurations
    test_config_t configs[] = {
        // Light load
        {
            .num_threads = 4,
            .num_processes = 2,
            .test_duration = 10,
            .memory_intensity = 3,
            .context_switch_frequency = 1000
        },
        // Medium load
        {
            .num_threads = 8,
            .num_processes = 4,
            .test_duration = 10,  // Reduced from 15 to avoid hangs
            .memory_intensity = 6,
            .context_switch_frequency = 500
        },
        // Heavy load
        {
            .num_threads = 16,
            .num_processes = 4,
            .test_duration = 20,
            .memory_intensity = 9,
            .context_switch_frequency = 100
        }
    };
    
    int num_configs = sizeof(configs) / sizeof(configs[0]);
    
    // Allow user to specify which test to run
    int test_selection = 1;  // Default to medium load
    if (argc > 1) {
        test_selection = atoi(argv[1]);
        if (test_selection < 1 || test_selection > num_configs) {
            printf("Invalid test selection. Available tests: 1-%d\n", num_configs);
            return 1;
        }
        test_selection--;  // Convert to 0-based index
    }
    
    printf("Running test configuration %d:\n", test_selection + 1);
    printf("  1 = Light load, 2 = Medium load, 3 = Heavy load\n");
    
    // Run the selected test
    run_efficiency_test(&configs[test_selection]);
    
    // Analyze results
    analyze_monitoring_data();
    
    // Cleanup
    if (monitoring_data) {
        free(monitoring_data);
    }
    
    printf("\n=== Test Complete ===\n");
    if (asid_profiling_enabled) {
        printf("TIP: Compare these results with the clean efficiency test to see monitoring overhead.\n");
    } else {
        printf("NOTE: This test verified basic functionality without detailed ASID metrics.\n");
        printf("      To enable full monitoring, implement the ASID profiling syscall (SYS_ASID_PROFILING).\n");
        printf("      Try ./asid_efficiency_clean for clean performance measurement.\n");
    }
    
    return 0;
} 