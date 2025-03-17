// SPDX-License-Identifier: MPL-2.0

//! Example usage of the process context ID manager.

use super::process_ctx_id::{allocate_process_ctx_id, last_process_ctx_id, ProcessCtxId, process_ctx_id_manager};
use crate::prelude::*;

/// Process Context represents the state and resources associated with a process.
/// This is a simplified example structure.
pub struct ProcessContext {
    /// The unique ID for this process context
    id: ProcessCtxId,
    /// Process name for demonstration
    name: String,
    /// Other process context data would go here...
}

impl ProcessContext {
    /// Creates a new process context with the given name
    pub fn new(name: &str) -> Self {
        // Use the simple global allocator for basic sequential IDs
        let id = allocate_process_ctx_id();
        
        Self {
            id,
            name: name.to_string(),
        }
    }
    
    /// Creates a new process context with a specific ID
    pub fn with_specific_id(name: &str, desired_id: ProcessCtxId) -> Result<Self> {
        // Use the more sophisticated manager for advanced ID allocation
        let manager = process_ctx_id_manager();
        
        // Try to allocate the specific ID
        if let Some(id) = manager.allocate_specific(desired_id) {
            Ok(Self {
                id,
                name: name.to_string(),
            })
        } else {
            Err(Error::new(Errno::EBUSY, "Failed to allocate specific process context ID"))
        }
    }
    
    /// Returns the ID of this process context
    pub fn id(&self) -> ProcessCtxId {
        self.id
    }
    
    /// Returns the name of this process context
    pub fn name(&self) -> &str {
        &self.name
    }
}

impl Drop for ProcessContext {
    fn drop(&mut self) {
        // When a process context is dropped, release its ID back to the pool
        process_ctx_id_manager().release(self.id);
        debug!("Process context {} ('{}') released", self.id, self.name);
    }
}

/// A container that manages multiple process contexts
pub struct ProcessContextManager {
    contexts: Mutex<BTreeMap<ProcessCtxId, Arc<ProcessContext>>>,
}

impl ProcessContextManager {
    /// Creates a new process context manager
    pub fn new() -> Self {
        Self {
            contexts: Mutex::new(BTreeMap::new()),
        }
    }
    
    /// Creates a new process context and adds it to the manager
    pub fn create_context(&self, name: &str) -> Result<Arc<ProcessContext>> {
        let context = Arc::new(ProcessContext::new(name));
        let id = context.id();
        
        let mut contexts = self.contexts.lock();
        contexts.insert(id, context.clone());
        
        Ok(context)
    }
    
    /// Creates a process context with a specific ID and adds it to the manager
    pub fn create_context_with_id(&self, name: &str, id: ProcessCtxId) -> Result<Arc<ProcessContext>> {
        let context = Arc::new(ProcessContext::with_specific_id(name, id)?);
        
        let mut contexts = self.contexts.lock();
        contexts.insert(id, context.clone());
        
        Ok(context)
    }
    
    /// Gets a process context by ID
    pub fn get_context(&self, id: ProcessCtxId) -> Option<Arc<ProcessContext>> {
        self.contexts.lock().get(&id).cloned()
    }
    
    /// Removes a process context from the manager
    pub fn remove_context(&self, id: ProcessCtxId) -> Option<Arc<ProcessContext>> {
        self.contexts.lock().remove(&id)
    }
    
    /// Returns the number of managed process contexts
    pub fn count(&self) -> usize {
        self.contexts.lock().len()
    }
}

/// Example function showing how to use the process context ID manager
pub fn demonstrate_process_ctx_id_usage() -> Result<()> {
    info!("Demonstrating process context ID manager...");
    
    // Initialize the process context ID manager (normally done at kernel startup)
    super::process_ctx_id::init();
    
    // Create a process context manager
    let manager = ProcessContextManager::new();
    
    // Create some process contexts
    let ctx1 = manager.create_context("Process 1")?;
    let ctx2 = manager.create_context("Process 2")?;
    let ctx3 = manager.create_context("Process 3")?;
    
    info!("Created process contexts with IDs: {}, {}, {}", 
          ctx1.id(), ctx2.id(), ctx3.id());
    
    // Try to create a process with a specific ID
    match manager.create_context_with_id("Special Process", 100) {
        Ok(ctx) => info!("Created special process with ID: {}", ctx.id()),
        Err(e) => warn!("Failed to create special process: {}", e),
    }
    
    // Release a process context
    let removed = manager.remove_context(ctx2.id());
    if let Some(ctx) = removed {
        info!("Removed process context {} ('{}')", ctx.id(), ctx.name());
    }
    
    // The ID should now be available for reuse
    let released_id = ctx2.id();
    if process_ctx_id_manager().is_allocated(released_id) {
        warn!("ID {} should have been released!", released_id);
    } else {
        info!("ID {} was successfully released", released_id);
    }
    
    // Try to allocate the same ID again
    match manager.create_context_with_id("Recycled Process", released_id) {
        Ok(ctx) => info!("Successfully recycled ID {} for process '{}'", ctx.id(), ctx.name()),
        Err(e) => warn!("Failed to recycle ID {}: {}", released_id, e),
    }
    
    info!("Process context ID manager demonstration completed.");
    info!("Last allocated ID: {}", last_process_ctx_id());
    info!("Number of managed contexts: {}", manager.count());
    
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    
    #[test]
    fn test_process_context_manager() {
        super::super::process_ctx_id::init();
        
        let manager = ProcessContextManager::new();
        
        // Create some contexts
        let ctx1 = manager.create_context("Test Process 1").unwrap();
        let ctx2 = manager.create_context("Test Process 2").unwrap();
        
        assert_eq!(manager.count(), 2);
        
        // Check we can retrieve them
        let retrieved = manager.get_context(ctx1.id()).unwrap();
        assert_eq!(retrieved.name(), "Test Process 1");
        
        // Remove one
        let removed = manager.remove_context(ctx2.id()).unwrap();
        assert_eq!(removed.name(), "Test Process 2");
        assert_eq!(manager.count(), 1);
        
        // ID should be released now
        assert!(!process_ctx_id_manager().is_allocated(ctx2.id()));
        
        // Should be able to reuse the ID
        let recycled = manager.create_context_with_id("Recycled", ctx2.id()).unwrap();
        assert_eq!(recycled.id(), ctx2.id());
        assert_eq!(recycled.name(), "Recycled");
    }
} 