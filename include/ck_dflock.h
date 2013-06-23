#ifndef _CK_DFLOCK_H
#define _CK_DFLOCK_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* for debugging only */
#include <stdio.h>

#include <ck_pr.h>
#include <ck_spinlock.h>

/* If we end up keeping rdtsc, then we should move it to ck_pr.h */
#include "../regressions/common.h"

#define CK_DFLOCK_BIN_COUNT 32
#define CK_DFLOCK_DEFAULT_GRANULARITY 10000 /* in microseconds */
#define CK_DFLOCK_ROUND_SIZE(L) ((L)->bin_granularity * CK_DFLOCK_BIN_COUNT)

struct ck_dflock_bin {
	struct ck_spinlock_mcs *lock;
	struct ck_spinlock_mcs *current_lock;
	unsigned int active CK_CC_CACHELINE;
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
	lock->bin_granularity = granularity;
	lock->occupied_bins = 0;
	memset(lock->bins, 0, sizeof(struct ck_dflock_bin) * CK_DFLOCK_BIN_COUNT);
}


CK_CC_INLINE static uint32_t 
ck_dflock_compute_bin(struct ck_dflock *lock, uint64_t deadline)
{
	printf("placing deadline %" PRIu64 " in bin %" PRIu64 "\n", deadline,
		((deadline % CK_DFLOCK_ROUND_SIZE(lock)) / lock->bin_granularity));
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
ck_dflock_next_bin(struct ck_dflock *lock)
{
	uint32_t start_bin, bin, u, occupied;

	occupied = ck_pr_load_uint(&lock->occupied_bins);
	if (occupied == 0) {
		return -1;
	}

	start_bin = ck_dflock_compute_bin(lock, rdtsc());
	for (u = 0 ; u < CK_DFLOCK_BIN_COUNT ; ++u) {
		bin = (start_bin + u) % CK_DFLOCK_BIN_COUNT;
		if (occupied & (1 << bin) != 0) {
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
	struct ck_spinlock_mcs *thread_lock = malloc(sizeof(struct ck_spinlock_mcs));

	bin_index = ck_dflock_compute_insert_bin(lock, deadline);
	bin = lock->bins + bin_index;

	printf("setting bit %" PRIu32 "\n", bin_index);
	/* set occupied bit for appropriate bin */
	do {
		set_bins = ck_pr_load_uint(&lock->occupied_bins);
		bin_update = set_bins | (1 << bin_index);
	} while (ck_pr_cas_uint(&lock->occupied_bins, set_bins, bin_update) == false);
	printf("old bitmask: %" PRIu32 "\n", set_bins);
	printf("new bitmask: %" PRIu32 "\n", bin_update);

	/* acquire local lock */
	printf("acquiring local lock %u...", bin_index);
	ck_spinlock_mcs_lock(&bin->lock, thread_lock);
	printf("done\n");
	ck_pr_store_ptr(&bin->current_lock, thread_lock);

	/* if another thread previously held the global lock... */
	if (set_bins != 0) {
		/* ... then wait for bin to be activated */
		printf("waiting for active bit %" PRIu32 "...", bin_index);
		while (ck_pr_load_uint(&bin->active) == 0) {
			ck_pr_stall();
		}
		printf("done\n");
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

	bin = lock->bins + lock->last_used_bin;

	/* 
	 * Unset active bit before releasing local lock because we're not sure yet whether
	 * we'll be returning control to this bin.  This can be optimized later to first
	 * check earlier bins for waiting threads and possibly leave this bit set.
	 */
	ck_pr_store_uint(&bin->active, 0);
	ck_pr_fence_store();
	ck_spinlock_mcs_unlock(&bin->lock, ck_pr_load_ptr(&bin->current_lock));
	free(bin->current_lock);
	ck_pr_store_ptr(&bin->current_lock, NULL);

	/* if no other thread is waiting on the local lock, we can mark this bin as unused */
	if (ck_spinlock_mcs_locked(&bin->lock) == false) {
		/*do {
			set_bins = ck_pr_load_32(&lock->occupied_bins);
			bin_update = set_bins & ~(1 << lock->last_used_bin);
		} while (ck_pr_cas_32(&lock->occupied_bins, set_bins, bin_update) == false);*/
		ck_pr_and_uint(&lock->occupied_bins, ~(1 << lock->last_used_bin));
	}

	/* signal the next bin if applicable */
	next_bin = ck_dflock_next_bin(lock);
	if (next_bin >= 0) {
		ck_pr_store_uint(&lock->bins[next_bin].active, 1);
	}

	return;
}

#endif /* _CK_DFLOCK_H */
