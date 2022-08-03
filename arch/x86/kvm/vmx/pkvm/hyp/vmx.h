/*
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef __PKVM_VMX_H
#define __PKVM_VMX_H

static inline u64 pkvm_construct_eptp(unsigned long root_hpa,
		struct vmx_capability *vmx_cap)
{
	u64 eptp = 0;

	if (vmx_cap->ept & VMX_EPT_PAGE_WALK_4_BIT)
		eptp = VMX_EPTP_PWL_4;
	else if (vmx_cap->ept & VMX_EPT_PAGE_WALK_5_BIT)
		eptp = VMX_EPTP_PWL_5;

	if (vmx_cap->ept & VMX_EPTP_WB_BIT)
		eptp |= VMX_EPTP_MT_WB;

	eptp |= (root_hpa & PAGE_MASK);

	return eptp;
}

#endif
