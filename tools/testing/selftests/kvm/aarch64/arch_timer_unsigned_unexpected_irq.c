// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch_timer_unsigned - Test to verify that the timer condition is implemented
 * as an unsigned comparison against CVAL. It checks the case where an interrupt
 * is not delivered when cval = -1LL and Counter > 0.
 *
 * Copyright (c) 2022 Google LLC
 */

#include <linux/time64.h>

#include "arch_timer.h"
#include "delay.h"
#include "gic.h"
#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "vgic.h"

#define GICD_BASE_GPA	0x80000000UL
#define GICR_BASE_GPA	0x80A00000UL

static uint64_t vtimer_irq;

static void guest_irq_handler(struct ex_regs *regs)
{
	uint64_t ctl;
	uint64_t irq;

	irq = gic_get_and_ack_irq();

	if (irq == IAR_SPURIOUS)
		return;

	GUEST_ASSERT_EQ(irq, vtimer_irq);

	ctl = read_sysreg(cntv_ctl_el0);

	GUEST_ASSERT(ctl & CTL_ISTATUS);

	/*
	 * No interrupt should be delivered for:
	 *
	 * 	cval = -1LL and Counter > 0
	 *
	 * This happens if the timer condition is implemented as a signed
	 * comparison.  EL2 receives an interrupt from the real Generic Timer,
	 * and KVM forwards it to the guest, which eventually makes it to this
	 * IRQ handler.
	 */
	GUEST_ASSERT(0);
}

static void guest_main(void)
{
	local_irq_disable();

	gic_init(GIC_V3, 1, (void *)GICD_BASE_GPA, (void *)GICR_BASE_GPA);

	write_sysreg(CTL_IMASK, cntv_ctl_el0);
	isb();

	gic_irq_enable(vtimer_irq);
	local_irq_enable();

	/*
	 * Set CVAL to a negative value. Note that Counter started as 0, so at
	 * this point it's most definitely in the positive range (it would take
	 * 126 years to become negative at 1Ghz).
	 */
	write_sysreg(-1LL, cntv_cval_el0);
	isb();

	write_sysreg(CTL_ENABLE, cntv_ctl_el0);
	isb();

	/*
	 * Assuming that hardware has implemented the timer condition as an
	 * unsigned comparison then the virtual timer IRQ should not fire
	 * in the next hundred years. Let's wait for 5 seconds.
	 */
	udelay(5000 * USEC_PER_MSEC);
	GUEST_DONE();
}

static void test_run(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	while (true) {
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		case UCALL_DONE:
			break;
		default:
			TEST_FAIL("unknown ucall: %lu", uc.cmd);
		}

		return;
	}
}

int main(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int gic_fd;

	vm = vm_create_with_one_vcpu(&vcpu, guest_main);
	ucall_init(vm, NULL);

	vcpu_device_attr_get(vcpu, KVM_ARM_VCPU_TIMER_CTRL,
			     KVM_ARM_VCPU_TIMER_IRQ_VTIMER, &vtimer_irq);
	sync_global_to_guest(vm, vtimer_irq);

	vm_init_descriptor_tables(vm);
	vcpu_init_descriptor_tables(vcpu);

	vm_install_exception_handler(vm, VECTOR_IRQ_CURRENT, guest_irq_handler);
	gic_fd = vgic_v3_setup(vm, 1, 64, GICD_BASE_GPA, GICR_BASE_GPA);
	__TEST_REQUIRE(gic_fd >= 0, "Failed to create vgic-v3");

	test_run(vcpu);

	close(gic_fd);
	kvm_vm_free(vm);
	return 0;
}
