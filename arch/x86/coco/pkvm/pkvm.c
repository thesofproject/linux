// SPDX-License-Identifier: GPL-2.0

#include <linux/kvm_para.h>
#include <asm/coco.h>
#include <asm/pkvm_guest.h>

DEFINE_STATIC_KEY_FALSE(pkvm_guest_detected);
EXPORT_SYMBOL(pkvm_guest_detected);

int pkvm_set_mem_host_visibility(unsigned long addr, int numpages, bool enc)
{
	unsigned long size = numpages * PAGE_SIZE;
	int ret;

	if (!enc) {
		int i;

		/*
		 * If the guest has never touched these pages before, they have
		 * not been donated to the guest yet, i.e. are still owned by
		 * the host. In such case the sharing will fail.
		 *
		 * So touch these pages first, to make sure they are owned by
		 * the guest before sharing.
		 */
		for (i = 0; i < numpages; i++)
			READ_ONCE(*(u8 *)(addr + i * PAGE_SIZE));

		/*
		 * pKVM may not have enough memory to perform the sharing. In such
		 * case it requests the host to provide more memory, and then may
		 * ask the guest to retry the hypercall.
		 */
		do {
			ret = kvm_hypercall2(PKVM_GHC_SHARE_MEM, __pa(addr), size);
		} while (ret == -EAGAIN);
	} else {
		ret = kvm_hypercall2(PKVM_GHC_UNSHARE_MEM, __pa(addr), size);
	}

	return ret;
}

__init void pkvm_guest_init_coco(void)
{
	cc_vendor = CC_VENDOR_PKVM;

	static_branch_enable(&pkvm_guest_detected);
}
