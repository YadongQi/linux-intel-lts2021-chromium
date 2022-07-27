/*
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/types.h>
#include <asm/kvm_pkvm.h>

unsigned long __page_base_offset;
unsigned long __symbol_base_offset;

unsigned int pkvm_memblock_nr;
struct memblock_region pkvm_memory[PKVM_MEMBLOCK_REGIONS];

void *pkvm_phys_to_virt(unsigned long phys)
{
	return (void *)__page_base_offset + phys;
}

unsigned long pkvm_virt_to_phys(void *virt)
{
	return (unsigned long)virt - __page_base_offset;
}

unsigned long pkvm_virt_to_symbol_phys(void *virt)
{
	return (unsigned long)virt - __symbol_base_offset;
}
