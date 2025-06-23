// SPDX-License-Identifier: MPL-2.0

// This program provides a command-line interface for accessing ASID profiling statistics
// from the kernel via the sys_asid_profiling syscall.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <errno.h>
#include <time.h>

// Syscall number for ASID profiling (this would need to be defined in the kernel headers)
#define SYS_ASID_PROFILING 999  // Placeholder syscall number

// Action codes for the syscall
#define ASID_ACTION_GET_STATS 0
#define ASID_ACTION_PRINT_LOG 1
#define ASID_ACTION_RESET 2
#define ASID_ACTION_GET_EFFICIENCY 3

// ASID statistics structure (matching kernel definition)
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

// ASID efficiency metrics structure (matching kernel definition)
typedef struct {
    uint64_t allocation_success_rate;  // Parts per million (0-1000000)
    uint64_t reuse_efficiency;         // Parts per million
    uint64_t flush_efficiency;         // Parts per million (higher is better)
    uint64_t avg_cycles_per_allocation;
    uint64_t avg_cycles_per_context_switch;
} asid_efficiency_t;

// Function to call the ASID profiling syscall
long asid_profiling_syscall(uint32_t action, void *buffer, size_t buffer_len) {
    return syscall(SYS_ASID_PROFILING, action, buffer, buffer_len);
}

// Function to format large numbers with commas
void format_number(uint64_t num, char *buffer, size_t buffer_size) {
    char temp[64];
    snprintf(temp, sizeof(temp), "%lu", num);
    
    int len = strlen(temp);
    int comma_count = (len - 1) / 3;
    int total_len = len + comma_count;
    
    if (total_len >= buffer_size) {
        snprintf(buffer, buffer_size, "%lu", num);
        return;
    }
    
    int src_pos = len - 1;
    int dst_pos = total_len - 1;
    buffer[total_len] = '\0';
    
    for (int i = 0; i < len; i++) {
        buffer[dst_pos--] = temp[src_pos--];
        if ((i + 1) % 3 == 0 && src_pos >= 0) {
            buffer[dst_pos--] = ',';
        }
    }
}

// Function to display ASID statistics
void display_asid_stats(const asid_stats_t *stats) {
    char num_buffer[64];
    
    printf("=== ASID Profiling Statistics ===\n");
    printf("\n");
    
    // System Information
    printf("--- System Information ---\n");
    printf("PCID Support:        %s\n", stats->pcid_enabled ? "Enabled" : "Disabled");
    printf("Current Generation:  %u\n", stats->current_generation);
    printf("Active ASIDs:        %u\n", stats->active_asids);
    printf("Total ASIDs Used:    %u\n", stats->total_asids_used);
    printf("\n");
    
    // Allocation Statistics
    printf("--- Allocation Statistics ---\n");
    format_number(stats->allocations_total, num_buffer, sizeof(num_buffer));
    printf("Total Allocations:   %s\n", num_buffer);
    
    format_number(stats->deallocations_total, num_buffer, sizeof(num_buffer));
    printf("Total Deallocations: %s\n", num_buffer);
    
    format_number(stats->allocation_failures, num_buffer, sizeof(num_buffer));
    printf("Allocation Failures: %s\n", num_buffer);
    
    format_number(stats->generation_rollovers, num_buffer, sizeof(num_buffer));
    printf("Generation Rollovers: %s\n", num_buffer);
    
    format_number(stats->asid_reuse_count, num_buffer, sizeof(num_buffer));
    printf("ASID Reuses:         %s\n", num_buffer);
    
    if (stats->allocations_total > 0) {
        double failure_rate = (double)stats->allocation_failures / (stats->allocations_total + stats->allocation_failures) * 100.0;
        printf("Failure Rate:        %.2f%%\n", failure_rate);
        
        double avg_alloc_time = (double)stats->allocation_time_total / stats->allocations_total;
        printf("Avg Alloc Time:      %.1f cycles\n", avg_alloc_time);
    }
    printf("\n");
    
    // Search Operations
    printf("--- Search Operations ---\n");
    format_number(stats->bitmap_searches, num_buffer, sizeof(num_buffer));
    printf("Bitmap Searches:     %s\n", num_buffer);
    
    format_number(stats->map_searches, num_buffer, sizeof(num_buffer));
    printf("Map Searches:        %s\n", num_buffer);
    printf("\n");
    
    // TLB Operations
    printf("--- TLB Operations ---\n");
    format_number(stats->tlb_single_address_flushes, num_buffer, sizeof(num_buffer));
    printf("Single Address:      %s\n", num_buffer);
    
    format_number(stats->tlb_single_context_flushes, num_buffer, sizeof(num_buffer));
    printf("Single Context:      %s\n", num_buffer);
    
    format_number(stats->tlb_all_context_flushes, num_buffer, sizeof(num_buffer));
    printf("All Contexts:        %s\n", num_buffer);
    
    format_number(stats->tlb_full_flushes, num_buffer, sizeof(num_buffer));
    printf("Full Flushes:        %s\n", num_buffer);
    
    uint64_t total_tlb_ops = stats->tlb_single_address_flushes + 
                            stats->tlb_single_context_flushes +
                            stats->tlb_all_context_flushes + 
                            stats->tlb_full_flushes;
    
    format_number(total_tlb_ops, num_buffer, sizeof(num_buffer));
    printf("Total TLB Ops:       %s\n", num_buffer);
    
    if (total_tlb_ops > 0) {
        double avg_tlb_time = (double)stats->tlb_flush_time_total / total_tlb_ops;
        printf("Avg TLB Flush Time:  %.1f cycles\n", avg_tlb_time);
    }
    printf("\n");
    
    // Context Switch Statistics
    printf("--- Context Switch Statistics ---\n");
    format_number(stats->context_switches, num_buffer, sizeof(num_buffer));
    printf("Total Switches:      %s\n", num_buffer);
    
    format_number(stats->context_switches_with_flush, num_buffer, sizeof(num_buffer));
    printf("Switches with Flush: %s\n", num_buffer);
    
    format_number(stats->vmspace_activations, num_buffer, sizeof(num_buffer));
    printf("VM Space Activations: %s\n", num_buffer);
    
    if (stats->context_switches > 0) {
        double flush_percentage = (double)stats->context_switches_with_flush / stats->context_switches * 100.0;
        printf("Flush Percentage:    %.2f%%\n", flush_percentage);
        
        double avg_switch_time = (double)stats->context_switch_time_total / stats->context_switches;
        printf("Avg Switch Time:     %.1f cycles\n", avg_switch_time);
    }
    printf("\n");
}

// Function to display efficiency metrics
void display_efficiency_metrics(const asid_efficiency_t *efficiency) {
    printf("=== ASID Efficiency Metrics ===\n");
    printf("\n");
    
    printf("Allocation Success Rate: %.4f%% (%lu/1000000)\n", 
           efficiency->allocation_success_rate / 10000.0,
           efficiency->allocation_success_rate);
    
    printf("ASID Reuse Efficiency:   %.4f%% (%lu/1000000)\n", 
           efficiency->reuse_efficiency / 10000.0,
           efficiency->reuse_efficiency);
    
    printf("TLB Flush Efficiency:    %.4f%% (%lu/1000000)\n", 
           efficiency->flush_efficiency / 10000.0,
           efficiency->flush_efficiency);
    
    printf("Avg Cycles/Allocation:   %lu\n", efficiency->avg_cycles_per_allocation);
    printf("Avg Cycles/Context Switch: %lu\n", efficiency->avg_cycles_per_context_switch);
    printf("\n");
}

// Function to print usage information
void print_usage(const char *program_name) {
    printf("Usage: %s [OPTION]\n", program_name);
    printf("Display ASID profiling statistics and metrics.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -s, --stats      Display detailed statistics (default)\n");
    printf("  -e, --efficiency Display efficiency metrics\n");
    printf("  -l, --log        Print detailed report to kernel log\n");
    printf("  -r, --reset      Reset all statistics\n");
    printf("  -a, --all        Display both statistics and efficiency metrics\n");
    printf("  -h, --help       Display this help message\n");
    printf("\n");
    printf("Note: This utility requires the sys_asid_profiling syscall to be available.\n");
}

int main(int argc, char *argv[]) {
    const char *program_name = argv[0];
    int show_stats = 0;
    int show_efficiency = 0;
    int print_log = 0;
    int reset_stats = 0;
    
    // Default to showing stats if no arguments
    if (argc == 1) {
        show_stats = 1;
    }
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--stats") == 0) {
            show_stats = 1;
        } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--efficiency") == 0) {
            show_efficiency = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            print_log = 1;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reset") == 0) {
            reset_stats = 1;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            show_stats = 1;
            show_efficiency = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(program_name);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(program_name);
            return EXIT_FAILURE;
        }
    }
    
    // Print kernel log if requested
    if (print_log) {
        printf("Printing detailed ASID report to kernel log...\n");
        long result = asid_profiling_syscall(ASID_ACTION_PRINT_LOG, NULL, 0);
        if (result < 0) {
            fprintf(stderr, "Failed to print log: %s\n", strerror(-result));
            return EXIT_FAILURE;
        }
        printf("Report printed to kernel log successfully.\n");
        if (!show_stats && !show_efficiency && !reset_stats) {
            return EXIT_SUCCESS;
        }
        printf("\n");
    }
    
    // Reset statistics if requested
    if (reset_stats) {
        printf("Resetting ASID profiling statistics...\n");
        long result = asid_profiling_syscall(ASID_ACTION_RESET, NULL, 0);
        if (result < 0) {
            fprintf(stderr, "Failed to reset statistics: %s\n", strerror(-result));
            return EXIT_FAILURE;
        }
        printf("Statistics reset successfully.\n");
        if (!show_stats && !show_efficiency) {
            return EXIT_SUCCESS;
        }
        printf("\n");
    }
    
    // Get and display statistics
    if (show_stats) {
        asid_stats_t stats;
        long result = asid_profiling_syscall(ASID_ACTION_GET_STATS, &stats, sizeof(stats));
        if (result < 0) {
            fprintf(stderr, "Failed to get ASID statistics: %s\n", strerror(-result));
            return EXIT_FAILURE;
        }
        
        display_asid_stats(&stats);
    }
    
    // Get and display efficiency metrics
    if (show_efficiency) {
        asid_efficiency_t efficiency;
        long result = asid_profiling_syscall(ASID_ACTION_GET_EFFICIENCY, &efficiency, sizeof(efficiency));
        if (result < 0) {
            fprintf(stderr, "Failed to get efficiency metrics: %s\n", strerror(-result));
            return EXIT_FAILURE;
        }
        
        display_efficiency_metrics(&efficiency);
    }
    
    return EXIT_SUCCESS;
} 