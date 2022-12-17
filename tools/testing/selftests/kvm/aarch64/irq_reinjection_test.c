#include "test_util.h"
#include "processor.h"

#include <asm/kvm.h>
#include <asm/sysreg.h>
#include <linux/kvm.h>
#include <stdint.h>

enum test_stage {
	TEST_START = 0,
	PREPARE_SEND_IRQ,
	READY_SEND_IRQ1,
	READY_SEND_IRQ2,
	SENT_IRQ,
	SENT_INTERMEDIATE_IRQ,
	RECEIVED_IRQ,
	TEST_DONE,
};

struct test_args {
	uint64_t target_mpidr;
	atomic_ulong stage;
};

static struct test_args test_args;

struct vcpu_stats {
	uint64_t count;
	uint64_t average;
	uint64_t bins[64];
	uint64_t bin_size;
};

static void update_vcpu_stats(struct vcpu_stats *stats, uint64_t datum)
{
	uint8_t bin_idx = min(ARRAY_SIZE(stats->bins), datum / stats->bin_size);

	stats->average = ((stats->average * stats->count) + datum) / (stats->count + 1);
	stats->count++;
	stats->bins[bin_idx]++;
}

static void __spin_wait_for_stage(struct test_args *args, enum test_stage stage)
{
	uint64_t cur;

	while (true) {
		cur = atomic_load_explicit(args->stage, memory_order_acquire);
		if (cur == stage)
			return;
		if (cur == TEST_DONE)
			GUEST_DONE();

		cpu_relax();
	}
}

static void guest_spin_wait_for_stage(enum test_stage stage)
{
	__spin_wait_for_stage(&test_args, stage);
}

static void guest_set_stage(enum test_stage stage)
{
	atomic_store_explicit(&test_args.stage, stage, memory_order_release);
}

static void guest_inc_stage(void)
{
	atomic_fetch_add_explicit(&test_args.stage, 1, memory_order_acq_rel);
}

static __always_inline uint64_t read_cntvct_ordered(void)
{
	return read_sysreg_s(SYS_CNTVCTSS_EL0);
}

static void guest_reinject_code(struct vcpu_stats *stats)
{
	uint64_t start, end;

	while (true) {
		guest_spin_wait_for_stage(PREPARE_SEND_IRQ);
		start = read_cntvct_ordered();

		guest_inc_stage();
		guest_spin_wait_for_stage(SENT_INTERMEDIATE_IRQ);
		end = read_cntvct_ordered();

		update_vcpu_stats(stats, end - start);
	}
}

static void guest_receiver_code(struct vcpu_stats *stats)
{
	uint64_t start, end;

	while (true) {
		guest_spin_wait_for_stage(PREPARE_SEND_IRQ);
		start = read_cntvct_ordered();

		guest_inc_stage();
		guest_spin_wait_for_stage(RECEIVED_IRQ);
		end = read_cntvct_ordered();

		update_vcpu_stats(stats, end - start);
	}
}

static void guest_code(struct vcpu_stats *stats)
{
	uint64_t mpidr = read_sysreg(mpidr_el1);

	if (mpidr == test_args.target_mpidr)
		guest_receiver_code(stats);
	else
		guest_reinject_code(stats);
}

int main(void)
{
	struct kvm_vcpu *vcpus[2];
	struct kvm_vm *vm;
}
