// SPDX-License-Identifier: MPL-2.0

use core::arch::global_asm;

use crate::{
    boot::{
        memory_region::{MemoryRegion, MemoryRegionArray, MemoryRegionType},
        BootloaderAcpiArg, BootloaderFramebufferArg,
    },
    mm::{
        kspace::{paddr_to_vaddr, LINEAR_MAPPING_BASE_VADDR},
        Paddr, Vaddr,
    },
};

global_asm!(include_str!("header.S"));

pub(super) const MULTIBOOT_ENTRY_MAGIC: u32 = 0x2BADB002;

fn parse_bootloader_name(mb1_info: &MultibootLegacyInfo) -> &str {
    let mut name = "Unknown Multiboot loader";
    if mb1_info.boot_loader_name != 0 {
        let ptr = paddr_to_vaddr(mb1_info.boot_loader_name as usize) as *const i8;
        // SAFETY: the bootloader name is C-style zero-terminated string.
        let cstr = unsafe { core::ffi::CStr::from_ptr(ptr) };
        if let Ok(s) = cstr.to_str() {
            name = s;
        }
    }
    name
}

fn parse_kernel_commandline(mb1_info: &MultibootLegacyInfo) -> &str {
    let mut cmdline = "";
    if mb1_info.cmdline != 0 {
        let ptr = paddr_to_vaddr(mb1_info.cmdline as usize) as *const i8;
        // SAFETY: the command line is C-style zero-terminated string.
        let cstr = unsafe { core::ffi::CStr::from_ptr(ptr) };
        cmdline = cstr.to_str().unwrap();
    }
    cmdline
}

fn parse_initramfs(mb1_info: &MultibootLegacyInfo) -> Option<&[u8]> {
    // FIXME: We think all modules are initramfs, can this cause problems?
    if mb1_info.mods_count == 0 {
        return None;
    }
    let modules_addr = mb1_info.mods_addr as usize;
    // We only use one module
    let (start, end) = unsafe {
        (
            (*(paddr_to_vaddr(modules_addr) as *const u32)) as usize,
            (*(paddr_to_vaddr(modules_addr + 4) as *const u32)) as usize,
        )
    };
    // We must return a slice composed by VA since kernel should read every in VA.
    let base_va = if start < LINEAR_MAPPING_BASE_VADDR {
        paddr_to_vaddr(start)
    } else {
        start
    };
    let length = end - start;

    Some(unsafe { core::slice::from_raw_parts(base_va as *const u8, length) })
}

fn parse_acpi_arg(_mb1_info: &MultibootLegacyInfo) -> BootloaderAcpiArg {
    // The multiboot protocol does not contain RSDP address.
    // TODO: What about UEFI?
    BootloaderAcpiArg::NotProvided
}

fn parse_framebuffer_info(mb1_info: &MultibootLegacyInfo) -> Option<BootloaderFramebufferArg> {
    if mb1_info.framebuffer_table.addr == 0 {
        return None;
    }
    Some(BootloaderFramebufferArg {
        address: mb1_info.framebuffer_table.addr as usize,
        width: mb1_info.framebuffer_table.width as usize,
        height: mb1_info.framebuffer_table.height as usize,
        bpp: mb1_info.framebuffer_table.bpp as usize,
    })
}

fn parse_memory_regions(mb1_info: &MultibootLegacyInfo) -> MemoryRegionArray {
    let mut regions = MemoryRegionArray::new();

    // Add the regions in the multiboot protocol.
    for entry in mb1_info.get_memory_map() {
        let start = entry.base_addr();
        let region = MemoryRegion::new(
            start.try_into().unwrap(),
            entry.length().try_into().unwrap(),
            entry.memory_type(),
        );
        regions.push(region).unwrap();
    }

    // Add the framebuffer region.
    let fb = BootloaderFramebufferArg {
        address: mb1_info.framebuffer_table.addr as usize,
        width: mb1_info.framebuffer_table.width as usize,
        height: mb1_info.framebuffer_table.height as usize,
        bpp: mb1_info.framebuffer_table.bpp as usize,
    };
    regions
        .push(MemoryRegion::new(
            fb.address,
            (fb.width * fb.height * fb.bpp).div_ceil(8), // round up when divide with 8 (bits/Byte)
            MemoryRegionType::Framebuffer,
        ))
        .unwrap();

    // Add the kernel region.
    regions.push(MemoryRegion::kernel()).unwrap();

    // Add the initramfs area.
    if mb1_info.mods_count != 0 {
        let modules_addr = mb1_info.mods_addr as usize;
        // We only use one module
        let (start, end) = unsafe {
            (
                (*(paddr_to_vaddr(modules_addr) as *const u32)) as usize,
                (*(paddr_to_vaddr(modules_addr + 4) as *const u32)) as usize,
            )
        };
        regions
            .push(MemoryRegion::new(
                start,
                end - start,
                MemoryRegionType::Module,
            ))
            .unwrap();
    }

    // Add the AP boot code region that will be copied into by the BSP.
    regions
        .push(MemoryRegion::new(
            super::smp::AP_BOOT_START_PA,
            super::smp::ap_boot_code_size(),
            MemoryRegionType::Reclaimable,
        ))
        .unwrap();

    // Add the kernel cmdline and boot loader name region since Grub does not specify it.
    let kcmdline = parse_kernel_commandline(mb1_info);
    regions
        .push(MemoryRegion::new(
            kcmdline.as_ptr() as Paddr - LINEAR_MAPPING_BASE_VADDR,
            kcmdline.len(),
            MemoryRegionType::Reclaimable,
        ))
        .unwrap();
    let bootloader_name = parse_bootloader_name(mb1_info);
    regions
        .push(MemoryRegion::new(
            bootloader_name.as_ptr() as Paddr - LINEAR_MAPPING_BASE_VADDR,
            bootloader_name.len(),
            MemoryRegionType::Reclaimable,
        ))
        .unwrap();

    regions.into_non_overlapping()
}

/// Representation of Multiboot Information according to specification.
///
/// Ref:https://www.gnu.org/software/grub/manual/multiboot/multiboot.html#Boot-information-format
///
///```text
///         +-------------------+
/// 0       | flags             |    (required)
///         +-------------------+
/// 4       | mem_lower         |    (present if flags[0] is set)
/// 8       | mem_upper         |    (present if flags[0] is set)
///         +-------------------+
/// 12      | boot_device       |    (present if flags[1] is set)
///         +-------------------+
/// 16      | cmdline           |    (present if flags[2] is set)
///         +-------------------+
/// 20      | mods_count        |    (present if flags[3] is set)
/// 24      | mods_addr         |    (present if flags[3] is set)
///         +-------------------+
/// 28 - 40 | syms              |    (present if flags[4] or
///         |                   |                flags[5] is set)
///         +-------------------+
/// 44      | mmap_length       |    (present if flags[6] is set)
/// 48      | mmap_addr         |    (present if flags[6] is set)
///         +-------------------+
/// 52      | drives_length     |    (present if flags[7] is set)
/// 56      | drives_addr       |    (present if flags[7] is set)
///         +-------------------+
/// 60      | config_table      |    (present if flags[8] is set)
///         +-------------------+
/// 64      | boot_loader_name  |    (present if flags[9] is set)
///         +-------------------+
/// 68      | apm_table         |    (present if flags[10] is set)
///         +-------------------+
/// 72      | vbe_control_info  |    (present if flags[11] is set)
/// 76      | vbe_mode_info     |
/// 80      | vbe_mode          |
/// 82      | vbe_interface_seg |
/// 84      | vbe_interface_off |
/// 86      | vbe_interface_len |
///         +-------------------+
/// 88      | framebuffer_addr  |    (present if flags[12] is set)
/// 96      | framebuffer_pitch |
/// 100     | framebuffer_width |
/// 104     | framebuffer_height|
/// 108     | framebuffer_bpp   |
/// 109     | framebuffer_type  |
/// 110-115 | color_info        |
///         +-------------------+
///```
///
#[derive(Debug, Copy, Clone)]
#[repr(C, packed)]
struct MultibootLegacyInfo {
    /// Indicate whether the below field exists.
    flags: u32,

    /// Physical memory low.
    mem_lower: u32,
    /// Physical memory high.
    mem_upper: u32,

    /// Indicates which BIOS disk device the boot loader loaded the OS image from.
    boot_device: u32,

    /// Command line passed to kernel.
    cmdline: u32,

    /// Modules count.
    mods_count: u32,
    /// The start address of modules list, each module structure format:
    /// ```text
    ///         +-------------------+
    /// 0       | mod_start         |
    /// 4       | mod_end           |
    ///         +-------------------+
    /// 8       | string            |
    ///         +-------------------+
    /// 12      | reserved (0)      |
    ///         +-------------------+
    /// ```
    mods_addr: u32,

    /// If flags[4] = 1, then the field starting at byte 28 are valid:
    /// ```text
    ///         +-------------------+
    /// 28      | tabsize           |
    /// 32      | strsize           |
    /// 36      | addr              |
    /// 40      | reserved (0)      |
    ///         +-------------------+
    /// ```
    /// These indicate where the symbol table from kernel image can be found.
    ///
    /// If flags[5] = 1, then the field starting at byte 28 are valid:
    /// ```text
    ///         +-------------------+
    /// 28      | num               |
    /// 32      | size              |
    /// 36      | addr              |
    /// 40      | shndx             |
    ///         +-------------------+
    /// ```
    /// These indicate where the section header table from an ELF kernel is,
    /// the size of each entry, number of entries, and the string table used as the index of names.
    symbols: [u8; 16],

    memory_map_len: u32,
    memory_map_addr: u32,

    drives_length: u32,
    drives_addr: u32,

    config_table: u32,

    boot_loader_name: u32,

    apm_table: u32,

    vbe_table: VbeTable,

    framebuffer_table: FramebufferTable,
}

impl MultibootLegacyInfo {
    fn get_memory_map(&self) -> MemoryEntryIter {
        let ptr = self.memory_map_addr as Paddr;
        let end = ptr + self.memory_map_len as usize;
        MemoryEntryIter {
            cur_ptr: paddr_to_vaddr(ptr),
            region_end: paddr_to_vaddr(end),
        }
    }
}

#[derive(Debug, Copy, Clone)]
#[repr(C, packed)]
struct VbeTable {
    control_info: u32,
    mode_info: u32,
    mode: u16,
    interface_seg: u16,
    interface_off: u16,
    interface_len: u16,
}

#[derive(Debug, Copy, Clone)]
#[repr(C, packed)]
struct FramebufferTable {
    addr: u64,
    pitch: u32,
    width: u32,
    height: u32,
    bpp: u8,
    typ: u8,
    color_info: [u8; 6],
}

/// A memory entry in the memory map header info region.
///
/// The memory layout of the entry structure doesn't fit in any scheme
/// provided by Rust:
///
/// ```text
///         +-------------------+   <- start of the struct pointer
/// -4      | size              |
///         +-------------------+
/// 0       | base_addr         |
/// 8       | length            |
/// 16      | type              |
///         +-------------------+
/// ```
///
/// The start of a entry is not 64-bit aligned. Although the boot
/// protocol may provide the `mmap_addr` 64-bit aligned when added with
/// 4, it is not guaranteed. So we need to use pointer arithmetic to
/// access the fields.
struct MemoryEntry {
    ptr: Vaddr,
}

impl MemoryEntry {
    fn size(&self) -> u32 {
        // SAFETY: the entry can only be constructed from a valid address.
        unsafe { (self.ptr as *const u32).read_unaligned() }
    }

    fn base_addr(&self) -> u64 {
        // SAFETY: the entry can only be constructed from a valid address.
        unsafe { ((self.ptr + 4) as *const u64).read_unaligned() }
    }

    fn length(&self) -> u64 {
        // SAFETY: the entry can only be constructed from a valid address.
        unsafe { ((self.ptr + 12) as *const u64).read_unaligned() }
    }

    fn memory_type(&self) -> MemoryRegionType {
        // The multiboot (v1) manual doesn't specify the length of the type field.
        // Experimental result shows that "u8" works. So be it.
        // SAFETY: the entry can only be constructed from a valid address.
        let typ_val = unsafe { ((self.ptr + 20) as *const u8).read_unaligned() };
        // The meaning of the values are however documented clearly by the manual.
        match typ_val {
            1 => MemoryRegionType::Usable,
            2 => MemoryRegionType::Reserved,
            3 => MemoryRegionType::Reclaimable,
            4 => MemoryRegionType::NonVolatileSleep,
            5 => MemoryRegionType::BadMemory,
            _ => MemoryRegionType::Reserved,
        }
    }
}

/// A memory entry iterator in the memory map header info region.
#[derive(Debug, Copy, Clone)]
struct MemoryEntryIter {
    cur_ptr: Vaddr,
    region_end: Vaddr,
}

impl Iterator for MemoryEntryIter {
    type Item = MemoryEntry;

    fn next(&mut self) -> Option<Self::Item> {
        if self.cur_ptr >= self.region_end {
            return None;
        }
        let entry = MemoryEntry { ptr: self.cur_ptr };
        self.cur_ptr += entry.size() as usize + 4;
        Some(entry)
    }
}

/// The entry point of Rust code called by inline asm.
#[no_mangle]
unsafe extern "sysv64" fn __multiboot_entry(boot_magic: u32, boot_params: u64) -> ! {
    assert_eq!(boot_magic, MULTIBOOT_ENTRY_MAGIC);
    let mb1_info =
        unsafe { &*(paddr_to_vaddr(boot_params as usize) as *const MultibootLegacyInfo) };

    use crate::boot::{call_ostd_main, EarlyBootInfo, EARLY_INFO};

    EARLY_INFO.call_once(|| EarlyBootInfo {
        bootloader_name: parse_bootloader_name(mb1_info),
        kernel_cmdline: parse_kernel_commandline(mb1_info),
        initramfs: parse_initramfs(mb1_info),
        acpi_arg: parse_acpi_arg(mb1_info),
        framebuffer_arg: parse_framebuffer_info(mb1_info),
        memory_regions: parse_memory_regions(mb1_info),
    });

    call_ostd_main();
}
