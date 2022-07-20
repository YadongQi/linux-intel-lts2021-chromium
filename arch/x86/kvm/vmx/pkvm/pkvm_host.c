/*
 * SPDX-License-Identifier: GPL-2.0
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/trapnr.h>

#include <pkvm.h>
#include <vmx/vmx_lib.h>

MODULE_LICENSE("GPL");

struct pkvm_hyp *pkvm;

struct gdt_page pkvm_gdt_page = {
	.gdt = {
		[GDT_ENTRY_KERNEL32_CS]		= GDT_ENTRY_INIT(0xc09b, 0, 0xfffff),
		[GDT_ENTRY_KERNEL_CS]		= GDT_ENTRY_INIT(0xa09b, 0, 0xfffff),
		[GDT_ENTRY_KERNEL_DS]		= GDT_ENTRY_INIT(0xc093, 0, 0xfffff),
		[GDT_ENTRY_DEFAULT_USER32_CS]	= GDT_ENTRY_INIT(0xc0fb, 0, 0xfffff),
		[GDT_ENTRY_DEFAULT_USER_DS]	= GDT_ENTRY_INIT(0xc0f3, 0, 0xfffff),
		[GDT_ENTRY_DEFAULT_USER_CS]	= GDT_ENTRY_INIT(0xa0fb, 0, 0xfffff),
	},
};

static void *pkvm_early_alloc_contig(int pages)
{
	return alloc_pages_exact(pages << PAGE_SHIFT, GFP_KERNEL | __GFP_ZERO);
}

static void pkvm_early_free(void *ptr, int pages)
{
	free_pages_exact(ptr, pages << PAGE_SHIFT);
}

static int pkvm_host_check_and_setup_vmx_cap(struct pkvm_hyp *pkvm)
{
	struct vmcs_config *vmcs_config = &pkvm->vmcs_config;
	struct vmx_capability *vmx_cap = &pkvm->vmx_cap;
	int ret = 0;
	struct vmcs_config_setting setting = {
		.cpu_based_exec_ctrl_min =
			CPU_BASED_USE_MSR_BITMAPS |
			CPU_BASED_ACTIVATE_SECONDARY_CONTROLS,
		.cpu_based_exec_ctrl_opt = 0,
		.cpu_based_2nd_exec_ctrl_min =
			SECONDARY_EXEC_ENABLE_EPT |
			SECONDARY_EXEC_SHADOW_VMCS,
		.cpu_based_2nd_exec_ctrl_opt =
			SECONDARY_EXEC_ENABLE_INVPCID |
			SECONDARY_EXEC_XSAVES |
			SECONDARY_EXEC_ENABLE_RDTSCP |
			SECONDARY_EXEC_ENABLE_USR_WAIT_PAUSE,
		.pin_based_exec_ctrl_min = 0,
		.pin_based_exec_ctrl_opt = 0,
		.vmexit_ctrl_min =
			VM_EXIT_HOST_ADDR_SPACE_SIZE |
			VM_EXIT_LOAD_IA32_EFER |
			VM_EXIT_SAVE_IA32_PAT |
			VM_EXIT_SAVE_IA32_EFER |
			VM_EXIT_SAVE_DEBUG_CONTROLS,
		.vmexit_ctrl_opt = 0,
		.vmentry_ctrl_min =
			VM_ENTRY_LOAD_DEBUG_CONTROLS |
			VM_ENTRY_IA32E_MODE |
			VM_ENTRY_LOAD_IA32_EFER |
			VM_ENTRY_LOAD_IA32_PAT,
		.vmentry_ctrl_opt = 0,
		.has_broken_vmx_preemption_timer = false,
		.perf_global_ctrl_workaround = false,
	};

	if (!boot_cpu_has(X86_FEATURE_VMX))
		return -EINVAL;

	if (__setup_vmcs_config(vmcs_config, vmx_cap, &setting) < 0)
		return -EINVAL;

	pr_info("pin_based_exec_ctrl 0x%x\n", vmcs_config->pin_based_exec_ctrl);
	pr_info("cpu_based_exec_ctrl 0x%x\n", vmcs_config->cpu_based_exec_ctrl);
	pr_info("cpu_based_2nd_exec_ctrl 0x%x\n", vmcs_config->cpu_based_2nd_exec_ctrl);
	pr_info("vmexit_ctrl 0x%x\n", vmcs_config->vmexit_ctrl);
	pr_info("vmentry_ctrl 0x%x\n", vmcs_config->vmentry_ctrl);

	return ret;
}

static void init_gdt(struct pkvm_pcpu *pcpu)
{
	pcpu->gdt_page = pkvm_gdt_page;
}

void noop_handler(void)
{
	/* To be added */
}

static void init_idt(struct pkvm_pcpu *pcpu)
{
	gate_desc *idt = pcpu->idt_page.idt;
	struct idt_data d = {
		.segment = __KERNEL_CS,
		.bits.ist = 0,
		.bits.zero = 0,
		.bits.type = GATE_INTERRUPT,
		.bits.dpl = 0,
		.bits.p = 1,
	};
	gate_desc desc;
	int i;

	for (i = 0; i <= X86_TRAP_IRET; i++) {
		d.vector = i;
		d.bits.ist = 0;
		d.addr = (const void *)noop_handler;
		idt_init_desc(&desc, &d);
		write_idt_entry(idt, i, &desc);
	}
}

static void init_tss(struct pkvm_pcpu *pcpu)
{
	struct desc_struct *d = pcpu->gdt_page.gdt;
	tss_desc tss;

	set_tssldt_descriptor(&tss, (unsigned long)&pcpu->tss, DESC_TSS,
			__KERNEL_TSS_LIMIT);

	write_gdt_entry(d, GDT_ENTRY_TSS, &tss, DESC_TSS);
}

static int pkvm_setup_pcpu(struct pkvm_hyp *pkvm, int cpu)
{
	struct pkvm_pcpu *pcpu;

	if (cpu >= CONFIG_NR_CPUS)
		return -ENOMEM;

	pcpu = pkvm_early_alloc_contig(PKVM_PCPU_PAGES);
	if (!pcpu)
		return -ENOMEM;

	/* tmp use host cr3, switch to pkvm owned cr3 after de-privilege */
	pcpu->cr3 = __read_cr3();

	init_gdt(pcpu);
	init_idt(pcpu);
	init_tss(pcpu);

	pkvm->pcpus[cpu] = pcpu;

	return 0;
}

int __init pkvm_init(void)
{
	int ret = 0, cpu;

	pkvm = pkvm_early_alloc_contig(PKVM_PAGES);
	if (!pkvm) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = pkvm_host_check_and_setup_vmx_cap(pkvm);
	if (ret)
		goto fail1;

	for_each_possible_cpu(cpu) {
		ret = pkvm_setup_pcpu(pkvm, cpu);
		if (ret)
			goto fail1;
	}

	pkvm->num_cpus = num_possible_cpus();

	return 0;

fail1:
	pkvm_early_free(pkvm, PKVM_PAGES);
fail:
	return ret;
}
