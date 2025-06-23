// SPDX-License-Identifier: MPL-2.0

//! ASID (Address Space ID) profiling and performance monitoring.
//!
//! This module provides comprehensive profiling capabilities for ASID operations,
//! including allocation/deallocation statistics, TLB flush tracking, and performance metrics.

use core::sync::atomic::{AtomicU64, AtomicU32, AtomicU16, Ordering};
use alloc::collections::BTreeMap;
use log::{info, debug};

use crate::sync::SpinLock;

/// Global ASID profiling statistics
pub static ASID_STATS: AsidStats = AsidStats::new();

/// Comprehensive ASID profiling statistics
pub struct AsidStats {
    // Allocation/Deallocation tracking
    pub allocations_total: AtomicU64,
    pub deallocations_total: AtomicU64,
    pub allocation_failures: AtomicU64,
    pub generation_rollovers: AtomicU64,
    
    // ASID reuse tracking
    pub asid_reuse_count: AtomicU64,
    pub bitmap_searches: AtomicU64,
    pub map_searches: AtomicU64,
    
    // TLB operation tracking
    pub tlb_single_address_flushes: AtomicU64,
    pub tlb_single_context_flushes: AtomicU64,
    pub tlb_all_context_flushes: AtomicU64,
    pub tlb_full_flushes: AtomicU64,
    
    // Context switch tracking
    pub context_switches: AtomicU64,
    pub context_switches_with_flush: AtomicU64,
    pub vmspace_activations: AtomicU64,
    
    // Performance timing (in CPU cycles)
    pub allocation_time_total: AtomicU64,
    pub deallocation_time_total: AtomicU64,
    pub tlb_flush_time_total: AtomicU64,
    pub context_switch_time_total: AtomicU64,
    
    // Current state
    pub active_asids: AtomicU32,
    pub current_generation: AtomicU16,
    pub pcid_enabled: AtomicU32, // 0 = disabled, 1 = enabled
    
    // Per-ASID usage statistics (protected by spinlock)
    per_asid_stats: SpinLock<BTreeMap<u16, AsidUsageStats>>,
}

/// Per-ASID usage statistics
#[derive(Debug, Clone, Default)]
pub struct AsidUsageStats {
    pub allocation_count: u64,
    pub activation_count: u64,
    pub last_used_timestamp: u64,
    pub total_active_time: u64,
    pub tlb_flushes: u64,
}

impl AsidStats {
    /// Create a new ASID statistics structure
    pub const fn new() -> Self {
        Self {
            allocations_total: AtomicU64::new(0),
            deallocations_total: AtomicU64::new(0),
            allocation_failures: AtomicU64::new(0),
            generation_rollovers: AtomicU64::new(0),
            
            asid_reuse_count: AtomicU64::new(0),
            bitmap_searches: AtomicU64::new(0),
            map_searches: AtomicU64::new(0),
            
            tlb_single_address_flushes: AtomicU64::new(0),
            tlb_single_context_flushes: AtomicU64::new(0),
            tlb_all_context_flushes: AtomicU64::new(0),
            tlb_full_flushes: AtomicU64::new(0),
            
            context_switches: AtomicU64::new(0),
            context_switches_with_flush: AtomicU64::new(0),
            vmspace_activations: AtomicU64::new(0),
            
            allocation_time_total: AtomicU64::new(0),
            deallocation_time_total: AtomicU64::new(0),
            tlb_flush_time_total: AtomicU64::new(0),
            context_switch_time_total: AtomicU64::new(0),
            
            active_asids: AtomicU32::new(0),
            current_generation: AtomicU16::new(0),
            pcid_enabled: AtomicU32::new(0),
            
            per_asid_stats: SpinLock::new(BTreeMap::new()),
        }
    }
    
    /// Record an ASID allocation
    pub fn record_allocation(&self, asid: u16, time_cycles: u64) {
        self.allocations_total.fetch_add(1, Ordering::Relaxed);
        self.allocation_time_total.fetch_add(time_cycles, Ordering::Relaxed);
        self.active_asids.fetch_add(1, Ordering::Relaxed);
        
        // Update per-ASID stats
        let mut per_asid = self.per_asid_stats.lock();
        let stats = per_asid.entry(asid).or_default();
        stats.allocation_count += 1;
        stats.last_used_timestamp = self.get_timestamp();
        
        debug!("[ASID_PROF] Allocated ASID {} in {} cycles", asid, time_cycles);
    }
    
    /// Record an ASID deallocation
    pub fn record_deallocation(&self, asid: u16, time_cycles: u64) {
        self.deallocations_total.fetch_add(1, Ordering::Relaxed);
        self.deallocation_time_total.fetch_add(time_cycles, Ordering::Relaxed);
        self.active_asids.fetch_sub(1, Ordering::Relaxed);
        
        debug!("[ASID_PROF] Deallocated ASID {} in {} cycles", asid, time_cycles);
    }
    
    /// Record an allocation failure
    pub fn record_allocation_failure(&self) {
        self.allocation_failures.fetch_add(1, Ordering::Relaxed);
        debug!("[ASID_PROF] ASID allocation failed");
    }
    
    /// Record a generation rollover
    pub fn record_generation_rollover(&self, new_generation: u16) {
        self.generation_rollovers.fetch_add(1, Ordering::Relaxed);
        self.current_generation.store(new_generation, Ordering::Relaxed);
        info!("[ASID_PROF] Generation rollover to {}", new_generation);
    }
    
    /// Record bitmap search operation
    pub fn record_bitmap_search(&self) {
        self.bitmap_searches.fetch_add(1, Ordering::Relaxed);
    }
    
    /// Record map search operation
    pub fn record_map_search(&self) {
        self.map_searches.fetch_add(1, Ordering::Relaxed);
    }
    
    /// Record ASID reuse
    pub fn record_asid_reuse(&self, asid: u16) {
        self.asid_reuse_count.fetch_add(1, Ordering::Relaxed);
        debug!("[ASID_PROF] Reusing ASID {}", asid);
    }
    
    /// Record TLB operation
    pub fn record_tlb_operation(&self, op_type: TlbOperationType, asid: Option<u16>, time_cycles: u64) {
        self.tlb_flush_time_total.fetch_add(time_cycles, Ordering::Relaxed);
        
        match op_type {
            TlbOperationType::SingleAddress => {
                self.tlb_single_address_flushes.fetch_add(1, Ordering::Relaxed);
            }
            TlbOperationType::SingleContext => {
                self.tlb_single_context_flushes.fetch_add(1, Ordering::Relaxed);
            }
            TlbOperationType::AllContexts => {
                self.tlb_all_context_flushes.fetch_add(1, Ordering::Relaxed);
            }
            TlbOperationType::FullFlush => {
                self.tlb_full_flushes.fetch_add(1, Ordering::Relaxed);
            }
        }
        
        // Update per-ASID TLB flush count if applicable
        if let Some(asid) = asid {
            let mut per_asid = self.per_asid_stats.lock();
            if let Some(stats) = per_asid.get_mut(&asid) {
                stats.tlb_flushes += 1;
            }
        }
        
        debug!("[ASID_PROF] TLB {:?} operation in {} cycles", op_type, time_cycles);
    }
    
    /// Record context switch
    pub fn record_context_switch(&self, asid: u16, needed_flush: bool, time_cycles: u64) {
        self.context_switches.fetch_add(1, Ordering::Relaxed);
        self.context_switch_time_total.fetch_add(time_cycles, Ordering::Relaxed);
        
        if needed_flush {
            self.context_switches_with_flush.fetch_add(1, Ordering::Relaxed);
        }
        
        // Update per-ASID activation stats
        let mut per_asid = self.per_asid_stats.lock();
        if let Some(stats) = per_asid.get_mut(&asid) {
            stats.activation_count += 1;
            stats.last_used_timestamp = self.get_timestamp();
        }
        
        debug!("[ASID_PROF] Context switch to ASID {} (flush: {}) in {} cycles", 
               asid, needed_flush, time_cycles);
    }
    
    /// Record VM space activation
    pub fn record_vmspace_activation(&self) {
        self.vmspace_activations.fetch_add(1, Ordering::Relaxed);
    }
    
    /// Set PCID enabled status
    pub fn set_pcid_enabled(&self, enabled: bool) {
        self.pcid_enabled.store(if enabled { 1 } else { 0 }, Ordering::Relaxed);
        info!("[ASID_PROF] PCID support: {}", if enabled { "enabled" } else { "disabled" });
    }
    
    /// Get comprehensive statistics report
    pub fn get_report(&self) -> AsidStatsReport {
        let per_asid = self.per_asid_stats.lock();
        
        AsidStatsReport {
            // Basic counters
            allocations_total: self.allocations_total.load(Ordering::Relaxed),
            deallocations_total: self.deallocations_total.load(Ordering::Relaxed),
            allocation_failures: self.allocation_failures.load(Ordering::Relaxed),
            generation_rollovers: self.generation_rollovers.load(Ordering::Relaxed),
            
            // Search operations
            bitmap_searches: self.bitmap_searches.load(Ordering::Relaxed),
            map_searches: self.map_searches.load(Ordering::Relaxed),
            asid_reuse_count: self.asid_reuse_count.load(Ordering::Relaxed),
            
            // TLB operations
            tlb_single_address_flushes: self.tlb_single_address_flushes.load(Ordering::Relaxed),
            tlb_single_context_flushes: self.tlb_single_context_flushes.load(Ordering::Relaxed),
            tlb_all_context_flushes: self.tlb_all_context_flushes.load(Ordering::Relaxed),
            tlb_full_flushes: self.tlb_full_flushes.load(Ordering::Relaxed),
            
            // Context switches
            context_switches: self.context_switches.load(Ordering::Relaxed),
            context_switches_with_flush: self.context_switches_with_flush.load(Ordering::Relaxed),
            vmspace_activations: self.vmspace_activations.load(Ordering::Relaxed),
            
            // Performance timing
            allocation_time_total: self.allocation_time_total.load(Ordering::Relaxed),
            deallocation_time_total: self.deallocation_time_total.load(Ordering::Relaxed),
            tlb_flush_time_total: self.tlb_flush_time_total.load(Ordering::Relaxed),
            context_switch_time_total: self.context_switch_time_total.load(Ordering::Relaxed),
            
            // Current state
            active_asids: self.active_asids.load(Ordering::Relaxed),
            current_generation: self.current_generation.load(Ordering::Relaxed),
            pcid_enabled: self.pcid_enabled.load(Ordering::Relaxed) != 0,
            
            // Per-ASID summary
            total_asids_used: per_asid.len() as u32,
            per_asid_stats: per_asid.clone(),
        }
    }
    
    /// Reset all statistics
    pub fn reset(&self) {
        // Reset all atomic counters
        self.allocations_total.store(0, Ordering::Relaxed);
        self.deallocations_total.store(0, Ordering::Relaxed);
        self.allocation_failures.store(0, Ordering::Relaxed);
        self.generation_rollovers.store(0, Ordering::Relaxed);
        
        self.asid_reuse_count.store(0, Ordering::Relaxed);
        self.bitmap_searches.store(0, Ordering::Relaxed);
        self.map_searches.store(0, Ordering::Relaxed);
        
        self.tlb_single_address_flushes.store(0, Ordering::Relaxed);
        self.tlb_single_context_flushes.store(0, Ordering::Relaxed);
        self.tlb_all_context_flushes.store(0, Ordering::Relaxed);
        self.tlb_full_flushes.store(0, Ordering::Relaxed);
        
        self.context_switches.store(0, Ordering::Relaxed);
        self.context_switches_with_flush.store(0, Ordering::Relaxed);
        self.vmspace_activations.store(0, Ordering::Relaxed);
        
        self.allocation_time_total.store(0, Ordering::Relaxed);
        self.deallocation_time_total.store(0, Ordering::Relaxed);
        self.tlb_flush_time_total.store(0, Ordering::Relaxed);
        self.context_switch_time_total.store(0, Ordering::Relaxed);
        
        // Clear per-ASID stats
        self.per_asid_stats.lock().clear();
        
        info!("[ASID_PROF] Statistics reset");
    }
    
    /// Get current timestamp (using TSC if available)
    fn get_timestamp(&self) -> u64 {
        #[cfg(target_arch = "x86_64")]
        unsafe {
            core::arch::x86_64::_rdtsc()
        }
        #[cfg(not(target_arch = "x86_64"))]
        0 // Fallback for non-x86_64 architectures
    }
}

/// Types of TLB operations for profiling
#[derive(Debug, Clone, Copy)]
pub enum TlbOperationType {
    SingleAddress,
    SingleContext,
    AllContexts,
    FullFlush,
}

/// Comprehensive statistics report
#[derive(Debug, Clone)]
pub struct AsidStatsReport {
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
    pub pcid_enabled: bool,
    pub total_asids_used: u32,
    
    // Per-ASID statistics
    pub per_asid_stats: BTreeMap<u16, AsidUsageStats>,
}

impl AsidStatsReport {
    /// Print a detailed report
    pub fn print_report(&self) {
        info!("=== ASID Performance Report ===");
        info!("PCID Support: {}", if self.pcid_enabled { "Enabled" } else { "Disabled" });
        info!("Current Generation: {}", self.current_generation);
        info!("Active ASIDs: {}", self.active_asids);
        info!("Total ASIDs Used: {}", self.total_asids_used);
        
        info!("--- Allocation Statistics ---");
        info!("Total Allocations: {}", self.allocations_total);
        info!("Total Deallocations: {}", self.deallocations_total);
        info!("Allocation Failures: {}", self.allocation_failures);
        info!("Generation Rollovers: {}", self.generation_rollovers);
        info!("ASID Reuses: {}", self.asid_reuse_count);
        
        if self.allocations_total > 0 {
            info!("Avg Allocation Time: {} cycles", 
                  self.allocation_time_total / self.allocations_total);
        }
        
        info!("--- Search Operations ---");
        info!("Bitmap Searches: {}", self.bitmap_searches);
        info!("Map Searches: {}", self.map_searches);
        
        info!("--- TLB Operations ---");
        info!("Single Address Flushes: {}", self.tlb_single_address_flushes);
        info!("Single Context Flushes: {}", self.tlb_single_context_flushes);
        info!("All Context Flushes: {}", self.tlb_all_context_flushes);
        info!("Full TLB Flushes: {}", self.tlb_full_flushes);
        
        let total_tlb_ops = self.tlb_single_address_flushes + self.tlb_single_context_flushes +
                           self.tlb_all_context_flushes + self.tlb_full_flushes;
        if total_tlb_ops > 0 {
            info!("Avg TLB Flush Time: {} cycles", self.tlb_flush_time_total / total_tlb_ops);
        }
        
        info!("--- Context Switch Statistics ---");
        info!("Total Context Switches: {}", self.context_switches);
        info!("Context Switches with Flush: {}", self.context_switches_with_flush);
        info!("VM Space Activations: {}", self.vmspace_activations);
        
        if self.context_switches > 0 {
            let flush_percentage = (self.context_switches_with_flush * 100) / self.context_switches;
            info!("Flush Percentage: {}%", flush_percentage);
            info!("Avg Context Switch Time: {} cycles", 
                  self.context_switch_time_total / self.context_switches);
        }
        
        // Top 10 most used ASIDs
        let mut sorted_asids: alloc::vec::Vec<_> = self.per_asid_stats.iter().collect();
        sorted_asids.sort_by(|a, b| b.1.activation_count.cmp(&a.1.activation_count));
        
        info!("--- Top 10 Most Active ASIDs ---");
        for (i, (asid, stats)) in sorted_asids.iter().take(10).enumerate() {
            info!("{}. ASID {}: {} activations, {} allocations, {} TLB flushes",
                  i + 1, asid, stats.activation_count, stats.allocation_count, stats.tlb_flushes);
        }
    }
    
    /// Calculate efficiency metrics
    pub fn calculate_efficiency(&self) -> EfficiencyMetrics {
        EfficiencyMetrics {
            allocation_success_rate: if self.allocations_total + self.allocation_failures > 0 {
                (self.allocations_total as f64) / ((self.allocations_total + self.allocation_failures) as f64)
            } else {
                1.0
            },
            
            reuse_efficiency: if self.allocations_total > 0 {
                (self.asid_reuse_count as f64) / (self.allocations_total as f64)
            } else {
                0.0
            },
            
            flush_efficiency: if self.context_switches > 0 {
                1.0 - ((self.context_switches_with_flush as f64) / (self.context_switches as f64))
            } else {
                1.0
            },
            
            avg_cycles_per_allocation: if self.allocations_total > 0 {
                (self.allocation_time_total as f64) / (self.allocations_total as f64)
            } else {
                0.0
            },
            
            avg_cycles_per_context_switch: if self.context_switches > 0 {
                (self.context_switch_time_total as f64) / (self.context_switches as f64)
            } else {
                0.0
            },
        }
    }
}

/// Efficiency metrics calculated from the statistics
#[derive(Debug, Clone)]
pub struct EfficiencyMetrics {
    pub allocation_success_rate: f64,  // 0.0 to 1.0
    pub reuse_efficiency: f64,         // Higher is better
    pub flush_efficiency: f64,         // Higher is better (fewer flushes)
    pub avg_cycles_per_allocation: f64,
    pub avg_cycles_per_context_switch: f64,
}

/// Helper macro for timing ASID operations
#[macro_export]
macro_rules! profile_asid_operation {
    ($operation:expr) => {{
        #[cfg(target_arch = "x86_64")]
        let start_cycles = unsafe { core::arch::x86_64::_rdtsc() };
        #[cfg(not(target_arch = "x86_64"))]
        let start_cycles = 0u64;
        
        let result = $operation;
        
        #[cfg(target_arch = "x86_64")]
        let end_cycles = unsafe { core::arch::x86_64::_rdtsc() };
        #[cfg(not(target_arch = "x86_64"))]
        let end_cycles = 0u64;
        
        (result, end_cycles.saturating_sub(start_cycles))
    }};
}

/// Print current ASID statistics (convenience function)
pub fn print_asid_stats() {
    let report = ASID_STATS.get_report();
    report.print_report();
}

/// Reset ASID statistics (convenience function)
pub fn reset_asid_stats() {
    ASID_STATS.reset();
} 