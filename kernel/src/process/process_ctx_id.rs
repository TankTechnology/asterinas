// SPDX-License-Identifier: MPL-2.0

//! A process context id manager.
//!
//! This module provides functionality to allocate and manage unique process context IDs.
//! Each process context ID is guaranteed to be unique across the system.

use core::sync::atomic::{AtomicU32, Ordering};

use crate::prelude::*;
use id_alloc::IdAlloc;
use spin::Once;

/// Process Context ID.
pub type ProcessCtxId = u32;

/// A global allocator for process context IDs.
/// 
/// This simple implementation uses an atomic counter to ensure each ID is unique.
/// For systems that may recycle IDs, consider using the bitmap-based IdAlloc approach
/// shown in the more complex implementation below.
struct SimpleProcessCtxIdAllocator {
    next_id: AtomicU32,
}

impl SimpleProcessCtxIdAllocator {
    /// Creates a new ID allocator starting from 1
    pub const fn new() -> Self {
        Self {
            next_id: AtomicU32::new(1),
        }
    }

    /// Allocates a new process context ID.
    pub fn allocate(&self) -> ProcessCtxId {
        self.next_id.fetch_add(1, Ordering::SeqCst)
    }

    /// Returns the last allocated ID
    pub fn last_id(&self) -> ProcessCtxId {
        self.next_id.load(Ordering::SeqCst) - 1
    }
}

/// A global instance of the simple process context ID allocator.
static SIMPLE_PROCESS_CTX_ID_ALLOCATOR: SimpleProcessCtxIdAllocator = SimpleProcessCtxIdAllocator::new();

/// Allocates a new process context ID
pub fn allocate_process_ctx_id() -> ProcessCtxId {
    SIMPLE_PROCESS_CTX_ID_ALLOCATOR.allocate()
}

/// Returns the last allocated process context ID
pub fn last_process_ctx_id() -> ProcessCtxId {
    SIMPLE_PROCESS_CTX_ID_ALLOCATOR.last_id()
}

/// A more sophisticated process context ID manager using bitmap-based allocation.
///
/// This implementation uses a bitmap to track allocated IDs and allows for ID recycling.
/// It's more memory-efficient and allows reusing IDs when processes terminate.
pub struct ProcessCtxIdManager {
    /// The IdAlloc-based allocator for recycling IDs
    id_allocator: Mutex<IdAlloc>,
    /// Maximum number of process contexts supported
    max_process_contexts: usize,
}

impl ProcessCtxIdManager {
    /// Creates a new process context ID manager with the specified capacity
    pub fn new(max_process_contexts: usize) -> Self {
        Self {
            id_allocator: Mutex::new(IdAlloc::with_capacity(max_process_contexts)),
            max_process_contexts,
        }
    }

    /// Allocates a new process context ID
    ///
    /// Returns None if all IDs are already allocated.
    pub fn allocate(&self) -> Option<ProcessCtxId> {
        let mut id_allocator = self.id_allocator.lock();
        id_allocator.alloc().map(|id| id as ProcessCtxId)
    }

    /// Releases a previously allocated process context ID
    pub fn release(&self, id: ProcessCtxId) {
        if (id as usize) < self.max_process_contexts {
            let mut id_allocator = self.id_allocator.lock();
            id_allocator.free(id as usize);
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
        id_allocator.alloc_specific(id as usize).map(|id| id as ProcessCtxId)
    }
    
    /// Returns the maximum number of process contexts supported
    pub fn capacity(&self) -> usize {
        self.max_process_contexts
    }
}

/// A convenience function to create a process context ID manager with a default capacity
fn create_default_process_ctx_id_manager() -> ProcessCtxIdManager {
    const DEFAULT_MAX_PROCESS_CONTEXTS: usize = 65536; // 64K processes
    ProcessCtxIdManager::new(DEFAULT_MAX_PROCESS_CONTEXTS)
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_simple_allocator() {
        let allocator = SimpleProcessCtxIdAllocator::new();
        let id1 = allocator.allocate();
        let id2 = allocator.allocate();
        assert_eq!(id1 + 1, id2);
        assert_eq!(allocator.last_id(), id2);
        assert_eq!(0, 1);
    }

    #[test]
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
} 