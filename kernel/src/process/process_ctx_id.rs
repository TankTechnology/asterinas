// SPDX-License-Identifier: MPL-2.0

//! A process context id manager.
//!
//! This module provides functionality to allocate and manage unique process context IDs.
//! Each process context ID is guaranteed to be unique across the system.

use core::sync::atomic::{AtomicU32, Ordering};

use crate::prelude::*;
use id_alloc::IdAlloc;
use ostd::prelude::ktest;
use spin::Once;

#[cfg(target_arch = "x86_64")]
use crate::arch::x86::mm::pcid;

/// Process Context ID.
pub type ProcessCtxId = u32;

/// Process context ID allocation state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PcidState {
    /// PCID is available for allocation
    Free,
    /// PCID is allocated but not actively used
    Allocated,
    /// PCID is allocated and actively in use
    Active,
}

/// Process Context ID Manager.
///
/// This implementation provides two levels of PCID management:
/// 1. Traditional bitmap allocation for all processes
/// 2. Active PCID tracking to optimize for only actively running processes
///
/// This ensures we only need to manage PCIDs for processes that are actively running,
/// not all processes in the system.
pub struct ProcessCtxIdManager {
    /// The IdAlloc-based allocator for recycling IDs
    id_allocator: Mutex<IdAlloc>,
    /// Maximum number of process contexts supported
    max_process_contexts: usize,
    /// Tracks the state of each PCID (free, allocated, active)
    pcid_states: Mutex<Vec<PcidState>>,
    /// Set to true when hardware PCID support is available and enabled
    hw_pcid_supported: bool,
}

impl ProcessCtxIdManager {
    /// Creates a new process context ID manager with the specified capacity
    pub fn new(max_process_contexts: usize) -> Self {
        #[cfg(target_arch = "x86_64")]
        let hw_pcid_supported = pcid::is_pcid_supported();
        
        #[cfg(not(target_arch = "x86_64"))]
        let hw_pcid_supported = false;

        Self {
            id_allocator: Mutex::new(IdAlloc::with_capacity(max_process_contexts)),
            max_process_contexts,
            pcid_states: Mutex::new(vec![PcidState::Free; max_process_contexts]),
            hw_pcid_supported,
        }
    }

    /// Allocates a new process context ID
    ///
    /// Returns None if all IDs are already allocated.
    pub fn allocate(&self) -> Option<ProcessCtxId> {
        let mut id_allocator = self.id_allocator.lock();
        let id = id_allocator.alloc().map(|id| id as ProcessCtxId)?;
        
        let mut pcid_states = self.pcid_states.lock();
        if id as usize >= pcid_states.len() {
            return Some(id);
        }
        pcid_states[id as usize] = PcidState::Allocated;
        
        Some(id)
    }

    /// Releases a previously allocated process context ID
    pub fn release(&self, id: ProcessCtxId) {
        if (id as usize) < self.max_process_contexts {
            let mut id_allocator = self.id_allocator.lock();
            id_allocator.free(id as usize);
            
            let mut pcid_states = self.pcid_states.lock();
            if (id as usize) < pcid_states.len() {
                pcid_states[id as usize] = PcidState::Free;
            }
        }
    }

    /// Checks if a specific process context ID is currently allocated
    pub fn is_allocated(&self, id: ProcessCtxId) -> bool {
        if (id as usize) >= self.max_process_contexts {
            return false;
        }
        
        let id_allocator = self.id_allocator.lock();
        id_allocator.is_allocated(id as usize)
    }
    
    /// Attempts to allocate a specific process context ID
    ///
    /// Returns Some(id) if allocation was successful, None otherwise.
    pub fn allocate_specific(&self, id: ProcessCtxId) -> Option<ProcessCtxId> {
        if (id as usize) >= self.max_process_contexts {
            return None;
        }
        
        let mut id_allocator = self.id_allocator.lock();
        let allocated = id_allocator.alloc_specific(id as usize).map(|id| id as ProcessCtxId);
        
        if allocated.is_some() {
            let mut pcid_states = self.pcid_states.lock();
            if (id as usize) < pcid_states.len() {
                pcid_states[id as usize] = PcidState::Allocated;
            }
        }
        
        allocated
    }

    /// Sets a process context ID as active (currently in use)
    ///
    /// Returns None if the ID is not currently allocated.
    pub fn activate(&self, id: ProcessCtxId) -> Option<()> {
        if !self.is_allocated(id) || (id as usize) >= self.max_process_contexts {
            return None;
        }
        
        let mut pcid_states = self.pcid_states.lock();
        if (id as usize) < pcid_states.len() {
            pcid_states[id as usize] = PcidState::Active;
        }
        
        Some(())
    }
    
    /// Sets a process context ID as inactive (not currently in use)
    ///
    /// Returns None if the ID is not currently allocated.
    pub fn deactivate(&self, id: ProcessCtxId) -> Option<()> {
        if !self.is_allocated(id) || (id as usize) >= self.max_process_contexts {
            return None;
        }
        
        let mut pcid_states = self.pcid_states.lock();
        if (id as usize) < pcid_states.len() {
            pcid_states[id as usize] = PcidState::Allocated;
            
            // If PCID is supported, invalidate this PCID's TLB entries when deactivating
            #[cfg(target_arch = "x86_64")]
            {
                if self.hw_pcid_supported {
                    pcid::invalidate_pcid(id);
                }
            }
        }
        
        Some(())
    }
    
    /// Checks if a specific process context ID is currently active
    pub fn is_active(&self, id: ProcessCtxId) -> bool {
        if (id as usize) >= self.max_process_contexts {
            return false;
        }
        
        let pcid_states = self.pcid_states.lock();
        if (id as usize) < pcid_states.len() {
            return pcid_states[id as usize] == PcidState::Active;
        }
        
        false
    }
    
    /// Returns the total number of currently active PCIDs
    pub fn active_count(&self) -> usize {
        let pcid_states = self.pcid_states.lock();
        pcid_states.iter().filter(|&&state| state == PcidState::Active).count()
    }
    
    /// Returns the maximum number of process contexts supported
    pub fn capacity(&self) -> usize {
        self.max_process_contexts
    }
    
    /// Returns whether hardware PCID support is available
    pub fn has_hw_pcid_support(&self) -> bool {
        self.hw_pcid_supported
    }
}

/// A convenience function to create a process context ID manager with a default capacity
fn create_default_process_ctx_id_manager() -> ProcessCtxIdManager {
    // Default to 4096 processes (the limit of 12-bit PCIDs in x86_64)
    #[cfg(target_arch = "x86_64")]
    let default_contexts = pcid::PCID_CAP as usize;
    
    #[cfg(not(target_arch = "x86_64"))]
    let default_contexts = 4096;
    
    ProcessCtxIdManager::new(default_contexts)
}

/// A static instance of the ProcessCtxIdManager with lazy initialization
static PROCESS_CTX_ID_MANAGER: Once<ProcessCtxIdManager> = Once::new();

/// Initializes the global process context ID manager
pub fn init() {
    PROCESS_CTX_ID_MANAGER.call_once(create_default_process_ctx_id_manager);
}

/// Gets the global process context ID manager
pub fn process_ctx_id_manager() -> &'static ProcessCtxIdManager {
    PROCESS_CTX_ID_MANAGER.get().expect("ProcessCtxIdManager not initialized")
}

#[cfg(ktest)]
mod tests {
    use super::*;

    #[ktest]
    fn test_bitmap_manager() {
        let manager = ProcessCtxIdManager::new(10);
        
        // Allocate all IDs
        let mut ids = Vec::new();
        for _ in 0..10 {
            let id = manager.allocate();
            assert!(id.is_some());
            ids.push(id.unwrap());
        }
        
        // Should be full now
        assert!(manager.allocate().is_none());
        
        // Release one ID
        manager.release(ids[5]);
        
        // Should be able to allocate one more
        let new_id = manager.allocate();
        assert!(new_id.is_some());
        assert_eq!(new_id.unwrap(), ids[5]);
        
        // Try specific allocation
        manager.release(ids[3]);
        assert!(manager.is_allocated(ids[3]) == false);
        let specific = manager.allocate_specific(ids[3]);
        assert_eq!(specific, Some(ids[3]));
        assert!(manager.is_allocated(ids[3]));
    }
    
    #[ktest]
    fn test_pcid_activation() {
        let manager = ProcessCtxIdManager::new(10);
        
        // Allocate an ID
        let id = manager.allocate().unwrap();
        
        // Should not be active by default
        assert!(!manager.is_active(id));
        
        // Activate the ID
        manager.activate(id);
        
        // Should be active now
        assert!(manager.is_active(id));
        
        // One active ID
        assert_eq!(manager.active_count(), 1);
        
        // Deactivate the ID
        manager.deactivate(id);
        
        // Should not be active now
        assert!(!manager.is_active(id));
        
        // No active IDs
        assert_eq!(manager.active_count(), 0);
    }
} 