/*
 * Copyright (c) 2026 Free Software Foundation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _MACH_AARCH64_MULTIBOOT_H_
#define _MACH_AARCH64_MULTIBOOT_H_

/*
 *	aarch64 boots via the Linux arm64 boot protocol, not multiboot,
 *	but kern/bootstrap.c is structured around multiboot's view of
 *	"the kernel was handed a list of boot modules".  The aarch64
 *	boot path synthesises a multiboot-shaped module list from the
 *	DTB's /chosen/multiboot,module nodes during c_boot_entry, so
 *	the same bootstrap_create() works unchanged.
 *
 *	This header therefore exposes only the subset of the multiboot1
 *	layout that bootstrap.c actually touches: struct multiboot_module,
 *	struct multiboot_raw_info, and the MULTIBOOT_MODS flag.  Fields
 *	are sized as vm_offset_t (64-bit on aarch64) rather than the 32-bit
 *	fields of the on-the-wire protocol, since this struct is populated
 *	in-kernel and never crosses the kernel/loader boundary.
 */

#define MULTIBOOT_MODS		0x00000008

#ifndef __ASSEMBLER__

#include <mach/machine/vm_types.h>

struct multiboot_module
{
	vm_offset_t	mod_start;
	vm_offset_t	mod_end;
	vm_offset_t	string;
	unsigned	reserved;
};

struct multiboot_raw_info
{
	uint32_t	flags;
	uint32_t	mods_count;
	vm_offset_t	mods_addr;
};

#endif /* __ASSEMBLER__ */

#endif /* _MACH_AARCH64_MULTIBOOT_H_ */
