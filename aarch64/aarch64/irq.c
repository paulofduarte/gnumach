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

#include "aarch64/irq.h"

struct irq_src	*root_irq_src;

/*
 *	device/intr.c exposes IRQGETPICMODE through irqgetstat(), and reads
 *	pic_mode to report which interrupt-controller mode the kernel is
 *	using.  On aarch64 there is no PIC/APIC distinction; the GIC is the
 *	only option, so report a fixed value here.
 */
int pic_mode = 0;
