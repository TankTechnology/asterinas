// SPDX-License-Identifier: MPL-2.0

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <time.h>

#define NUM_PROCESSES 1000
#define MEMORY_SIZE (1 * 256 * 1024)  // Memory size per process
#define PAGE_SIZE 4096
#define TEST_ITERATIONS 5

// Structure to gather performance metrics
typedef struct {
    unsigned long long page_faults;
    struct timespec start_time;
    struct timespec end_time;
    double elapsed_time;
} perf_stats_t;

// Get the page fault count for the current process
unsigned long long get_page_faults() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        perror("getrusage");
        return 0;
    }
    return usage.ru_majflt + usage.ru_minflt;
}

// Check if CPU and kernel support PCID
int is_pcid_supported() {
    // check if PCID is supported by the CPU
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) {
        perror("Failed to open /proc/cpuinfo");
        return 0;
    }

    char line[1024];  // 增大缓冲区以防截断
    int pcid_supported = 0;

    // Check CPU flags for PCID support
    while (fgets(line, sizeof(line), cpuinfo)) {
        if (strstr(line, "flags") && strstr(line, "pcid")) {
            pcid_supported = 1;
            break;
        }
    }
    fclose(cpuinfo);

    // if not detected from cpuinfo, try alternative method
    if (!pcid_supported) {
        // on Asterinas, /proc/cpuinfo may not contain complete flags
        // check messages in dmesg about PCID
        FILE *dmesg = popen("dmesg | grep -i pcid", "r");
        if (dmesg) {
            while (fgets(line, sizeof(line), dmesg)) {
                if (strstr(line, "PCID supported: true") || 
                    strstr(line, "PCID supported: 1")) {
                    pcid_supported = 1;
                    break;
                }
            }
            pclose(dmesg);
        }
    }

    if (!pcid_supported) {
        // check kernel command line parameters
        FILE *cmdline = fopen("/proc/cmdline", "r");
        if (cmdline) {
            if (fgets(line, sizeof(line), cmdline)) {
                if (strstr(line, "nopti") || strstr(line, "pti=off")) {
                    // PTI (Page Table Isolation) is usually used with PCID
                    // if PTI is disabled, PCID usage may be limited
                    printf("Note: PTI disabled in kernel cmdline\n");
                }
            }
            fclose(cmdline);
        }
    }

    // increase test code to read CPU information
    printf("CPU Flags found in /proc/cpuinfo:\n");
    cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        while (fgets(line, sizeof(line), cpuinfo)) {
            if (strstr(line, "flags")) {
                printf("%s", line);
            }
        }
        fclose(cpuinfo);
    }

    return pcid_supported;
}

// Check if running in Asterinas OS
int is_asterinas() {
    FILE *version = fopen("/proc/version", "r");
    if (!version) {
        return 0; // Can't determine
    }
    
    char line[1024];
    int is_aster = 0;
    
    if (fgets(line, sizeof(line), version)) {
        if (strstr(line, "Asterinas")) {
            is_aster = 1;
        }
    }
    fclose(version);
    
    return is_aster;
}

// Function to access memory with a pattern that stresses TLB
void access_memory_pattern(char *memory, size_t size) {
    // First touch all pages to ensure they're mapped
    for (size_t i = 0; i < size; i += PAGE_SIZE) {
        memory[i] = 1;
    }
    
    // Now access in a pattern that will use many TLB entries
    for (int pass = 0; pass < 10; pass++) {
        // Forward access
        for (size_t i = 0; i < size; i += PAGE_SIZE) {
            memory[i] += pass;
        }
        
        // Reverse access
        for (size_t i = size - PAGE_SIZE; i > 0; i -= PAGE_SIZE) {
            memory[i] += pass;
        }
        
        // Random-like access with different stride patterns to maximize TLB pressure
        for (size_t stride = 13; stride < 100; stride += 11) {
            for (size_t i = 0; i < size; i += (PAGE_SIZE * stride) % size) {
                memory[i % size] += pass;
            }
        }
    }
}

// Worker process function
void worker_process(int id) {
    // Allocate memory
    char *memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (memory == MAP_FAILED) {
        perror("mmap in worker");
        exit(1);
    }
    
    // Initialize with a pattern
    memset(memory, id & 0xFF, MEMORY_SIZE);
    
    // Access memory to stress TLB
    for (int i = 0; i < 20; i++) {
        access_memory_pattern(memory, MEMORY_SIZE);
        
        // Yield to force context switches
        sched_yield();
    }
    
    munmap(memory, MEMORY_SIZE);
    exit(0);
}

// Run test with multiple processes to stress ASID/TLB handling
void run_test(perf_stats_t *stats) {
    pid_t pids[NUM_PROCESSES];
    unsigned long long initial_faults = get_page_faults();
    
    // Record start time
    clock_gettime(CLOCK_MONOTONIC, &stats->start_time);
    
    // Create worker processes
    for (int i = 0; i < NUM_PROCESSES; i++) {
        pid_t pid = fork();
        
        if (pid < 0) {
            perror("fork");
            exit(1);
        } else if (pid == 0) {
            // Child process
            worker_process(i);
            // Should not reach here
            exit(1);
        } else {
            // Parent process
            pids[i] = pid;
        }
    }
    
    // Wait for all workers to complete
    for (int i = 0; i < NUM_PROCESSES; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Worker %d failed with status %d\n", i, status);
        }
    }
    
    // Record end time
    clock_gettime(CLOCK_MONOTONIC, &stats->end_time);
    
    // Calculate elapsed time
    stats->elapsed_time = 
        (stats->end_time.tv_sec - stats->start_time.tv_sec) +
        (stats->end_time.tv_nsec - stats->start_time.tv_nsec) / 1000000000.0;
    
    // Record page faults
    stats->page_faults = get_page_faults() - initial_faults;
}

int main() {
    perf_stats_t stats[TEST_ITERATIONS];
    perf_stats_t avg_stats = {0};
    
    // Check system info
    int pcid_supported = is_pcid_supported();
    int is_aster = is_asterinas();
    
    printf("======== ASID/PCID PERFORMANCE TEST ========\n");
    printf("System info:\n");
    printf("  CPU PCID support: %s\n", pcid_supported ? "YES" : "NO");
    printf("  Running on Asterinas: %s\n", is_aster ? "YES" : "NO");
    printf("  Test configuration: %d processes, %d KB memory per process\n", 
           NUM_PROCESSES,  MEMORY_SIZE / (1024));
    printf("==========================================\n\n");
    
    printf("Running %d test iterations...\n", TEST_ITERATIONS);
    
    // Run multiple test iterations
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        printf("Iteration %d/%d: ", i+1, TEST_ITERATIONS);
        fflush(stdout);
        
        run_test(&stats[i]);
        
        printf("%.4f seconds, %llu page faults\n", 
               stats[i].elapsed_time, stats[i].page_faults);
        
        // Accumulate for average
        avg_stats.elapsed_time += stats[i].elapsed_time;
        avg_stats.page_faults += stats[i].page_faults;
        
    }
    
    // Calculate averages
    avg_stats.elapsed_time /= TEST_ITERATIONS;
    avg_stats.page_faults /= TEST_ITERATIONS;
    
    // Print final results
    printf("\n======== TEST RESULTS ========\n");
    printf("Average execution time: %.4f seconds\n", avg_stats.elapsed_time);
    printf("Average page faults: %llu\n", avg_stats.page_faults);
    printf("Time per process: %.4f seconds\n", 
           avg_stats.elapsed_time / NUM_PROCESSES);
    
    printf("\nInterpretation:\n");
    if (pcid_supported) {
        printf("PCID is supported by your CPU and appears to be enabled.\n");
        printf("The observed performance reflects PCID-optimized context switches.\n");
        printf("This should result in better performance compared to systems without PCID.\n");
    } else {
        printf("PCID is not supported or not enabled on your system.\n");
        printf("Context switches require full TLB flushes, which can reduce performance.\n");
    }
    
    printf("\nIn Asterinas OS, PCID/ASID support improves performance by:\n");
    printf("1. Avoiding unnecessary TLB flushes during context switches\n");
    printf("2. Using unique identifiers (ASIDs) for each address space\n");
    printf("3. Allowing TLB entries from different processes to coexist\n");
    
    return 0;
}