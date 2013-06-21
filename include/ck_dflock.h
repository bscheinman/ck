#ifndef _CK_DFLOCK_H
#define _CK_DFLOCK_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <ck_pr.h>
#include <ck_spinlock.h>

#define CK_DFLOCK_BIN_COUNT 32
#define CK_DFLOCK_DEFAULT_GRANULARITY 10000 /* in microseconds */

struct ck_dflock_bin {
	struct ck_spinlock_mcs *lock CK_CC_CACHELINE;
	struct ck_spinlock_mcs *current_lock;
	unsigned int active CK_CC_CACHELINE;
};


struct ck_dflock {
	uint32_t occupied_bins CK_CC_CACHELINE;
	uint32_t bin_granularity;
	uint32_t last_used_bin; /* used by unlock method to determine which local lock to unlock */
	struct ck_dflock_bin bins[CK_DFLOCK_BIN_COUNT];
};


CK_CC_INLINE static void
ck_dflock_init(struct ck_dflock *lock, uint32_t granularity)
{
	lock->bin_granularity = granularity;
	lock->occupied_bins = 0;
	memset(lock->bins, 0, sizeof(struct ck_dflock_bin) * CK_DFLOCK_BIN_COUNT);
}


CK_CC_INLINE static uint64_t
ck_dflock_timeval_to_ms(struct timeval *tv)
{
	uint64_t ms = 1000000 * (uint64_t)(tv->tv_sec);
	ms += tv->tv_usec;
	return ms;
}


CK_CC_INLINE static uint32_t 
ck_dflock_compute_bin(struct ck_dflock *lock, struct timeval *deadline)
{
	uint32_t round_size = CK_DFLOCK_BIN_COUNT * lock->bin_granularity;
	uint64_t ms = ck_dflock_timeval_to_ms(deadline);
	return (ms % round_size) / CK_DFLOCK_BIN_COUNT;
}


CK_CC_INLINE static uint32_t
ck_dflock_compute_insert_bin(struct ck_dflock *lock, struct timeval *deadline)
{
	struct timeval now;
	uint64_t deadline_ms, now_ms, round_size;
	int64_t future_ms;

	deadline_ms = ck_dflock_timeval_to_ms(deadline);
	gettimeofday(&now, NULL);
	now_ms = ck_dflock_timeval_to_ms(&now);

	future_ms = (int64_t)deadline_ms - (int64_t)now_ms;
	round_size = lock->bin_granularity * CK_DFLOCK_BIN_COUNT;
	if ((int64_t)future_ms < 0) {
		return ck_dflock_compute_bin(lock, &now);
	} else if (future_ms > (int64_t)round_size) {
		return (ck_dflock_compute_bin(lock, &now) - 1) % CK_DFLOCK_BIN_COUNT;
	} else {
		return ck_dflock_compute_bin(lock, deadline);
	}
}


/* Returns -1 if no bin should be activated */
CK_CC_INLINE static int
ck_dflock_next_bin(struct ck_dflock *lock, uint32_t occupied)
{
	uint32_t start_bin, bin, u;
	struct timeval now;

	if (occupied == 0) {
		return -1;
	}

	gettimeofday(&now, NULL);
	start_bin = ck_dflock_compute_bin(lock, &now);
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
ck_dflock_lock(struct ck_dflock *lock, struct timeval *deadline)
{
	uint32_t bin_index, set_bins, bin_update;
	struct ck_dflock_bin *bin;
	struct ck_spinlock_mcs *thread_lock = malloc(sizeof(struct ck_spinlock_mcs));

	bin_index = ck_dflock_compute_insert_bin(lock, deadline);
	bin = lock->bins + bin_index;

	/* set occupied bit for appropriate bin */
	/* FIXME: Can this be fetch-and-or instead? */
	do {
		set_bins = ck_pr_load_32(&lock->occupied_bins);
		bin_update = set_bins | (1 << bin_index);
	} while (ck_pr_cas_32(&lock->occupied_bins, set_bins, bin_update) == false);

	/* acquire local lock */
	ck_spinlock_mcs_lock(&bin->lock, thread_lock);
	ck_pr_store_ptr(&bin->current_lock, thread_lock);

	/* if another thread previously held the global lock... */
	if (set_bins != 0) {
		/* ... then wait for bin to be activated */
		while (ck_pr_load_uint(&bin->active) == 0) {
			ck_pr_stall();
		}
	}

	/* 
	 * now that we have the overall lock, make a note of which bin acquired it
	 * so that we know what to deactivate upon unlocking
	 */
	ck_pr_store_32(&lock->last_used_bin, bin_index);

	return;
}


CK_CC_INLINE static void
ck_dflock_unlock(struct ck_dflock *lock)
{
	struct ck_dflock_bin *bin;
	uint32_t set_bins, bin_update;
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
		/* FIXME: Can this be fetch-and-and instead? */
		do {
			set_bins = ck_pr_load_32(&lock->occupied_bins);
			bin_update = set_bins & ~(1 << lock->last_used_bin);
		} while (ck_pr_cas_32(&lock->occupied_bins, set_bins, bin_update) == false);
	} else {
		bin_update = ck_pr_load_32(&lock->occupied_bins);
	}

	/* signal the next bin if applicable */
	next_bin = ck_dflock_next_bin(lock, bin_update);
	if (next_bin >= 0) {
		ck_pr_store_uint(&lock->bins[next_bin].active, 1);
	}

	return;
}

#endif /* _CK_DFLOCK_H */
