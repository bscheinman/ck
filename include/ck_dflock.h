#ifndef _CK_DFLOCK_H
#define _CK_DFLOCK_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* for debugging only */
#include <stdio.h>
#include <sys/types.h>

#include <ck_pr.h>
#include <ck_spinlock.h>

/* If we end up keeping rdtsc, then we should move it to ck_pr.h */
#include "../regressions/common.h"

#define CK_DFLOCK_BIN_COUNT 32
#define CK_DFLOCK_DEFAULT_GRANULARITY 10000 /* in microseconds */
#define CK_DFLOCK_ROUND_SIZE(L) ((L)->bin_granularity * CK_DFLOCK_BIN_COUNT)


/* for debugging only */
/*static char *
to_binary(uint32_t x)
{
	char *s = malloc(sizeof(char) * 33);
	for (unsigned int u = 0 ; u < 32 ; ++u) {
		s[u] = (x & (1 << (31 - u))) ? '1' : '0';
	}
	return s;
}*/

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

	//printf("%u: setting bit %" PRIu32 "\n", gettid(), bin_index);
	//printf("%u,%" PRIu32 "\n", gettid(), bin_index);
	/* set occupied bit for appropriate bin */
	do {
		set_bins = ck_pr_load_uint(&lock->occupied_bins);
		bin_update = set_bins | (1 << bin_index);
	} while (ck_pr_cas_uint(&lock->occupied_bins, set_bins, bin_update) == false);
	/*char *b = to_binary(set_bins);
	printf("old bitmask: %s\n", b);
	free(b);
	b = to_binary(bin_update);
	printf("new bitmask: %s\n", b);
	free(b);*/

	/* if another thread previously held the global lock... */
	if (set_bins != 0) {
		/* ... then wait for bin to be activated */
		//printf("waiting for active bit %" PRIu32 "...", bin_index);
		while (ck_pr_load_uint(&bin->active) == 0) {
			ck_pr_stall();
		}
		//printf("done\n");
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

	ck_pr_store_uint(&bin->active, 0);
	ck_pr_fence_store();

	/* if no other thread is waiting on the local lock, we can mark this bin as unused */
	if (ck_pr_load_uint(&bin->contention_count) == 0) {
		ck_pr_and_uint(&lock->occupied_bins, ~(1 << lock->last_used_bin));
	}

	ck_spinlock_fas_unlock(&bin->lock);

	/* signal the next bin if applicable */
	next_bin = ck_dflock_next_bin(lock);
	if (next_bin >= 0) {
		//printf("awarding lock to bin %i\n", next_bin);
		ck_pr_store_uint(&lock->bins[next_bin].active, 1);
	} else {
		//printf("no next bin waiting\n");
	}

	ck_pr_fence_memory();
	return;
}

#endif /* _CK_DFLOCK_H */
