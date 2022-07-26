#include <linux/bitfield.h>
#include <pkvm.h>
#include <gfp.h>
#include "pkvm_hyp.h"
#include "mem_protect.h"
#include "pgtable.h"
#include "ept.h"

struct check_walk_data {
	enum pkvm_page_state	desired;
};

enum pkvm_component_id {
	PKVM_ID_HOST,
	PKVM_ID_HYP,
};

struct pkvm_mem_trans_desc {
	enum pkvm_component_id	id;
	union {
		struct {
			u64	addr;
		} host;

		struct {
			u64	addr;
		} hyp;
	};
};

struct pkvm_mem_transition {
	u64				size;
	struct pkvm_mem_trans_desc	initiator;
	struct pkvm_mem_trans_desc	completer;
};

static u64 pkvm_init_invalid_leaf_owner(pkvm_id owner_id)
{
	/* the page owned by others also means NOPAGE in page state */
	return FIELD_PREP(PKVM_INVALID_PTE_OWNER_MASK, owner_id) |
		FIELD_PREP(PKVM_PAGE_STATE_PROT_MASK, PKVM_NOPAGE);
}

int host_ept_set_owner_locked(phys_addr_t addr, u64 size, pkvm_id owner_id)
{
	u64 annotation = pkvm_init_invalid_leaf_owner(owner_id);
	int ret;


	/*
	 * The memory [addr, addr + size) will be unmaped from host ept. At the
	 * same time, the annotation with a NOPAGE flag will be put in the
	 * invalid pte that has been unmaped. And these information record that
	 * the page has been used by some guest and it's id can be read from
	 * annotation. Also when later these page back to host, the annotation
	 * will help to check the right page transition.
	 */
	ret = pkvm_pgtable_annotate(pkvm_hyp->host_vm.ept, addr, size, annotation);

	return ret;
}

int host_ept_set_owner(phys_addr_t addr, u64 size, pkvm_id owner_id)
{
	int ret;

	host_ept_lock();
	ret = host_ept_set_owner_locked(addr, size, owner_id);
	host_ept_unlock();

	return ret;
}

static int
__check_page_state_walker(struct pkvm_pgtable *pgt, unsigned long vaddr,
		       unsigned long vaddr_end, int level, void *ptep,
		       unsigned long flags, void *const arg)
{
	struct check_walk_data *data = arg;

	return pkvm_getstate(*(u64 *)ptep) == data->desired ? 0 : -EPERM;
}

static int check_page_state_range(struct pkvm_pgtable *pgt, u64 addr, u64 size,
				  enum pkvm_page_state state)
{
	struct check_walk_data data = {
		.desired		= state,
	};
	struct pkvm_pgtable_walker walker = {
		.cb		= __check_page_state_walker,
		.flags		= PKVM_PGTABLE_WALK_LEAF,
		.arg		= &data,
	};

	return pgtable_walk(pgt, addr, size, true, &walker);
}

static int __host_check_page_state_range(u64 addr, u64 size,
					 enum pkvm_page_state state)
{
	return check_page_state_range(pkvm_hyp->host_vm.ept, addr, size, state);
}

static int host_request_donation(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.host.addr;
	u64 size = tx->size;

	return __host_check_page_state_range(addr, size, PKVM_PAGE_OWNED);
}

static int check_donation(const struct pkvm_mem_transition *tx)
{
	int ret;

	switch (tx->initiator.id) {
	case PKVM_ID_HOST:
		ret = host_request_donation(tx);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	switch (tx->completer.id) {
	case PKVM_ID_HYP:
		ret = 0;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int host_initiate_donation(const struct pkvm_mem_transition *tx)
{
	u64 addr = tx->initiator.host.addr;
	u64 size = tx->size;

	return host_ept_set_owner_locked(addr, size, pkvm_hyp_id);
}

static int __do_donate(const struct pkvm_mem_transition *tx)
{
	int ret;

	switch (tx->initiator.id) {
	case PKVM_ID_HOST:
		ret = host_initiate_donation(tx);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		return ret;

	switch (tx->completer.id) {
	case PKVM_ID_HYP:
		ret = 0;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

/*
 * do_donate - the page owner transfer ownership to another component.
 *
 * Initiator: OWNED	=> NO_PAGE
 * Completer: NO_APGE	=> OWNED
 *
 * The special component is pkvm_hyp, due to pkvm_hyp can access all of the
 * memory, so there need to do nothing if the page owner transfer to hyp or
 * hyp transfer to other entity.
 */
static int do_donate(const struct pkvm_mem_transition *donation)
{
	int ret;

	ret = check_donation(donation);
	if (ret)
		return ret;

	return WARN_ON(__do_donate(donation));
}

int __pkvm_host_donate_hyp(u64 hpa, u64 size)
{
	int ret;
	u64 hyp_addr = (u64)__pkvm_va(hpa);
	struct pkvm_mem_transition donation = {
		.size		= size,
		.initiator	= {
			.id	= PKVM_ID_HOST,
			.host	= {
				.addr	= hpa,
			},
		},
		.completer	= {
			.id	= PKVM_ID_HYP,
			.hyp	= {
				.addr = hyp_addr,
			},
		},
	};

	host_ept_lock();

	ret = do_donate(&donation);

	host_ept_unlock();

	return ret;
}
