#ifndef _CK_DFLOCK_H
#define _CK_DFLOCK_H

#include <inttypes.h>

#include <ck_pr.h>
#include <ck_spinlock.h>

/*
 * If we end up keeping rdtsc, then we should move it to ck_pr.h.
 * It would probably be better to let the user define what they want their
 * deadline values to represent; they could pass to the initialization
 * method a pointer to a function that would return the current value for
 * their chosen deadline scheme.
 */
#include "../regressions/common.h"

/* In the future we can use macros to use 64 bins on 64-bit architectures */
#define CK_DFLOCK_BIN_COUNT 32
#define CK_DFLOCK_DEFAULT_GRANULARITY 10000
#define CK_DFLOCK_ROUND_SIZE(L) ((L)->bin_granularity * CK_DFLOCK_BIN_COUNT)

struct ck_dflock_bin {
	struct ck_spinlock_fas lock;
	unsigned int active CK_CC_CACHELINE;
	unsigned int contention_count CK_CC_CACHELINE;
};

struct ck_dflock {
	uint32_t occupied_bins CK_CC_CACHELINE;
	uint32_t bin_granularity;

	/*
	 * Used by unlock method to determine which local lock to unlock.
	 * For reader-writer locks we'll need to return some token from the lock
	 * method that can be passed back to the unlock method.
	 */
	uint32_t last_used_bin; 

	struct ck_dflock_bin bins[CK_DFLOCK_BIN_COUNT];
};


CK_CC_INLINE static void
ck_dflock_init(struct ck_dflock *lock, uint32_t granularity)
{
	unsigned int u;
	struct ck_dflock_bin *bin;

	lock->bin_granularity = granularity;
	lock->occupied_bins = 0;

	for (u = 0 ; u < CK_DFLOCK_BIN_COUNT ; ++u) {
		bin = lock->bins + u;
		ck_spinlock_fas_init(&bin->lock);
		bin->active = 0;
		bin->contention_count = 0;
	}

	ck_pr_fence_memory();
	return;
}


CK_CC_INLINE static uint32_t 
ck_dflock_compute_bin(struct ck_dflock *lock, uint64_t deadline)
{
	return (deadline % CK_DFLOCK_ROUND_SIZE(lock)) / lock->bin_granularity;
}


CK_CC_INLINE static uint32_t
ck_dflock_compute_insert_bin(struct ck_dflock *lock, uint64_t deadline)
{
	uint64_t now = rdtsc();

	if (now >= deadline) {
		/* If the deadline has already passed then we should put it in the most immediate bin */
		return ck_dflock_compute_bin(lock, now);
	} else if ((deadline - now) > CK_DFLOCK_ROUND_SIZE(lock)) {
		/* If deadline is more than a full round in the future, put it in the furthest bin */
		return (ck_dflock_compute_bin(lock, now) - 1) % CK_DFLOCK_BIN_COUNT;
	} else {
		return ck_dflock_compute_bin(lock, deadline);
	}
}


/* Returns -1 if no bin should be activated */
CK_CC_INLINE static int
ck_dflock_next_bin(struct ck_dflock *lock, uint32_t occupied)
{
	uint32_t start_bin, bin, u;

	if (occupied == 0) {
		return -1;
	}

	start_bin = ck_dflock_compute_bin(lock, rdtsc());
	for (u = 0 ; u < CK_DFLOCK_BIN_COUNT ; ++u) {
		bin = (start_bin + u) % CK_DFLOCK_BIN_COUNT;
		if ((occupied & (1 << bin)) != 0) {
			return bin;
		}
	}

	/* this can't actually be hit but is necessary to avoid compiler warnings */
	return -1;
}


CK_CC_INLINE static void 
ck_dflock_lock(struct ck_dflock *lock, uint64_t deadline)
{
	uint32_t bin_index, set_bins, bin_update;
	struct ck_dflock_bin *bin;

	bin_index = ck_dflock_compute_insert_bin(lock, deadline);
	bin = lock->bins + bin_index;

	ck_pr_inc_uint(&bin->contention_count);
	ck_spinlock_fas_lock(&bin->lock);
	ck_pr_dec_uint(&bin->contention_count);

	/* set occupied bit for appropriate bin */
	do {
		set_bins = ck_pr_load_uint(&lock->occupied_bins);
		bin_update = set_bins | (1 << bin_index);
	} while (ck_pr_cas_uint(&lock->occupied_bins, set_bins, bin_update) == false);
	ck_pr_fence_memory();

	/* if another thread previously held the global lock... */
	if (set_bins != 0) {
		/* ... then wait for bin to be activated */
		while (ck_pr_load_uint(&bin->active) == 0) {
			ck_pr_stall();
		}
	} else {
		/* 
		 * Otherwise set the active bit for this bin in case we keep
		 * control here upon unlocking.
		 */
		ck_pr_store_uint(&bin->active, 1);
	}

	/* 
	 * now that we have the overall lock, make a note of which bin acquired it
	 * so that we know what to deactivate upon unlocking
	 */
	ck_pr_store_uint(&lock->last_used_bin, bin_index);
	ck_pr_fence_store();

	return;
}


CK_CC_INLINE static void
ck_dflock_unlock(struct ck_dflock *lock)
{
	struct ck_dflock_bin *bin;
	int next_bin;
	uint32_t set_bins, bin_update;

	bin = lock->bins + lock->last_used_bin;

	/* if no other thread is waiting on the local lock, we can mark this bin as unused */
	if (ck_pr_load_uint(&bin->contention_count) == 0) {
		do {
			set_bins = ck_pr_load_uint(&lock->occupied_bins);
			bin_update = set_bins & ~(1 << lock->last_used_bin);
		} while (ck_pr_cas_uint(&lock->occupied_bins, set_bins, bin_update) == false);
	} else {
		bin_update = ck_pr_load_uint(&lock->occupied_bins);
	}

	next_bin = ck_dflock_next_bin(lock, bin_update);
	if (next_bin != (int)(lock->last_used_bin)) {
		ck_pr_store_uint(&bin->active, 0);
		if (next_bin >= 0) {
			ck_pr_store_uint(&lock->bins[next_bin].active, 1);
		}
	}

	ck_spinlock_fas_unlock(&bin->lock);
	ck_pr_fence_memory();

	return;
}

#endif /* _CK_DFLOCK_H */
