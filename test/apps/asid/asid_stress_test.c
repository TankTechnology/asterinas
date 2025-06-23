// SPDX-License-Identifier: MPL-2.0

// ASID Process Stress Test Program
// This program creates a very large number of processes (more than 4096) to stress test
// the ASID mechanism under extreme conditions. Each process performs random memory
// accesses within its own memory space to verify that memory operations remain correct
// even when the ASID space is heavily utilized.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sched.h>

// Test configuration
#define DEFAULT_NUM_PROCESSES 5000      // More than typical ASID limit
#define DEFAULT_MEMORY_SIZE (1024 * 1024) // 1MB per process
#define DEFAULT_NUM_ACCESSES 2000       // Memory accesses per process
#define PATTERN_SEED 0xDEADBEEF
#define MAX_PROCESSES 8192              // Safety limit
#define PROGRESS_INTERVAL 50            // Report progress every N processes

// Syscall for ASID profiling
#define SYS_ASID_PROFILING 999

// Global statistics shared through files
#define STATS_FILE "/tmp/asid_test_stats"

typedef struct {
    int process_id;
    size_t memory_size;
    int num_accesses;
    uint64_t errors_detected;
    uint64_t memory_operations;
    time_t start_time;
    time_t end_time;
} process_data_t;

typedef struct {
    uint64_t allocations_total;
    uint64_t deallocations_total;
    uint64_t allocation_failures;
    uint64_t generation_rollovers;
    uint64_t tlb_single_address_flushes;
    uint64_t tlb_single_context_flushes;
    uint64_t tlb_all_context_flushes;
    uint64_t tlb_full_flushes;
    uint64_t context_switches;
    uint64_t context_switches_with_flush;
    uint32_t active_asids;
    uint16_t current_generation;
    uint32_t pcid_enabled;
    uint32_t total_asids_used;
} asid_stats_t;

typedef struct {
    int total_processes;
    int completed_processes;
    int failed_processes;
    uint64_t total_operations;
    uint64_t total_errors;
} test_stats_t;

// Function to get ASID statistics
int get_asid_stats(asid_stats_t *stats) {
    long result = syscall(SYS_ASID_PROFILING, 0, stats, sizeof(asid_stats_t));
    return (result >= 0) ? 0 : -1;
}

// Function to reset ASID statistics
int reset_asid_stats(void) {
    long result = syscall(SYS_ASID_PROFILING, 2, NULL, 0);
    return (result >= 0) ? 0 : -1;
}

// Get process ID
pid_t getpid_wrapper(void) {
    return getpid();
}

// Update shared statistics
void update_stats(int completed, int failed, uint64_t operations, uint64_t errors) {
    FILE *f = fopen(STATS_FILE, "a");
    if (f) {
        fprintf(f, "%d %d %lu %lu\n", completed, failed, operations, errors);
        fclose(f);
    }
}

// Memory stress test function for each process
int run_memory_stress_test(process_data_t *data) {
    unsigned int seed = PATTERN_SEED ^ data->process_id ^ time(NULL) ^ getpid();
    uint64_t errors = 0;
    uint64_t operations = 0;
    
    data->start_time = time(NULL);
    
    printf("Process %d (PID: %d): Starting memory stress test with %zu bytes\n", 
           data->process_id, getpid(), data->memory_size);
    
    // Allocate memory with MAP_PRIVATE to ensure each process has its own copy
    void *memory = mmap(NULL, data->memory_size, PROT_READ | PROT_WRITE, 
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (memory == MAP_FAILED) {
        fprintf(stderr, "Process %d: Failed to allocate memory: %s\n", 
                data->process_id, strerror(errno));
        return -1;
    }
    
    // Initialize memory with unique pattern based on process ID
    uint32_t *mem_ptr = (uint32_t*)memory;
    size_t num_words = data->memory_size / sizeof(uint32_t);
    uint32_t base_pattern = PATTERN_SEED ^ data->process_id ^ getpid();
    
    printf("Process %d: Initializing %zu words with pattern 0x%08x\n", 
           data->process_id, num_words, base_pattern);
    
    for (size_t i = 0; i < num_words; i++) {
        mem_ptr[i] = base_pattern ^ (uint32_t)i;
        operations++;
    }
    
    printf("Process %d: Starting %d memory access iterations\n", 
           data->process_id, data->num_accesses);
    
    // Perform intensive random memory access patterns
    for (int access = 0; access < data->num_accesses; access++) {
        // Test 1: Random read/write verification
        size_t index = rand_r(&seed) % num_words;
        uint32_t expected = base_pattern ^ (uint32_t)index;
        
        // Read and verify
        uint32_t actual = mem_ptr[index];
        operations++;
        if (actual != expected) {
            errors++;
            if (errors <= 5) { // Only print first few errors
                fprintf(stderr, "Process %d: Memory error at index %zu! Expected 0x%08x, got 0x%08x\n", 
                       data->process_id, index, expected, actual);
            }
        }
        
        // Test 2: Write new pattern and verify immediately
        uint32_t new_value = expected ^ (uint32_t)access;
        mem_ptr[index] = new_value;
        operations++;
        
        if (mem_ptr[index] != new_value) {
            errors++;
        }
        
        // Test 3: Restore original and verify
        mem_ptr[index] = expected;
        operations++;
        if (mem_ptr[index] != expected) {
            errors++;
        }
        
        // Test 4: Sequential access pattern (stress TLB and page tables)
        if (access % 10 == 0) {
            size_t start_idx = rand_r(&seed) % (num_words - 128);
            for (int i = 0; i < 128; i++) {
                size_t seq_idx = start_idx + i;
                uint32_t seq_expected = base_pattern ^ (uint32_t)seq_idx;
                
                if (mem_ptr[seq_idx] != seq_expected) {
                    errors++;
                }
                operations++;
                
                // Write and read back to stress memory subsystem
                uint32_t temp_val = seq_expected ^ 0x55555555;
                mem_ptr[seq_idx] = temp_val;
                if (mem_ptr[seq_idx] != temp_val) {
                    errors++;
                }
                mem_ptr[seq_idx] = seq_expected;
                operations += 2;
            }
        }
        
        // Test 5: Large stride access (stress page table and ASID switching)
        if (access % 20 == 0) {
            size_t stride = 4096 + (rand_r(&seed) % 4096); // 4-8KB stride (cross pages)
            for (int i = 0; i < 32 && (i * stride) < num_words; i++) {
                size_t stride_idx = (i * stride) % num_words;
                uint32_t stride_expected = base_pattern ^ (uint32_t)stride_idx;
                
                if (mem_ptr[stride_idx] != stride_expected) {
                    errors++;
                }
                operations++;
                
                // Perform some computation to increase process switching likelihood
                volatile uint32_t dummy = 0;
                for (int j = 0; j < 100; j++) {
                    dummy += stride_expected + j;
                }
            }
        }
        
        // Test 6: Force context switches to stress ASID mechanism
        if (access % 50 == 0) {
            sched_yield(); // Encourage scheduler to switch to other processes
            
            // Do a system call to force kernel entry/exit
            getpid();
        }
        
        // Progress report for long-running processes
        if (access % 500 == 0) {
            printf("Process %d: Completed %d/%d iterations, %lu errors so far\n", 
                   data->process_id, access, data->num_accesses, errors);
        }
    }
    
    data->end_time = time(NULL);
    data->errors_detected = errors;
    data->memory_operations = operations;
    
    // Final comprehensive memory verification
    printf("Process %d: Performing final memory verification...\n", data->process_id);
    for (size_t i = 0; i < num_words; i++) {
        uint32_t expected = base_pattern ^ (uint32_t)i;
        if (mem_ptr[i] != expected) {
            errors++;
            if (errors <= 10) {
                fprintf(stderr, "Process %d: Final check error at index %zu! Expected 0x%08x, got 0x%08x\n", 
                       data->process_id, i, expected, mem_ptr[i]);
            }
        }
        operations++;
    }
    
    // Clean up
    if (munmap(memory, data->memory_size) != 0) {
        fprintf(stderr, "Process %d: Failed to unmap memory: %s\n", 
                data->process_id, strerror(errno));
    }
    
    // Update global statistics
    update_stats(errors == 0 ? 1 : 0, errors == 0 ? 0 : 1, operations, errors);
    
    printf("Process %d: Completed in %ld seconds, %lu operations, %lu errors\n", 
           data->process_id, data->end_time - data->start_time, operations, errors);
    
    return (errors == 0) ? 0 : -1;
}

// Function to read statistics from file
void read_stats(test_stats_t *stats) {
    FILE *f = fopen(STATS_FILE, "r");
    if (!f) {
        memset(stats, 0, sizeof(test_stats_t));
        return;
    }
    
    memset(stats, 0, sizeof(test_stats_t));
    int completed, failed;
    uint64_t operations, errors;
    
    while (fscanf(f, "%d %d %lu %lu", &completed, &failed, &operations, &errors) == 4) {
        stats->completed_processes += completed;
        stats->failed_processes += failed;
        stats->total_operations += operations;
        stats->total_errors += errors;
    }
    
    fclose(f);
}

void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -n <num>    Number of processes to spawn (default: %d)\n", DEFAULT_NUM_PROCESSES);
    printf("  -m <size>   Memory size per process in KB (default: %d)\n", DEFAULT_MEMORY_SIZE / 1024);
    printf("  -a <num>    Number of memory accesses per process (default: %d)\n", DEFAULT_NUM_ACCESSES);
    printf("  -s          Show ASID statistics before and after test\n");
    printf("  -r          Reset ASID statistics before test\n");
    printf("  -b <num>    Batch size - spawn processes in batches (default: 100)\n");
    printf("  -h          Show this help message\n");
    printf("\nThis stress test creates many processes to test ASID management under extreme load.\n");
    printf("Each process gets its own address space and ASID, stressing the ASID allocation mechanism.\n");
}

int main(int argc, char *argv[]) {
    int num_processes = DEFAULT_NUM_PROCESSES;
    size_t memory_size = DEFAULT_MEMORY_SIZE;
    int num_accesses = DEFAULT_NUM_ACCESSES;
    int show_stats = 0;
    int reset_stats = 0;
    int batch_size = 100;
    int opt;
    
    // Parse command line arguments
    while ((opt = getopt(argc, argv, "n:m:a:srb:h")) != -1) {
        switch (opt) {
            case 'n':
                num_processes = atoi(optarg);
                if (num_processes <= 0 || num_processes > MAX_PROCESSES) {
                    fprintf(stderr, "Invalid process count: %d (must be 1-%d)\n", num_processes, MAX_PROCESSES);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'm':
                memory_size = atoi(optarg) * 1024; // Convert KB to bytes
                if (memory_size < 1024 || memory_size > 100*1024*1024) {
                    fprintf(stderr, "Invalid memory size: %zu bytes\n", memory_size);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'a':
                num_accesses = atoi(optarg);
                if (num_accesses <= 0) {
                    fprintf(stderr, "Invalid access count: %d\n", num_accesses);
                    exit(EXIT_FAILURE);
                }
                break;
            case 's':
                show_stats = 1;
                break;
            case 'r':
                reset_stats = 1;
                break;
            case 'b':
                batch_size = atoi(optarg);
                if (batch_size <= 0 || batch_size > 1000) {
                    fprintf(stderr, "Invalid batch size: %d (must be 1-1000)\n", batch_size);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'h':
                print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    
    printf("=== ASID Process Stress Test ===\n");
    printf("Configuration:\n");
    printf("  Total processes:     %d\n", num_processes);
    printf("  Memory per process:  %zu KB\n", memory_size / 1024);
    printf("  Accesses per process: %d\n", num_accesses);
    printf("  Total memory:        %zu MB\n", (num_processes * memory_size) / (1024 * 1024));
    printf("  Batch size:          %d\n", batch_size);
    printf("\n");
    
    // Clean up any existing stats file
    unlink(STATS_FILE);
    
    // Show initial ASID statistics
    if (show_stats) {
        asid_stats_t stats;
        if (get_asid_stats(&stats) == 0) {
            printf("=== Initial ASID Statistics ===\n");
            printf("Active ASIDs:         %u\n", stats.active_asids);
            printf("Current Generation:   %u\n", stats.current_generation);
            printf("Total ASIDs Used:     %u\n", stats.total_asids_used);
            printf("Generation Rollovers: %lu\n", stats.generation_rollovers);
            printf("PCID Enabled:         %s\n", stats.pcid_enabled ? "Yes" : "No");
            printf("\n");
        } else {
            printf("Failed to get initial ASID statistics\n");
        }
    }
    
    // Reset statistics if requested
    if (reset_stats) {
        if (reset_asid_stats() == 0) {
            printf("ASID statistics reset\n\n");
        } else {
            printf("Failed to reset ASID statistics\n");
        }
    }
    
    time_t test_start_time = time(NULL);
    
    // Spawn processes in batches to avoid overwhelming the system
    pid_t *child_pids = malloc(num_processes * sizeof(pid_t));
    if (!child_pids) {
        fprintf(stderr, "Failed to allocate memory for process tracking\n");
        exit(EXIT_FAILURE);
    }
    
    int processes_spawned = 0;
    int batch_count = 0;
    
    while (processes_spawned < num_processes) {
        int current_batch_size = (num_processes - processes_spawned > batch_size) ? 
                                batch_size : (num_processes - processes_spawned);
        
        printf("Spawning batch %d: processes %d-%d\n", 
               batch_count, processes_spawned, processes_spawned + current_batch_size - 1);
        
        // Fork processes in current batch
        for (int i = 0; i < current_batch_size; i++) {
            int process_index = processes_spawned + i;
            pid_t pid = fork();
            
            if (pid == 0) {
                // Child process
                process_data_t data;
                data.process_id = process_index;
                data.memory_size = memory_size;
                data.num_accesses = num_accesses;
                
                int result = run_memory_stress_test(&data);
                exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
            } else if (pid > 0) {
                child_pids[process_index] = pid;
            } else {
                fprintf(stderr, "Failed to fork process %d: %s\n", process_index, strerror(errno));
                child_pids[process_index] = -1;
            }
        }
        
        processes_spawned += current_batch_size;
        batch_count++;
        
        // Small delay between batches to avoid overwhelming the system
        if (processes_spawned < num_processes) {
            printf("Batch %d spawned, waiting 2 seconds before next batch...\n", batch_count);
            sleep(2);
        }
    }
    
    printf("All %d processes spawned, waiting for completion...\n", num_processes);
    
    // Wait for all child processes
    int completed_count = 0;
    for (int i = 0; i < num_processes; i++) {
        if (child_pids[i] > 0) {
            int status;
            if (waitpid(child_pids[i], &status, 0) == child_pids[i]) {
                completed_count++;
                if (completed_count % PROGRESS_INTERVAL == 0) {
                    printf("Completed: %d/%d processes\n", completed_count, num_processes);
                }
            }
        }
    }
    
    time_t test_end_time = time(NULL);
    
    // Read final statistics
    test_stats_t final_stats;
    read_stats(&final_stats);
    
    // Show final ASID statistics
    if (show_stats) {
        asid_stats_t stats;
        if (get_asid_stats(&stats) == 0) {
            printf("\n=== Final ASID Statistics ===\n");
            printf("Active ASIDs:         %u\n", stats.active_asids);
            printf("Current Generation:   %u\n", stats.current_generation);
            printf("Total ASIDs Used:     %u\n", stats.total_asids_used);
            printf("Generation Rollovers: %lu\n", stats.generation_rollovers);
            printf("Context Switches:     %lu\n", stats.context_switches);
            printf("TLB Flushes:          %lu\n", stats.tlb_single_address_flushes + 
                                                    stats.tlb_single_context_flushes + 
                                                    stats.tlb_all_context_flushes + 
                                                    stats.tlb_full_flushes);
            printf("\n");
        }
    }
    
    // Print final summary
    printf("=== Final Results ===\n");
    printf("Test Duration:        %ld seconds\n", test_end_time - test_start_time);
    printf("Processes Spawned:    %d\n", num_processes);
    printf("Processes Completed:  %d\n", final_stats.completed_processes);
    printf("Processes Failed:     %d\n", final_stats.failed_processes);
    printf("Total Memory Ops:     %lu\n", final_stats.total_operations);
    printf("Total Errors:         %lu\n", final_stats.total_errors);
    
    if (final_stats.total_operations > 0) {
        printf("Error Rate:           %.2e\n", (double)final_stats.total_errors / final_stats.total_operations);
    }
    
    // Cleanup
    free(child_pids);
    unlink(STATS_FILE);
    
    if (final_stats.completed_processes == num_processes && final_stats.total_errors == 0) {
        printf("✅ PROCESS STRESS TEST PASSED - ASID mechanism handled extreme load correctly\n");
        return EXIT_SUCCESS;
    } else {
        printf("❌ PROCESS STRESS TEST FAILED - Issues detected under extreme load\n");
        return EXIT_FAILURE;
    }
} 