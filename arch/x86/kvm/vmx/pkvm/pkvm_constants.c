/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kbuild.h>
#include <pkvm.h>
#include <buddy_memory.h>

int main(void)
{
	DEFINE(PKVM_PERCPU_PAGES, PKVM_PCPU_PAGES + PKVM_HOST_VCPU_PAGES + PKVM_VMCS_PAGES);
	DEFINE(PKVM_GLOBAL_PAGES, PKVM_PAGES);
	DEFINE(PKVM_VMEMMAP_ENTRY_SIZE, sizeof(struct pkvm_page));
	return 0;
}
