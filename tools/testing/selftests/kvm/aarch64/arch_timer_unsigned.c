// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch_timer_unsigned - Test to verify that the timer condition is implemented
 * as an unsigned comparison against CVAL.
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
	uint64_t cval, cnt, ctl;
	uint64_t irq;

	irq = gic_get_and_ack_irq();

	if (irq == IAR_SPURIOUS)
		return;

	GUEST_ASSERT_EQ(irq, vtimer_irq);

	cval = read_sysreg(cntv_cval_el0);
	cnt = read_sysreg(cntvct_el0);
	ctl = read_sysreg(cntv_ctl_el0);

	/* Has the timer condition been met? */
	GUEST_ASSERT(cnt >= cval);

	/*
	 * Does the virtual timer control register indictate that the timer
	 * condition has been met? Note that KVM does *not* trap guest reads of
	 * the virtual timer registers, so this value should come from hardware.
	 */
	GUEST_ASSERT(ctl & CTL_ISTATUS);

	GUEST_DONE();
}

static void guest_main(void)
{
	GUEST_SYNC(read_sysreg(cntvct_el0));

	local_irq_disable();

	gic_init(GIC_V3, 1, (void *)GICD_BASE_GPA, (void *)GICR_BASE_GPA);

	write_sysreg(CTL_IMASK, cntv_ctl_el0);
	isb();

	gic_irq_enable(vtimer_irq);
	local_irq_enable();

	/* Set CVAL to a value far in the past */
	write_sysreg(0, cntv_cval_el0);
	isb();

	write_sysreg(CTL_ENABLE, cntv_ctl_el0);
	isb();

	/*
	 * Assuming that hardware has implemented the timer condition as an
	 * unsigned comparison then we virtual timer IRQ should fire
	 * immediately. Otherwise, if hardware has implemented the timer
	 * condition as a *signed* comparison, one of two things could happen:
	 *
	 * 1) The vCPU is never scheduled out. The hardware interrupt will never
	 * fire and the guest will fail on the below assertion.
	 *
	 * 2) The vCPU is scheduled out for some time then scheduled back in.
	 * KVM detects that the timer condition has been met (unsigned) and
	 * synthesizes an interrupt into the guest. The guest will test
	 * CNTV_CTL_EL0.ISTATUS == 0b1 (read from hardware), which is expected
	 * to fail.
	 */
	udelay(1000 * USEC_PER_MSEC);
	GUEST_ASSERT(0);
}

static void test_run(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	while (true) {
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			__TEST_REQUIRE(uc.args[1] & BIT_ULL(63), "Requires a 64 bit counter");
			continue;
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

	/*
	 * Set the virtual counter to a 'negative' value by offsetting it. Keep
	 * in mind CNTVCT_EL0 is an unsigned quantity.
	 */
	vcpu_set_reg(vcpu, KVM_REG_ARM_TIMER_CNT, BIT_ULL(63));

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
