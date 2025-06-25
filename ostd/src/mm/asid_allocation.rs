// SPDX-License-Identifier: MPL-2.0

//! Address Space ID (ASID) allocation.
//!
//! This module provides functions to allocate and deallocate ASIDs.



use log;

extern crate alloc;

use crate::{profile_asid_operation, mm::asid_profiling::ASID_STATS};

/// The maximum ASID value from the architecture.
///
/// When we run out of ASIDs, we use this special value to indicate
/// that the TLB entries for this address space need to be flushed
/// using INVPCID on context switch.
pub use crate::arch::mm::ASID_CAP;
use crate::sync::SpinLock;

/// The special ASID value that indicates the TLB entries for this
/// address space need to be flushed on context switch.
pub const ASID_FLUSH_REQUIRED: u16 = ASID_CAP;

/// The lowest ASID value that can be allocated.
///
/// ASID 0 is typically reserved for the kernel.
pub const ASID_MIN: u16 = 1;

/// Global ASID manager.
static ASID_MANAGER: SpinLock<AsidManager> = SpinLock::new(AsidManager::new());

/// ASID manager.
///
/// This structure manages the allocation and deallocation of ASIDs.
/// ASIDs are used to avoid TLB flushes when switching between processes.
struct AsidManager {
    /// The bitmap of allocated ASIDs.
    /// Each bit represents an ASID, where 1 means allocated and 0 means free.
    /// ASIDs start from ASID_MIN.
    bitmap: [u64; (ASID_CAP as usize - ASID_MIN as usize).div_ceil(64)],

    /// The next ASID to try to allocate.
    next: u16,

    /// Current ASID generation.
    current_generation: u16,
}

impl AsidManager {
    /// Creates a new ASID manager.
    const fn new() -> Self {
        Self {
            bitmap: [0; (ASID_CAP as usize - ASID_MIN as usize).div_ceil(64)],
            next: ASID_MIN,
            current_generation: 0,
        }
    }

    /// Finds and sets a free bit in the bitmap.
    ///
    /// Returns the allocated ASID if successful, or `None` if no free ASIDs are available.
    fn find_and_set_free_bit(&mut self) -> Option<u16> {
        ASID_STATS.record_bitmap_search();
        
        // Try to find a free ASID starting from `next`
        let start = self.next as usize - ASID_MIN as usize;

        // First search from next to end
        for i in start / 64..self.bitmap.len() {
            let word = self.bitmap[i];
            if word != u64::MAX {
                // Found a word with at least one free bit
                let bit = word.trailing_zeros() as usize;
                if bit < 64 {
                    let asid = ASID_MIN as usize + i * 64 + bit;
                    if asid <= ASID_CAP as usize {
                        self.bitmap[i] |= 1 << bit;
                        self.next = (asid + 1) as u16;
                        if self.next > ASID_CAP {
                            self.next = ASID_MIN;
                        }
                        return Some(asid as u16);
                    }
                }
            }
        }

        // Then search from beginning to next
        for i in 0..start / 64 {
            let word = self.bitmap[i];
            if word != u64::MAX {
                // Found a word with at least one free bit
                let bit = word.trailing_zeros() as usize;
                if bit < 64 {
                    let asid = ASID_MIN as usize + i * 64 + bit;
                    self.bitmap[i] |= 1 << bit;
                    self.next = (asid + 1) as u16;
                    return Some(asid as u16);
                }
            }
        }

        // No ASIDs available
        None
    }

    /// Allocates a new ASID.
    ///
    /// Returns the allocated ASID, or `ASID_FLUSH_REQUIRED` if no ASIDs are available.
    fn allocate(&mut self) -> u16 {
        // Try to find a free ASID
        if let Some(asid) = self.find_and_set_free_bit() {
            return asid;
        }

        // No ASIDs available - perform generation rollover and try again
        self.increment_generation();
        
        // After rollover, try allocation again
        // This should always succeed since we just reset the bitmap
        if let Some(asid) = self.find_and_set_free_bit() {
            asid
        } else {
            // If we still can't allocate after rollover, this indicates a serious problem
            // (e.g., ASID_CAP is 0 or invalid range)
            ASID_FLUSH_REQUIRED
        }
    }

    /// Deallocates an ASID.
    fn deallocate(&mut self, asid: u16) {
        // Don't deallocate the special ASID
        if asid == ASID_FLUSH_REQUIRED {
            return;
        }

        assert!((ASID_MIN..ASID_CAP).contains(&asid), "ASID out of range");

        let index = (asid as usize - ASID_MIN as usize) / 64;
        let bit = (asid as usize - ASID_MIN as usize) % 64;

        // Deallocate the ASID
        self.bitmap[index] &= !(1 << bit);
    }

    /// Increments the ASID generation and resets the bitmap.
    ///
    /// This is called when we run out of ASIDs and need to flush all TLBs.
    fn increment_generation(&mut self) {
        self.current_generation = self.current_generation.wrapping_add(1);
        
        // Reset the bitmap allocator
        self.bitmap = [0; (ASID_CAP as usize - ASID_MIN as usize).div_ceil(64)];
        self.next = ASID_MIN;
        
        // Record the generation rollover
        ASID_STATS.record_generation_rollover(self.current_generation);
    }
}

/// Allocates a new ASID.
///
/// Returns the allocated ASID, or `ASID_FLUSH_REQUIRED` if no ASIDs are available.
pub fn allocate() -> u16 {
    let (result, time_cycles) = profile_asid_operation!({
        ASID_MANAGER.lock().allocate()
    });
    
    if result == ASID_FLUSH_REQUIRED {
        ASID_STATS.record_allocation_failure();
    } else {
        ASID_STATS.record_allocation(result, time_cycles);
    }
    
    result
}



/// Deallocates an ASID.
pub fn deallocate(asid: u16) {
    if asid == ASID_FLUSH_REQUIRED {
        return;
    }

    let (_, time_cycles) = profile_asid_operation!({
        // Only deallocate from bitmap if it's in the valid range for the bitmap
        if (ASID_MIN..ASID_CAP).contains(&asid) {
            ASID_MANAGER.lock().deallocate(asid);
        }
    });
    
    ASID_STATS.record_deallocation(asid, time_cycles);
}

/// Gets the current ASID generation.
pub fn current_generation() -> u16 {
    ASID_MANAGER.lock().current_generation
}

/// Increments the ASID generation.
///
/// This is called when we run out of ASIDs and need to flush all TLBs.
pub fn increment_generation() {
    ASID_MANAGER.lock().increment_generation();
}
