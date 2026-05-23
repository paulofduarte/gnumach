/*
 * Copyright (c) 2024 Free Software Foundation.
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

#include "aarch64/model_dep.h"
#include "aarch64/locore.h"
#include "aarch64/hwcaps.h"
#include "aarch64/fpu.h"
#include "aarch64/bits/spsr.h"
#include "arm/gic-v2.h"
#include "arm/pl011.h"
#include "arm/psci.h"
#include <device/dtb.h>
#include <mach/machine.h>
#include <kern/printf.h>
#include <kern/startup.h>
#include <kern/bootstrap.h>
#include <kern/boot_script.h>
#include <mach/machine/multiboot.h>
#include <string.h>

#include <device/intr.h>	/* FIXME */

/* Some ELF definitions, for applying relocations.  */

#define R_AARCH64_NONE		0
#define R_AARCH64_RELATIVE	1027

typedef uint64_t	Elf64_Addr;
typedef uint64_t	Elf64_Xword;
typedef int64_t		Elf64_Sxword;

typedef struct
{
	Elf64_Addr	r_offset;
	Elf64_Xword	r_info;
	Elf64_Sxword	r_addend;
} Elf64_Rela;



const char *kernel_cmdline;

static void load_boot_modules_from_dtb(void);

struct irqdev irqtab;
int iunit[1];
interrupt_handler_fn ivect[1];

int spl_init;

void machine_idle(int mycpu)
{
#ifdef MACH_HYP
	hyp_idle();
#else
	// assert(cpu == cpu_number());
	asm volatile("wfi");
#endif
}

void halt_cpu(void)
{
#ifdef MACH_HYP
	hyp_halt();
#else
	/* Try PSCI.  */
	psci_cpu_off();
	/* Disable interrupts and WFI forever.  */
	asm volatile(
		"msr	DAIFSet, #15\n"
		"0:\n\t"
		"wfi\n\t"
		"b	0b"
	);
	__builtin_unreachable();
#endif
}

void halt_all_cpus(boolean_t reboot)
{
	if (reboot)
		psci_system_reset();
	else
		psci_system_off();

	/* TODO halt _all_ CPUs. */
	printf("Shutdown completed successfully, now in tight loop.\n");
	printf("You can safely power off the system or hit ctl-alt-del to reboot\n");
	halt_cpu();
}

/* FIXME */
static struct irq_ctlr *interrupt_controller;

static void zero_out_bss(void)
{
	extern char	__bss_start, __bss_end;

	memset(&__bss_start, 0, &__bss_end - &__bss_start);
}

static void apply_runtime_relocations(void)
{
	extern const Elf64_Rela	__rela_start, __rela_end;
	extern const char	__text_start;

	const Elf64_Rela	*rela;
	Elf64_Addr		*addr;
	Elf64_Addr		slide;

	/* TODO: This assumes we're linked at base address 0x0.  */
	slide = (Elf64_Addr) &__text_start;

	for (rela = &__rela_start; rela != &__rela_end; rela++) {
		switch (rela->r_info) {
			case R_AARCH64_NONE:
				/* Nothing to do.  */
				break;
			case R_AARCH64_RELATIVE:
				addr = (Elf64_Addr *)(slide + rela->r_offset);
				*addr = slide + rela->r_addend;
				break;
			default:
				panic("Unimplemented relocation type\n");
		}
	}
}

static void print_model(const char *model)
{
	const char	*c;
	boolean_t	seen_comma = FALSE;

	printf("Model name: ");
	for (c = model; *c; c++) {
		if (!seen_comma && *c == ',') {
			printf(" ");
			seen_comma = TRUE;
		} else {
			printf("%c", *c);
		}
	}
	printf("\n");
}

static void walk_dtb_visit_node(
	dtb_node_t 		node,
	dtb_ranges_map_t	map)
{
	struct dtb_node		child;
	struct dtb_ranges_map	nmap;
	boolean_t		have_nmap = FALSE;

	if (dtb_node_is_compatible(node, "arm,pl011")) {
		pl011_init(node, map);
	} else if (dtb_node_is_compatible(node, "arm,armv8-timer")) {
		cnt_init(node);
		/* FIXME */
		if (interrupt_controller)
			cnt_set_interrupt_parent(node, interrupt_controller);
	} else if (gic_v2_is_compatible(node)) {
		interrupt_controller = gic_v2_init(node, map);
	} else if (psci_is_compatible(node)) {
		psci_init(node);
	} else if (dtb_node_is_compatible(node, "simple-bus")) {
		nmap = dtb_node_make_ranges_map(node);
		nmap.next = map;
		have_nmap = TRUE;
	}

	dtb_for_each_child (*node, child) {
		walk_dtb_visit_node(&child, have_nmap ? &nmap : map);
	}
}

static void walk_dtb(void)
{
	struct dtb_node	node;
	struct dtb_prop	prop;

	node = dtb_root_node();
	/*
	 *	Look at root node's properties.
	 */
	dtb_for_each_prop (node, prop) {
		if (!strcmp(prop.name, "model")) {
			print_model(prop.data);
		}
	}

	/*
	 *	Look at top-level nodes and their props.
	 */
	dtb_for_each_child (node, node) {
		walk_dtb_visit_node(&node, NULL);
	}
}

/*
 *	Find devices.  The system is alive.
 */
void machine_init(void)
{
	fpu_init();

	/* Note that the kernel is entered with IRQ/FIQ masked.  */
	spl7_irq();
	spl_init = TRUE;

	walk_dtb();

	/* FIXME */
	assert(interrupt_controller != NULL);
	gic_v2_enable(interrupt_controller);
}

static void early_dtb_walk_visit_node(
	dtb_node_t 		node,
	dtb_ranges_map_t	map)
{
	struct dtb_node		child;
	struct dtb_ranges_map	nmap;
	boolean_t		have_nmap = FALSE;

	if (dtb_node_is_compatible(node, "arm,pl011")) {
		pl011_early_init(node, map);
	} else if (psci_is_compatible(node)) {
		psci_init(node);
	} else if (dtb_node_is_compatible(node, "simple-bus")) {
		nmap = dtb_node_make_ranges_map(node);
		nmap.next = map;
		have_nmap = TRUE;
	}

	dtb_for_each_child (*node, child) {
		early_dtb_walk_visit_node(&child, have_nmap ? &nmap : map);
	}
}

static void early_dtb_walk(void)
{
	struct dtb_node	node;
	struct dtb_prop	prop;

	node = dtb_root_node();

	/*
	 *	Look at top-level nodes and their props.
	 */
	dtb_for_each_child (node, node) {
		if (!strcmp(node.name, "chosen") || !strncmp(node.name, "chosen@", 7)) {
			prop = dtb_node_find_prop(&node, "bootargs");
			if (!DTB_IS_SENTINEL(prop))
				kernel_cmdline = (const char *) prop.data;
			/* TODO: /chosen/kaslr-seed */
			continue;
		}
		dtb_for_each_prop(node, prop) {
			if (!strcmp(prop.name, "device_type")
			    && !strcmp(prop.data, "memory"))
				pmap_discover_physical_memory(&node);
		}
		early_dtb_walk_visit_node(&node, NULL);
	}
}

static void print_el(void)
{
	long		current_el;
	unsigned short	el;

	asm("mrs %0, CurrentEL" : "=r"(current_el));
	el = SPSR_EL(current_el);

	printf("Booting in EL%d\n", el);
}

void __attribute__((noreturn)) c_boot_entry(dtb_t dtb)
{
	kern_return_t		kr;
	extern const char	version[];

	zero_out_bss();

	kr = dtb_load(dtb);
	assert(kr == KERN_SUCCESS);

	hwcaps_init();
	early_dtb_walk();
	pmap_bootstrap();
	/*
	 *	Now running with MMU from highmem, re-load things.
	 */
	asm volatile("" ::: "memory");
	apply_runtime_relocations();

	dtb = (dtb_t) phystokv(dtb);
	kr = dtb_load(dtb);
	assert(kr == KERN_SUCCESS);

	pmap_bootstrap_misc();
	load_exception_vector_table();

	/*
	 *	We should be able to use kmsg/cnputc now, even though
	 *	it doesn't yet go anywhere.
	 *	So before we do anything else, print the hello message.
	 */
	printf("%s\n", version);

	if (kernel_cmdline == NULL)
		kernel_cmdline = "";
	else
		kernel_cmdline = (const char *) phystokv(kernel_cmdline);
	printf("Kernel command line: %s\n", kernel_cmdline);

	print_el();

	machine_slot[0].is_cpu = TRUE;
	machine_slot[0].cpu_type = CPU_TYPE_ARM64;
	init_percpu(0);

	load_boot_modules_from_dtb();

	setup_main();
	__builtin_unreachable();
}

/*
 *	On x86 this is filled in by the multiboot1 loader (see
 *	i386/i386at/model_dep.c).  aarch64 doesn't get one of those, but
 *	kern/bootstrap.c is structured around walking boot_info.mods_addr,
 *	so we synthesise an equivalent table from the DTB's
 *	/chosen/multiboot,module nodes during early boot.
 */
struct multiboot_raw_info boot_info;

#define	BOOTSTRAP_MAX_MODULES	10
static struct multiboot_module boot_modules[BOOTSTRAP_MAX_MODULES];

/*
 *	Walk /chosen/multiboot,module nodes in the DTB and translate each
 *	into a multiboot_module entry that kern/bootstrap.c can consume
 *	verbatim.  QEMU's -device guest-loader synthesises exactly these
 *	nodes, so any module passed via guest-loader becomes available
 *	through the standard mods_addr/mods_count interface.
 *
 *	noinline keeps this function's locals out of c_boot_entry's frame.
 *	c_boot_entry's stack-local addresses are computed pre-MMU (sp still
 *	physical), and pmap_bootstrap() switches sp into the high virtual
 *	mapping mid-function.  An inlined version would have stack pointers
 *	hoisted into callee-saved registers before the switch, leaving us
 *	writing to physical addresses after the MMU is enabled.  A separate
 *	frame avoids that by being built with sp already virtual.
 */
static __attribute__((noinline)) void load_boot_modules_from_dtb(void)
{
	struct dtb_node		chosen, node;
	struct dtb_prop		prop;
	unsigned short		address_cells, size_cells;
	int			i = 0;
	vm_offset_t		off;

	chosen = dtb_node_by_path("/chosen");
	if (DTB_IS_SENTINEL(chosen))
		panic("No chosen node in DTB\n");

	dtb_for_each_child (chosen, node) {
		if (!dtb_node_is_compatible(&node, "multiboot,module"))
			continue;
		if (i >= BOOTSTRAP_MAX_MODULES)
			panic("Too many bootstrap modules (max %d)\n",
			      BOOTSTRAP_MAX_MODULES);

		prop = dtb_node_find_prop(&node, "bootargs");
		if (DTB_IS_SENTINEL(prop))
			panic("No bootargs for bootstrap module %d %s\n",
			      i, node.name);
		printf("module %d: %s\n", i, (const char *) prop.data);
		boot_modules[i].string = (vm_offset_t) prop.data;

		prop = dtb_node_find_prop(&node, "reg");
		assert(!DTB_IS_SENTINEL(prop));
		address_cells = node.address_cells;
		size_cells = node.size_cells;

		/*
		 *	Work around an apparent QEMU guest-loader bug,
		 *	where it unconditionally uses address/size cell
		 *	size of 2, yet doesn't set (or respect previously
		 *	set) #address-cells / #size-cells properties in
		 *	the parent node.
		 */
		if (prop.length == 16 && address_cells == 2 && size_cells == 1)
			size_cells = 2;

		off = 0;
		boot_modules[i].mod_start =
			dtb_prop_read_cells(&prop, address_cells, &off);
		boot_modules[i].mod_end = boot_modules[i].mod_start
			+ dtb_prop_read_cells(&prop, size_cells, &off);
		boot_modules[i].reserved = 0;
		i++;
	}

	if (i == 0)
		panic("No bootstrap modules loaded with Mach\n");

	boot_info.mods_count = i;
	boot_info.mods_addr = (vm_offset_t) boot_modules;
	boot_info.flags |= MULTIBOOT_MODS;
	printf("%d bootstrap modules\n", i);
}

vm_offset_t timemmap(dev_t dev, vm_offset_t off, vm_prot_t prot)
{
	extern time_value_t	*mtime;

	if (prot != VM_PROT_READ || off != 0)
		return (vm_offset_t) -1;
	return pmap_extract(kernel_pmap, (vm_offset_t) mtime);
}

vm_offset_t memmmap(dev_t dev, vm_offset_t off, vm_prot_t prot)
{
	if (!vm_page_aligned(off))
		return -1;

	return off;
}
