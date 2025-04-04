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

#define NUM_PROCESSES 100
#define ITERATIONS_PER_PROCESS 100
#define MEMORY_SIZE (4096 * 1024)  // 4 MB memory per process
#define PAGE_SIZE 4096

// Structure to gather performance metrics
typedef struct {
    unsigned long long page_faults;
    unsigned long long context_switches;
    double elapsed_time;
} perf_stats_t;

// Get the page fault count
unsigned long long get_page_faults() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        perror("getrusage");
        return 0;
    }
    return usage.ru_majflt + usage.ru_minflt;
}

// Check if CPU supports PCID
int is_pcid_supported() {
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) {
        perror("Failed to open /proc/cpuinfo");
        return 0;
    }

    char line[1024]; // increase buffer size to avoid truncation
    int pcid_supported = 0;

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
        // so we can see if the kernel has enabled the PCID flag
        // check startup information or messages in dmesg about PCID
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

// Function to measure TLB flushing cost using memory access pattern
void stress_tlb(char *memory, size_t size) {
    // Access memory with a stride of page size to force TLB entries
    for (size_t i = 0; i < size; i += PAGE_SIZE) {
        memory[i] += 1;
    }
}

// Test multiple processes accessing and modifying memory, forcing context switches
void run_test(perf_stats_t *stats) {
    pid_t pids[NUM_PROCESSES];
    struct timeval start, end;
    unsigned long long initial_faults = get_page_faults();
    
    gettimeofday(&start, NULL);
    
    for (int i = 0; i < NUM_PROCESSES; i++) {
        pid_t pid = fork();
        
        if (pid < 0) {
            perror("fork");
            exit(1);
        } else if (pid == 0) {
            // Child process
            // Allocate a large block of memory
            char *memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE, 
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                
            if (memory == MAP_FAILED) {
                perror("mmap");
                exit(1);
            }
            
            // Initialize memory with a pattern
            memset(memory, i, MEMORY_SIZE);
            
            // Force TLB entries to be created
            for (int j = 0; j < ITERATIONS_PER_PROCESS; j++) {
                stress_tlb(memory, MEMORY_SIZE);
                // Yield to increase context switches
                sched_yield();
            }
            
            munmap(memory, MEMORY_SIZE);
            exit(0);
        } else {
            // Parent process
            pids[i] = pid;
        }
    }
    
    // Wait for all children to complete
    for (int i = 0; i < NUM_PROCESSES; i++) {
        waitpid(pids[i], NULL, 0);
    }
    
    gettimeofday(&end, NULL);
    
    // Calculate metrics
    stats->elapsed_time = (end.tv_sec - start.tv_sec) + 
                         (end.tv_usec - start.tv_usec) / 1000000.0;
    stats->page_faults = get_page_faults() - initial_faults;
    
    // We can't directly measure TLB misses, but page faults and 
    // execution time are indicators
}

int main() {
    perf_stats_t stats;
    
    // Check if PCID is supported
    int pcid_supported = is_pcid_supported();
    printf("PCID support: %s\n", pcid_supported ? "YES" : "NO");
    
    printf("Starting ASID/PCID test: %d processes with %d iterations each\n", 
           NUM_PROCESSES, ITERATIONS_PER_PROCESS);
    
    printf("Each process will access %d KB of memory\n", MEMORY_SIZE / 1024);
    
    // Run the test
    run_test(&stats);
    
    // Print results
    printf("\nResults:\n");
    printf("Total time: %.4f seconds\n", stats.elapsed_time);
    printf("Page faults: %llu\n", stats.page_faults);
    printf("Time per process: %.4f seconds\n", 
           stats.elapsed_time / NUM_PROCESSES);
    
    printf("\nIf PCID is working correctly, performance should be better than\n");
    printf("without PCID when multiple processes are switching context.\n");
    
    return 0;
}