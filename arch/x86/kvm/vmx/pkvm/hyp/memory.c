/*
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/types.h>
#include <asm/kvm_pkvm.h>

#include <pkvm.h>
#include "memory.h"
#include "pgtable.h"
#include "pkvm_hyp.h"
#include "cpu.h"

unsigned long __page_base_offset;
unsigned long __symbol_base_offset;
unsigned long __x86_clflush_size;
static u8 max_physaddr_bits;

unsigned int pkvm_memblock_nr;
struct memblock_region pkvm_memory[PKVM_MEMBLOCK_REGIONS];

void *pkvm_iophys_to_virt(unsigned long phys)
{
	unsigned long iova = PKVM_IOVA_OFFSET + phys;

	if (iova >= __page_base_offset)
		return (void *)INVALID_ADDR;

	return (void *)iova;
}

void *pkvm_phys_to_virt(unsigned long phys)
{
	return (void *)__page_base_offset + phys;
}

unsigned long pkvm_virt_to_phys(void *virt)
{
	/* this api only take care direct & io mapping */
	if ((unsigned long)virt < PKVM_IOVA_OFFSET)
		return INVALID_ADDR;

	return ((unsigned long)virt >= __page_base_offset) ?
		(unsigned long)virt - __page_base_offset :
		(unsigned long)virt - PKVM_IOVA_OFFSET;
}

unsigned long pkvm_virt_to_symbol_phys(void *virt)
{
	return (unsigned long)virt - __symbol_base_offset;
}

void *host_gpa2hva(unsigned long gpa)
{
	/* host gpa = hpa */
	return pkvm_phys_to_virt(gpa);
}

extern struct pkvm_pgtable_ops mmu_ops;
static struct pkvm_mm_ops mm_ops = {
	.phys_to_virt = host_gpa2hva,
};

static int check_translation(struct kvm_vcpu *vcpu, gpa_t gpa,
		u64 prot, u32 access, struct x86_exception *exception)
{
	/* TODO: exception for #PF */
	return 0;
}

int gva2gpa(struct kvm_vcpu *vcpu, gva_t gva, gpa_t *gpa,
		u32 access, struct x86_exception *exception)
{
	struct pkvm_pgtable guest_mmu;
	gpa_t _gpa;
	u64 prot;
	int pg_level;

	/*TODO: support other paging mode beside long mode */
	guest_mmu.root_pa = vcpu->arch.cr3 & PAGE_MASK;
	pkvm_pgtable_init(&guest_mmu, &mm_ops, &mmu_ops, &pkvm_hyp->mmu_cap, false);
	pkvm_pgtable_lookup(&guest_mmu, (unsigned long)gva,
			(unsigned long *)&_gpa, &prot, &pg_level);
	*gpa = _gpa;
	if (_gpa == INVALID_ADDR)
		return -EFAULT;

	return check_translation(vcpu, _gpa, prot, access, exception);
}

/* only support host VM now */
static int copy_gva(struct kvm_vcpu *vcpu, gva_t gva, void *addr,
		unsigned int bytes, struct x86_exception *exception, bool from_guest)
{
	u32 access = VMX_AR_DPL(vmcs_read32(GUEST_SS_AR_BYTES)) == 3 ? PFERR_USER_MASK : 0;
	gpa_t gpa;
	void *hva;
	int ret;

	memset(exception, 0, sizeof(*exception));

	/*FIXME: need check the gva per page granularity */
	ret = gva2gpa(vcpu, gva, &gpa, access, exception);
	if (ret)
		return ret;

	hva = host_gpa2hva(gpa);
	if (from_guest)
		memcpy(addr, hva, bytes);
	else
		memcpy(hva, addr, bytes);

	return bytes;
}

int read_gva(struct kvm_vcpu *vcpu, gva_t gva, void *addr,
		unsigned int bytes, struct x86_exception *exception)
{
	return copy_gva(vcpu, gva, addr, bytes, exception, true);
}

int write_gva(struct kvm_vcpu *vcpu, gva_t gva, void *addr,
		unsigned int bytes, struct x86_exception *exception)
{
	return copy_gva(vcpu, gva, addr, bytes, exception, false);
}

/* only support host VM now */
static int copy_gpa(struct kvm_vcpu *vcpu, gpa_t gpa, void *addr,
		unsigned int bytes, bool from_guest)
{
	void *hva;

	hva = host_gpa2hva(gpa);
	if (from_guest)
		memcpy(addr, hva, bytes);
	else
		memcpy(hva, addr, bytes);

	return bytes;
}

int read_gpa(struct kvm_vcpu *vcpu, gpa_t gpa, void *addr, unsigned int bytes)
{
	return copy_gpa(vcpu, gpa, addr, bytes, true);
}

int write_gpa(struct kvm_vcpu *vcpu, gpa_t gpa, void *addr, unsigned int bytes)
{
	return copy_gpa(vcpu, gpa, addr, bytes, false);
}

bool find_mem_range(unsigned long addr, struct mem_range *range)
{
	int cur, left = 0, right = pkvm_memblock_nr;
	struct memblock_region *reg;
	unsigned long end;

	range->start = 0;
	range->end = ULONG_MAX;

	/* The list of memblock regions is sorted, binary search it */
	while (left < right) {
		cur = (left + right) >> 1;
		reg = &pkvm_memory[cur];
		end = reg->base + reg->size;
		if (addr < reg->base) {
			right = cur;
			range->end = reg->base;
		} else if (addr >= end) {
			left = cur + 1;
			range->start = end;
		} else {
			range->start = reg->base;
			range->end = end;
			return true;
		}
	}

	return false;
}

bool mem_range_included(struct mem_range *child, struct mem_range *parent)
{
	return parent->start <= child->start && child->end <= parent->end;
}

static void pkvm_clflush_cache_range_opt(void *vaddr, unsigned int size)
{
	const unsigned long clflush_size = __x86_clflush_size;
	void *p = (void *)((unsigned long)vaddr & ~(clflush_size - 1));
	void *vend = vaddr + size;

	if (p >= vend)
		return;

	for (; p < vend; p += clflush_size)
		clflushopt(p);
}

/**
 * pkvm_clflush_cache_range - flush a cache range with clflush
 * which is implemented refer to clflush_cache_range() in kernel.
 *
 * @vaddr:	virtual start address
 * @size:	number of bytes to flush
 */
void pkvm_clflush_cache_range(void *vaddr, unsigned int size)
{
	/*
	 * clflush is an unordered instruction which needs fencing
	 * with MFENCE or SFENCE to avoid ordering issue. Put a mb()
	 * before the clflush.
	 */
	mb();
	pkvm_clflush_cache_range_opt(vaddr, size);
	/* And also put another one after. */
	mb();
}

u64 get_max_physaddr_bits(void)
{
	u32 eax, ebx, ecx, edx;

	if (max_physaddr_bits)
		return max_physaddr_bits;

	eax = 0x80000000;
	ecx = 0;
	native_cpuid(&eax, &ebx, &ecx, &edx);
	if (eax >= 0x80000008) {
		eax = 0x80000008;
		native_cpuid(&eax, &ebx, &ecx, &edx);
		max_physaddr_bits = (u8)eax & 0xff;
	}

	return max_physaddr_bits;
}
