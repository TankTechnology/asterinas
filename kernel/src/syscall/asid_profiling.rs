// SPDX-License-Identifier: MPL-2.0

//! System calls for ASID profiling and monitoring.

use ostd::mm::asid_profiling::{print_asid_stats, reset_asid_stats, ASID_STATS};

use crate::{context::Context, current_userspace, prelude::*, syscall::SyscallReturn};

/// Get ASID profiling statistics.
/// 
/// This syscall returns detailed ASID profiling information including:
/// - Allocation/deallocation statistics
/// - TLB operation counts
/// - Context switch metrics
/// - Performance timing data
/// 
/// # Arguments
/// 
/// * `action` - The action to perform:
///   - 0: Get basic statistics
///   - 1: Print detailed report to kernel log
///   - 2: Reset all statistics
///   - 3: Get efficiency metrics
/// * `buffer` - User buffer to store results (for action 0 and 3)
/// * `buffer_len` - Length of the user buffer
/// 
/// # Returns
/// 
/// On success, returns the number of bytes written to buffer (for actions 0 and 3),
/// or 0 for other actions. On error, returns a negative error code.
pub fn sys_asid_profiling(action: u32, buffer: Vaddr, buffer_len: usize, _ctx: &Context) -> Result<SyscallReturn> {
    match action {
        0 => {
            // Get basic statistics
            if buffer_len < core::mem::size_of::<AsidStatsUserspace>() {
                return Err(Error::with_message(Errno::EINVAL, "Buffer too small"));
            }
            
            let report = ASID_STATS.get_report();
            let user_stats = AsidStatsUserspace {
                allocations_total: report.allocations_total,
                deallocations_total: report.deallocations_total,
                allocation_failures: report.allocation_failures,
                generation_rollovers: report.generation_rollovers,
                
                bitmap_searches: report.bitmap_searches,
                map_searches: report.map_searches,
                asid_reuse_count: report.asid_reuse_count,
                
                tlb_single_address_flushes: report.tlb_single_address_flushes,
                tlb_single_context_flushes: report.tlb_single_context_flushes,
                tlb_all_context_flushes: report.tlb_all_context_flushes,
                tlb_full_flushes: report.tlb_full_flushes,
                
                context_switches: report.context_switches,
                context_switches_with_flush: report.context_switches_with_flush,
                vmspace_activations: report.vmspace_activations,
                
                allocation_time_total: report.allocation_time_total,
                deallocation_time_total: report.deallocation_time_total,
                tlb_flush_time_total: report.tlb_flush_time_total,
                context_switch_time_total: report.context_switch_time_total,
                
                active_asids: report.active_asids,
                current_generation: report.current_generation,
                pcid_enabled: if report.pcid_enabled { 1 } else { 0 },
                total_asids_used: report.total_asids_used,
            };
            
            // Copy to user space - write fields individually
            let mut offset = 0;
            
            // Helper macro to write a field and advance offset
            macro_rules! write_field {
                ($field:expr) => {
                    let field_bytes = $field.to_ne_bytes();
                    let mut reader = ostd::mm::VmReader::from(&field_bytes[..]);
                    current_userspace!().write_bytes(buffer + offset, &mut reader)?;
                    offset += field_bytes.len();
                };
            }
            
            write_field!(user_stats.allocations_total);
            write_field!(user_stats.deallocations_total);
            write_field!(user_stats.allocation_failures);
            write_field!(user_stats.generation_rollovers);
            write_field!(user_stats.bitmap_searches);
            write_field!(user_stats.map_searches);
            write_field!(user_stats.asid_reuse_count);
            write_field!(user_stats.tlb_single_address_flushes);
            write_field!(user_stats.tlb_single_context_flushes);
            write_field!(user_stats.tlb_all_context_flushes);
            write_field!(user_stats.tlb_full_flushes);
            write_field!(user_stats.context_switches);
            write_field!(user_stats.context_switches_with_flush);
            write_field!(user_stats.vmspace_activations);
            write_field!(user_stats.allocation_time_total);
            write_field!(user_stats.deallocation_time_total);
            write_field!(user_stats.tlb_flush_time_total);
            write_field!(user_stats.context_switch_time_total);
            write_field!(user_stats.active_asids);
            write_field!(user_stats.current_generation);
            write_field!(user_stats.pcid_enabled);
            write_field!(user_stats.total_asids_used);
            
            Ok(SyscallReturn::Return(core::mem::size_of::<AsidStatsUserspace>() as isize))
        }
        
        1 => {
            // Print detailed report to kernel log
            print_asid_stats();
            Ok(SyscallReturn::Return(0))
        }
        
        2 => {
            // Reset all statistics
            reset_asid_stats();
            Ok(SyscallReturn::Return(0))
        }
        
        3 => {
            // Get efficiency metrics
            if buffer_len < core::mem::size_of::<AsidEfficiencyUserspace>() {
                return Err(Error::with_message(Errno::EINVAL, "Buffer too small"));
            }
            
            let report = ASID_STATS.get_report();
            let efficiency = report.calculate_efficiency();
            
            let user_efficiency = AsidEfficiencyUserspace {
                allocation_success_rate: (efficiency.allocation_success_rate * 1000000.0) as u64, // Store as parts per million
                reuse_efficiency: (efficiency.reuse_efficiency * 1000000.0) as u64,
                flush_efficiency: (efficiency.flush_efficiency * 1000000.0) as u64,
                avg_cycles_per_allocation: efficiency.avg_cycles_per_allocation as u64,
                avg_cycles_per_context_switch: efficiency.avg_cycles_per_context_switch as u64,
            };
            
            // Copy to user space - write fields individually
            let mut offset = 0;
            
            // Helper macro to write a field and advance offset
            macro_rules! write_field {
                ($field:expr) => {
                    let field_bytes = $field.to_ne_bytes();
                    let mut reader = ostd::mm::VmReader::from(&field_bytes[..]);
                    current_userspace!().write_bytes(buffer + offset, &mut reader)?;
                    offset += field_bytes.len();
                };
            }
            
            write_field!(user_efficiency.allocation_success_rate);
            write_field!(user_efficiency.reuse_efficiency);
            write_field!(user_efficiency.flush_efficiency);
            write_field!(user_efficiency.avg_cycles_per_allocation);
            write_field!(user_efficiency.avg_cycles_per_context_switch);
            
            Ok(SyscallReturn::Return(core::mem::size_of::<AsidEfficiencyUserspace>() as isize))
        }
        
        _ => Err(Error::with_message(Errno::EINVAL, "Invalid action")),
    }
}

/// ASID statistics structure for userspace
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AsidStatsUserspace {
    // Basic counters
    pub allocations_total: u64,
    pub deallocations_total: u64,
    pub allocation_failures: u64,
    pub generation_rollovers: u64,
    
    // Search operations
    pub bitmap_searches: u64,
    pub map_searches: u64,
    pub asid_reuse_count: u64,
    
    // TLB operations
    pub tlb_single_address_flushes: u64,
    pub tlb_single_context_flushes: u64,
    pub tlb_all_context_flushes: u64,
    pub tlb_full_flushes: u64,
    
    // Context switches
    pub context_switches: u64,
    pub context_switches_with_flush: u64,
    pub vmspace_activations: u64,
    
    // Performance timing
    pub allocation_time_total: u64,
    pub deallocation_time_total: u64,
    pub tlb_flush_time_total: u64,
    pub context_switch_time_total: u64,
    
    // Current state
    pub active_asids: u32,
    pub current_generation: u16,
    pub pcid_enabled: u32, // 0 = disabled, 1 = enabled
    pub total_asids_used: u32,
}

/// ASID efficiency metrics structure for userspace
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AsidEfficiencyUserspace {
    pub allocation_success_rate: u64,  // Parts per million (0-1000000)
    pub reuse_efficiency: u64,         // Parts per million
    pub flush_efficiency: u64,         // Parts per million (higher is better)
    pub avg_cycles_per_allocation: u64,
    pub avg_cycles_per_context_switch: u64,
} 