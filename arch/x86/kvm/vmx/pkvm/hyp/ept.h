/*
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef __PKVM_EPT_H
#define __PKVM_EPT_H

int pkvm_host_ept_map(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size, int pgsz_mask, u64 prot);
int pkvm_host_ept_unmap(unsigned long vaddr_start, unsigned long phys_start,
		unsigned long size);
void pkvm_host_ept_destroy(void);
int pkvm_host_ept_init(struct pkvm_pgtable_cap *cap, void *ept_pool_base,
		unsigned long ept_pool_pages);

#endif
