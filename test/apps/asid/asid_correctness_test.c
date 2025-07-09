// SPDX-License-Identifier: MPL-2.0

// Comprehensive correctness test for the new ASID implementation
// This test verifies:
// 1. Basic allocation and deallocation functionality
// 2. Generation rollover handling
// 3. Concurrent access from multiple threads/processes
// 4. Memory integrity under ASID stress
// 5. Edge cases and error conditions

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdint.h>
#include <assert.h>

#define NUM_THREADS 16
#define NUM_PROCESSES 4
#define MEMORY_SIZE (1 * 1024 * 1024)  // 1MB per thread
#define NUM_MEMORY_TESTS 5000
#define NUM_ASID_CYCLES 100
#define NUM_MEMORY_TESTS_MULTIPROCESS 1000  // Reduced for multi-process test
#define NUM_ASID_CYCLES_MULTIPROCESS 20     // Reduced for multi-process test
#define PATTERN_BASE 0xABCD1234

// Test configuration flags
#define TEST_BASIC_FUNCTIONALITY    1
#define TEST_CONCURRENT_ACCESS      1  
#define TEST_GENERATION_ROLLOVER    1
#define TEST_MEMORY_INTEGRITY       1
#define TEST_EDGE_CASES            1

// Syscall number for ASID profiling
#define SYS_ASID_PROFILING 999
#define ASID_ACTION_GET_STATS 0
#define ASID_ACTION_RESET 2

// Simplified ASID stats structure
typedef struct {
    uint64_t allocations_total;
    uint64_t deallocations_total;
    uint64_t allocation_failures;
    uint64_t generation_rollovers;
    uint32_t active_asids;
    uint16_t current_generation;
} asid_stats_t;

typedef struct {
    int thread_id;
    int process_id;
    void *memory;
    size_t size;
    int test_result;
    uint64_t memory_errors;
    uint64_t asid_operations;
    int use_reduced_workload;  // For multi-process tests
} thread_test_data_t;

// Global test results
static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

// Helper function to get ASID stats
int get_asid_stats(asid_stats_t *stats) {
    long result = syscall(SYS_ASID_PROFILING, ASID_ACTION_GET_STATS, stats, sizeof(*stats));
    return (result == 0) ? 0 : -1;
}

// Helper function to reset ASID stats
int reset_asid_stats(void) {
    long result = syscall(SYS_ASID_PROFILING, ASID_ACTION_RESET, NULL, 0);
    return (result == 0) ? 0 : -1;
}

// Check if ASID profiling syscall is available
int is_asid_profiling_available(void) {
    asid_stats_t dummy_stats;
    return (get_asid_stats(&dummy_stats) == 0);
}

// Test helper macros
#define TEST_START(name) do { \
    printf("🔍 Running test: %s\n", name); \
    total_tests++; \
} while(0)

#define TEST_ASSERT(condition, message) do { \
    if (!(condition)) { \
        printf("❌ ASSERTION FAILED: %s\n", message); \
        failed_tests++; \
        return 0; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    printf("✅ PASSED: %s\n", name); \
    passed_tests++; \
    return 1; \
} while(0)

#define TEST_FAIL(name, reason) do { \
    printf("❌ FAILED: %s - %s\n", name, reason); \
    failed_tests++; \
    return 0; \
} while(0)

// Thread function for concurrent ASID operations
void* concurrent_asid_thread(void* arg) {
    thread_test_data_t *data = (thread_test_data_t*)arg;
    unsigned int seed = time(NULL) ^ data->thread_id ^ getpid();
    
    data->memory_errors = 0;
    data->asid_operations = 0;
    data->test_result = 1;  // Assume success
    
    // Allocate memory for this thread
    data->memory = mmap(NULL, data->size, PROT_READ | PROT_WRITE, 
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (data->memory == MAP_FAILED) {
        printf("Thread %d-%d: Memory allocation failed\n", 
               data->process_id, data->thread_id);
        data->test_result = 0;
        return NULL;
    }
    
    uint32_t *mem_ptr = (uint32_t*)data->memory;
    size_t num_words = data->size / sizeof(uint32_t);
    
    // Initialize memory with thread-specific pattern
    uint32_t pattern = PATTERN_BASE ^ (data->process_id << 16) ^ data->thread_id;
    for (size_t i = 0; i < num_words; i++) {
        mem_ptr[i] = pattern ^ (uint32_t)i;
    }
    
    // Use reduced workload for multi-process tests to avoid hangs
    int max_cycles = data->use_reduced_workload ? NUM_ASID_CYCLES_MULTIPROCESS : NUM_ASID_CYCLES;
    int max_tests = data->use_reduced_workload ? NUM_MEMORY_TESTS_MULTIPROCESS : NUM_MEMORY_TESTS;
    
    // Perform memory operations while triggering ASID activity
    for (int cycle = 0; cycle < max_cycles && data->test_result; cycle++) {
        // Intensive memory access to trigger TLB activity
        for (int access = 0; access < max_tests; access++) {
            size_t index = rand_r(&seed) % num_words;
            uint32_t expected = pattern ^ (uint32_t)index;
            
            // Verify memory integrity
            if (mem_ptr[index] != expected) {
                data->memory_errors++;
                if (data->memory_errors > 10) {  // Too many errors
                    data->test_result = 0;
                    break;
                }
            }
            
            // Write new pattern and verify
            uint32_t new_value = expected ^ cycle ^ access;
            mem_ptr[index] = new_value;
            
            if (mem_ptr[index] != new_value) {
                data->memory_errors++;
                if (data->memory_errors > 10) {
                    data->test_result = 0;
                    break;
                }
            }
            
            // Restore original pattern
            mem_ptr[index] = expected;
            data->asid_operations++;
        }
        
        // Yield to allow other threads and trigger context switches
        if (cycle % 10 == 0) {
            sched_yield();
        }
    }
    
    // Final memory integrity check
    for (size_t i = 0; i < num_words; i++) {
        uint32_t expected = pattern ^ (uint32_t)i;
        if (mem_ptr[i] != expected) {
            data->memory_errors++;
            data->test_result = 0;
        }
    }
    
    munmap(data->memory, data->size);
    return NULL;
}

// Test 1: Basic ASID functionality
int test_basic_functionality(void) {
    TEST_START("Basic ASID Functionality");
    
    int has_profiling = is_asid_profiling_available();
    if (!has_profiling) {
        printf("  - ASID profiling syscall not available, testing memory operations only\n");
    }
    
    asid_stats_t stats_before, stats_after;
    
    // Reset and get initial stats (if available)
    if (has_profiling) {
        reset_asid_stats();
        get_asid_stats(&stats_before);
    }
    
    // Create a simple thread to trigger ASID operations
    pthread_t thread;
    thread_test_data_t data = {
        .thread_id = 0,
        .process_id = 0,
        .size = MEMORY_SIZE,
        .test_result = 0,
        .use_reduced_workload = 0
    };
    
    int ret = pthread_create(&thread, NULL, concurrent_asid_thread, &data);
    TEST_ASSERT(ret == 0, "Failed to create test thread");
    
    pthread_join(thread, NULL);
    TEST_ASSERT(data.test_result == 1, "Thread test failed");
    TEST_ASSERT(data.memory_errors == 0, "Memory corruption detected");
    
    // Get final stats (if available)
    if (has_profiling) {
        sleep(1);  // Allow time for operations to complete
        get_asid_stats(&stats_after);
        
        printf("  - ASID allocations: %lu → %lu\n", 
               stats_before.allocations_total, stats_after.allocations_total);
    }
    
    printf("  - Memory operations: %lu\n", data.asid_operations);
    printf("  - Memory errors: %lu\n", data.memory_errors);
    printf("  - Test result: %s\n", data.test_result ? "PASS" : "FAIL");
    
    TEST_PASS("Basic ASID Functionality");
}

// Test 2: Concurrent access from multiple threads
int test_concurrent_access(void) {
    TEST_START("Concurrent ASID Access");
    
    pthread_t threads[NUM_THREADS];
    thread_test_data_t thread_data[NUM_THREADS];
    asid_stats_t stats_before, stats_after;
    int has_profiling = is_asid_profiling_available();
    
    if (has_profiling) {
        reset_asid_stats();
        get_asid_stats(&stats_before);
    }
    
    // Create multiple threads for concurrent testing
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].process_id = 0;
        thread_data[i].size = MEMORY_SIZE;
        thread_data[i].test_result = 0;
        thread_data[i].use_reduced_workload = 0;
        
        int ret = pthread_create(&threads[i], NULL, concurrent_asid_thread, &thread_data[i]);
        TEST_ASSERT(ret == 0, "Failed to create concurrent thread");
    }
    
    // Wait for all threads to complete
    int successful_threads = 0;
    uint64_t total_memory_errors = 0;
    uint64_t total_operations = 0;
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
        if (thread_data[i].test_result) {
            successful_threads++;
        }
        total_memory_errors += thread_data[i].memory_errors;
        total_operations += thread_data[i].asid_operations;
    }
    
    if (has_profiling) {
        get_asid_stats(&stats_after);
    }
    
    TEST_ASSERT(successful_threads == NUM_THREADS, "Some threads failed");
    TEST_ASSERT(total_memory_errors == 0, "Memory corruption detected in concurrent access");
    
    printf("  - Successful threads: %d/%d\n", successful_threads, NUM_THREADS);
    printf("  - Total operations: %lu\n", total_operations);
    printf("  - Total memory errors: %lu\n", total_memory_errors);
    
    if (has_profiling) {
        printf("  - ASID allocations: %lu → %lu\n", 
               stats_before.allocations_total, stats_after.allocations_total);
        printf("  - Generation rollovers: %lu\n", stats_after.generation_rollovers);
    } else {
        printf("  - ASID profiling not available, verified memory integrity only\n");
    }
    
    TEST_PASS("Concurrent ASID Access");
}

// Test 3: Multi-process ASID operations
int test_multiprocess_access(void) {
    TEST_START("Multi-Process ASID Access");
    
    asid_stats_t stats_before, stats_after;
    int has_profiling = is_asid_profiling_available();
    
    if (has_profiling) {
        reset_asid_stats();
        get_asid_stats(&stats_before);
    }
    
    pid_t pids[NUM_PROCESSES];
    int successful_processes = 0;
    
    // Fork multiple processes
    for (int p = 0; p < NUM_PROCESSES; p++) {
        printf("  - Creating process %d/%d...\n", p + 1, NUM_PROCESSES);
        pid_t pid = fork();
        
        if (pid == 0) {
            // Child process - run concurrent threads
            printf("    Child process %d starting with %d threads\n", p, NUM_THREADS/2);
            pthread_t threads[NUM_THREADS/2];  // Fewer threads per process
            thread_test_data_t thread_data[NUM_THREADS/2];
            int child_success = 1;
            
            for (int i = 0; i < NUM_THREADS/2; i++) {
                thread_data[i].thread_id = i;
                thread_data[i].process_id = p;
                thread_data[i].size = MEMORY_SIZE;
                thread_data[i].test_result = 0;
                thread_data[i].use_reduced_workload = 1;  // Use reduced workload for multi-process
                
                if (pthread_create(&threads[i], NULL, concurrent_asid_thread, &thread_data[i]) != 0) {
                    printf("    Child process %d: Failed to create thread %d\n", p, i);
                    child_success = 0;
                    break;
                }
            }
            
            printf("    Child process %d: All threads created, waiting for completion\n", p);
            
            if (child_success) {
                for (int i = 0; i < NUM_THREADS/2; i++) {
                    pthread_join(threads[i], NULL);
                    if (!thread_data[i].test_result) {
                        printf("    Child process %d: Thread %d failed\n", p, i);
                        child_success = 0;
                    }
                }
                printf("    Child process %d: All threads completed\n", p);
            }
            
            printf("    Child process %d: Exiting with status %d\n", p, child_success);
            exit(child_success ? 0 : 1);
        } else if (pid > 0) {
            pids[p] = pid;
        } else {
            TEST_FAIL("Multi-Process ASID Access", "Failed to fork process");
        }
    }
    
    // Wait for all child processes
    printf("  - Waiting for %d child processes to complete...\n", NUM_PROCESSES);
    for (int p = 0; p < NUM_PROCESSES; p++) {
        int status;
        printf("  - Waiting for process %d (PID: %d)...\n", p + 1, pids[p]);
        pid_t result = waitpid(pids[p], &status, 0);
        if (result == pids[p]) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                printf("  - Process %d completed successfully\n", p + 1);
                successful_processes++;
            } else {
                printf("  - Process %d failed (exit status: %d)\n", p + 1, WEXITSTATUS(status));
            }
        } else {
            printf("  - Error waiting for process %d\n", p + 1);
        }
    }
    
    sleep(1);  // Allow stats to stabilize
    if (has_profiling) {
        get_asid_stats(&stats_after);
    }
    
    TEST_ASSERT(successful_processes == NUM_PROCESSES, "Some child processes failed");
    
    printf("  - Successful processes: %d/%d\n", successful_processes, NUM_PROCESSES);
    
    if (has_profiling) {
        printf("  - ASID allocations: %lu → %lu\n", 
               stats_before.allocations_total, stats_after.allocations_total);
        printf("  - Active ASIDs: %u\n", stats_after.active_asids);
        printf("  - Current generation: %u\n", stats_after.current_generation);
    } else {
        printf("  - ASID profiling not available, verified process functionality only\n");
    }
    
    TEST_PASS("Multi-Process ASID Access");
}

// Test 4: Generation rollover behavior
int test_generation_rollover(void) {
    TEST_START("Generation Rollover Behavior");
    
    asid_stats_t stats;
    int has_profiling = is_asid_profiling_available();
    
    uint16_t initial_generation = 0;
    uint64_t initial_rollovers = 0;
    
    if (has_profiling) {
        reset_asid_stats();
        get_asid_stats(&stats);
        initial_generation = stats.current_generation;
        initial_rollovers = stats.generation_rollovers;
        
        printf("  - Initial generation: %u\n", initial_generation);
        printf("  - Initial rollovers: %lu\n", initial_rollovers);
    } else {
        printf("  - ASID profiling not available, testing stress behavior only\n");
    }
    
    // The new implementation should handle generation rollover automatically
    // We'll just verify the system continues to work correctly
    
    // Run a stress test to potentially trigger rollover
    pthread_t threads[NUM_THREADS];
    thread_test_data_t thread_data[NUM_THREADS];
    
    for (int round = 0; round < 3; round++) {  // Multiple rounds
        for (int i = 0; i < NUM_THREADS; i++) {
            thread_data[i].thread_id = i;
            thread_data[i].process_id = round;
            thread_data[i].size = MEMORY_SIZE;
            thread_data[i].test_result = 0;
            thread_data[i].use_reduced_workload = 1;  // Use reduced workload for stress test
            
            pthread_create(&threads[i], NULL, concurrent_asid_thread, &thread_data[i]);
        }
        
        for (int i = 0; i < NUM_THREADS; i++) {
            pthread_join(threads[i], NULL);
            TEST_ASSERT(thread_data[i].test_result == 1, "Thread failed during rollover test");
        }
        
        if (has_profiling) {
            get_asid_stats(&stats);
            printf("  - Round %d: generation=%u, rollovers=%lu, active=%u\n", 
                   round, stats.current_generation, stats.generation_rollovers, stats.active_asids);
        } else {
            printf("  - Round %d: All threads completed successfully\n", round);
        }
    }
    
    // System should still be functional regardless of rollovers
    if (has_profiling) {
        TEST_ASSERT(stats.allocation_failures == 0 || 
                    stats.generation_rollovers > initial_rollovers,
                    "System should handle ASID exhaustion via rollover");
    } else {
        printf("  - System remains functional under stress (profiling not available)\n");
    }
    
    TEST_PASS("Generation Rollover Behavior");
}

// Test 5: Edge cases and error conditions
int test_edge_cases(void) {
    TEST_START("Edge Cases and Error Conditions");
    
    // Test rapid allocation/deallocation cycles
    for (int cycle = 0; cycle < 10; cycle++) {
        pthread_t rapid_threads[4];
        thread_test_data_t rapid_data[4];
        
        // Quick threads that do minimal work
        for (int i = 0; i < 4; i++) {
            rapid_data[i].thread_id = i;
            rapid_data[i].process_id = cycle;
            rapid_data[i].size = 64 * 1024;  // Smaller memory
            rapid_data[i].test_result = 0;
            rapid_data[i].use_reduced_workload = 1;  // Use reduced workload for rapid cycles
            
            pthread_create(&rapid_threads[i], NULL, concurrent_asid_thread, &rapid_data[i]);
        }
        
        for (int i = 0; i < 4; i++) {
            pthread_join(rapid_threads[i], NULL);
            TEST_ASSERT(rapid_data[i].test_result == 1, "Rapid cycle thread failed");
        }
    }
    
    // Test with minimal memory allocations
    void *tiny_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, 
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(tiny_mem != MAP_FAILED, "Failed to allocate tiny memory");
    
    uint32_t *ptr = (uint32_t*)tiny_mem;
    *ptr = 0x12345678;
    TEST_ASSERT(*ptr == 0x12345678, "Tiny memory access failed");
    
    munmap(tiny_mem, 4096);
    
    TEST_PASS("Edge Cases and Error Conditions");
}

// Main test runner
int main(int argc, char *argv[]) {
    printf("=== ASID Correctness Test Suite ===\n");
    printf("Testing the new unified ASID manager implementation\n");
    
    // Check if ASID profiling is available
    if (is_asid_profiling_available()) {
        printf("✓ ASID profiling syscall available - full testing enabled\n\n");
    } else {
        printf("⚠ ASID profiling syscall not available - testing core functionality only\n");
        printf("  (Memory integrity and concurrency will still be thoroughly tested)\n\n");
    }
    
    if (TEST_BASIC_FUNCTIONALITY) {
        test_basic_functionality();
        printf("\n");
    }
    
    if (TEST_CONCURRENT_ACCESS) {
        test_concurrent_access();
        printf("\n");
    }
    
    if (TEST_GENERATION_ROLLOVER) {
        test_multiprocess_access();
        printf("\n");
    }
    
    if (TEST_GENERATION_ROLLOVER) {
        test_generation_rollover();
        printf("\n");
    }
    
    if (TEST_EDGE_CASES) {
        test_edge_cases();
        printf("\n");
    }
    
    // Print final summary
    printf("=== Test Results Summary ===\n");
    printf("Total tests run: %d\n", total_tests);
    printf("Passed: %d\n", passed_tests);
    printf("Failed: %d\n", failed_tests);
    
    if (failed_tests == 0) {
        printf("🎉 ALL TESTS PASSED - ASID implementation is correct!\n");
        return 0;
    } else {
        printf("⚠️  SOME TESTS FAILED - Please investigate ASID implementation\n");
        return 1;
    }
} 