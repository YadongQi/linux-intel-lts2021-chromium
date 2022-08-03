/*
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef __ASM_x86_PKVM_IMAGE_VARS_H
#define __ASM_x86_PKVM_IMAGE_VARS_H

#ifndef CONFIG_PKVM_INTEL_DEBUG
PKVM_ALIAS(physical_mask);
PKVM_ALIAS(sme_me_mask);
PKVM_ALIAS(__default_kernel_pte_mask);
PKVM_ALIAS(vmcs12_field_offsets);
PKVM_ALIAS(nr_vmcs12_fields);
#endif

#endif
