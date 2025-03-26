// SPDX-License-Identifier: MPL-2.0

//! PCID (Process Context ID) support for x86_64.
//!
//! This module provides support for PCID on x86_64 processors.
//! PCID allows the CPU to maintain multiple TLB entries for different address spaces,
//! avoiding full TLB flushes during context switches.

use core::arch::x86_64::{__cpuid, _invpcid, _rdmsr, _wrmsr};

use ostd::prelude::*;
use spin::{Mutex, Once};
use x86::bits64::tlb;
use x86_64::registers::control::{Cr3, Cr3Flags, Cr4, Cr4Flags};

/// Enum for INVPCID instruction types
#[repr(u64)]
#[derive(Debug, Clone, Copy)]
pub enum InvpcidType {
    /// Invalidate a specific address
    IndividualAddress = 0,
    /// Invalidate all except globals
    SingleContext = 1,
    /// Invalidate by PCID all entries with matching PCID except globals
    AllContextExceptGlobal = 2,
    /// Invalidate by PCID all entries with matching PCID including globals
    AllContext = 3,
}

/// Structure for INVPCID descriptor (used by INVPCID instruction)
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct InvpcidDesc {
    /// PCID to be invalidated
    pcid: u64,
    /// Virtual address to be invalidated
    addr: u64,
    /// Reserved
    reserved: [u64; 2],
}

/// PCID capability limit for x86_64 processors (12 bits available)
pub const PCID_CAP: u32 = 4096;

/// Invalid PCID value used when all PCIDs are allocated
pub const PCID_INVALID: u32 = PCID_CAP;

/// Check if PCID is supported by the CPU
pub fn is_pcid_supported() -> bool {
    let cpu_info = unsafe { __cpuid(1) };
    // Check PCID feature flag in CPUID.1:ECX.PCID[bit 17]
    (cpu_info.ecx & (1 << 17)) != 0
}

/// Check if INVPCID instruction is supported by the CPU
pub fn is_invpcid_supported() -> bool {
    let cpu_info = unsafe { __cpuid(7) };
    // Check INVPCID feature flag in CPUID.7:EBX.INVPCID[bit 10]
    (cpu_info.ebx & (1 << 10)) != 0
}

/// Initialize PCID support if available
pub fn init() -> bool {
    let pcid_supported = is_pcid_supported();
    let invpcid_supported = is_invpcid_supported();
    
    if pcid_supported {
        log::info!("PCID support detected. Enabling PCID.");
        if enable_pcid() {
            log::info!("PCID enabled successfully.");
            
            if invpcid_supported {
                log::info!("INVPCID instruction supported.");
            } else {
                log::info!("INVPCID instruction not supported, falling back to CR3 reloading for TLB invalidation.");
            }
            
            return true;
        } else {
            log::warn!("Failed to enable PCID even though it's supported.");
        }
    } else {
        log::info!("PCID not supported by this CPU.");
    }
    
    false
}

/// Enable PCID support in CR4 register
pub fn enable_pcid() -> bool {
    if !is_pcid_supported() {
        return false;
    }

    unsafe {
        // Set CR4.PCIDE (bit 17)
        Cr4::update(|flags| {
            *flags |= Cr4Flags::PCID;
        });
    }
    true
}

/// Perform INVPCID instruction
///
/// # Safety
///
/// This is unsafe because it can invalidate TLB entries which might be needed.
pub unsafe fn invpcid(typ: InvpcidType, pcid: u32, addr: usize) {
    // Only execute if INVPCID instruction is supported
    if is_invpcid_supported() {
        let desc = InvpcidDesc {
            pcid: pcid as u64,
            addr: addr as u64,
            reserved: [0; 2],
        };
        _invpcid(typ as u64, &desc as *const _);
    } else {
        // Fallback to CR3 reloading if INVPCID not supported
        flush_by_cr3_reload();
    }
}

/// Invalidate all TLB entries for a specific PCID
pub fn invalidate_pcid(pcid: u32) {
    unsafe {
        invpcid(InvpcidType::SingleContext, pcid, 0);
    }
}

/// Invalidate a specific address in a specific PCID
pub fn invalidate_addr_pcid(pcid: u32, addr: usize) {
    unsafe {
        invpcid(InvpcidType::IndividualAddress, pcid, addr);
    }
}

/// Invalidate all TLB entries for all PCIDs (except global pages)
pub fn invalidate_all_pcids_except_global() {
    unsafe {
        invpcid(InvpcidType::AllContextExceptGlobal, 0, 0);
    }
}

/// Invalidate all TLB entries including globals
pub fn invalidate_all_pcids_including_global() {
    unsafe {
        invpcid(InvpcidType::AllContext, 0, 0);
    }
}

/// Flush TLB by reloading CR3 (fallback method when INVPCID is not available)
unsafe fn flush_by_cr3_reload() {
    let (addr, flags) = Cr3::read();
    Cr3::write(addr, flags);
}

/// Set CR3 with a specific PCID
///
/// # Safety
///
/// This is unsafe because it changes the active page table.
pub unsafe fn set_cr3_with_pcid(page_table_addr: usize, pcid: u32) {
    let pcid_flags = if pcid == PCID_INVALID {
        Cr3Flags::empty()
    } else {
        Cr3Flags::from_bits_truncate((pcid & 0xFFF) as u64)
    };
    
    // When PCID is enabled, set NOFLUSH bit if we're updating PCID for the same page table
    let noflush = if Cr4::read().contains(Cr4Flags::PCID) {
        let (current_addr, _) = Cr3::read();
        if current_addr.as_u64() as usize == page_table_addr {
            Cr3Flags::PAGE_LEVEL_CACHE_DISABLE
        } else {
            Cr3Flags::empty()
        }
    } else {
        Cr3Flags::empty()
    };
    
    Cr3::write(
        x86_64::PhysAddr::new(page_table_addr as u64),
        pcid_flags | noflush
    );
} 