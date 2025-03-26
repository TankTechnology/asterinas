// SPDX-License-Identifier: MPL-2.0

//! VmSpace extension to support Process Context IDs (PCIDs).
//!
//! This module extends the VmSpace with PCID support to improve
//! TLB efficiency during context switches.

use core::sync::atomic::Ordering;

use alloc::sync::Arc;
use ostd::mm::{PageProperty, VmSpace};

use crate::{
    prelude::*,
    process::process_ctx_id::{process_ctx_id_manager, ProcessCtxId}
};

#[cfg(target_arch = "x86_64")]
use crate::arch::x86::mm::pcid::{
    is_pcid_supported, set_cr3_with_pcid, invalidate_pcid, PCID_INVALID
};

/// Trait to extend VmSpace with PCID functionality
pub trait VmSpacePcidExt {
    /// Allocate a PCID for this VmSpace
    fn allocate_pcid(&self) -> Option<ProcessCtxId>;
    
    /// Get the current PCID for this VmSpace if allocated
    fn get_pcid(&self) -> Option<ProcessCtxId>;
    
    /// Activate this VmSpace with its PCID
    fn activate_with_pcid(self: &Arc<Self>);
    
    /// Release the PCID when the VmSpace is dropped
    fn release_pcid(&self);
}

// PCID storage in VmSpace - using thread_local to avoid changing VmSpace struct
thread_local! {
    static VMSPACE_PCIDS: Mutex<HashMap<*const VmSpace, ProcessCtxId>> = 
        Mutex::new(HashMap::new());
}

impl VmSpacePcidExt for VmSpace {
    fn allocate_pcid(&self) -> Option<ProcessCtxId> {
        // Check if we already have a PCID
        if let Some(pcid) = self.get_pcid() {
            return Some(pcid);
        }
        
        let pcid_manager = process_ctx_id_manager();
        if !pcid_manager.has_hw_pcid_support() {
            // If hardware doesn't support PCID, use the invalid value
            return Some(PCID_INVALID);
        }
        
        // Allocate a new PCID
        let pcid = pcid_manager.allocate()?;
        
        // Store it in our map
        VMSPACE_PCIDS.with(|pcids| {
            let mut pcids = pcids.lock();
            pcids.insert(self as *const _, pcid);
        });
        
        Some(pcid)
    }
    
    fn get_pcid(&self) -> Option<ProcessCtxId> {
        VMSPACE_PCIDS.with(|pcids| {
            let pcids = pcids.lock();
            pcids.get(&(self as *const _)).copied()
        })
    }
    
    fn activate_with_pcid(self: &Arc<Self>) {
        // Get or allocate PCID
        let pcid = self.allocate_pcid().unwrap_or(PCID_INVALID);
        
        // Mark as active in the PCID manager
        if pcid != PCID_INVALID {
            let pcid_manager = process_ctx_id_manager();
            pcid_manager.activate(pcid);
        }
        
        // Activate the VmSpace normally
        self.activate();
        
        #[cfg(target_arch = "x86_64")]
        if is_pcid_supported() {
            // Get page table physical address
            let cpu_info = disable_preempt();
            let current_cpu = cpu_info.current_cpu();
            
            // If we were the last activated VmSpace, we can use the NOFLUSH flag
            let pt_paddr = self.pt.get_paddr();
            
            // Set CR3 with the PCID
            unsafe {
                set_cr3_with_pcid(pt_paddr, pcid);
            }
        }
    }
    
    fn release_pcid(&self) {
        let pcid_opt = VMSPACE_PCIDS.with(|pcids| {
            let mut pcids = pcids.lock();
            pcids.remove(&(self as *const _))
        });
        
        if let Some(pcid) = pcid_opt {
            if pcid != PCID_INVALID {
                // Mark as inactive first
                let pcid_manager = process_ctx_id_manager();
                let _ = pcid_manager.deactivate(pcid);
                
                // Then release it
                pcid_manager.release(pcid);
            }
        }
    }
} 