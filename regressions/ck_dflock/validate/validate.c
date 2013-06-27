#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/time.h>

#include <ck_dflock.h>
#include <ck_pr.h>

#include "../../common.h"

#define ITERATE 10000000
#define INTERVAL 1000

static int nthr;
static unsigned int timeframe = 0;
static unsigned int locked = 0;
static struct affinity a;
static struct ck_dflock lock;

static void *
thread(void *context CK_CC_UNUSED)
{
	unsigned int i = ITERATE, j;
	unsigned int delay = (timeframe += INTERVAL);
	deadline_t deadline;

	if (aff_iterate(&a) != 0) {
		ck_error("ERROR: Could not affine thread\n");
	}

	while (i--) {
		deadline = rdtsc() + delay;
		ck_dflock_lock(&lock, deadline);

		ck_pr_inc_uint(&locked);
		ck_pr_inc_uint(&locked);
		ck_pr_inc_uint(&locked);
		ck_pr_inc_uint(&locked);
		ck_pr_inc_uint(&locked);
		ck_pr_inc_uint(&locked);
		ck_pr_inc_uint(&locked);
		ck_pr_inc_uint(&locked);
		ck_pr_inc_uint(&locked);
		ck_pr_inc_uint(&locked);

		j = ck_pr_load_uint(&locked);

		if (j != 10) {
			ck_error("ERROR (WR): Race condition (%u)\n", j);
		}

		ck_pr_dec_uint(&locked);
		ck_pr_dec_uint(&locked);
		ck_pr_dec_uint(&locked);
		ck_pr_dec_uint(&locked);
		ck_pr_dec_uint(&locked);
		ck_pr_dec_uint(&locked);
		ck_pr_dec_uint(&locked);
		ck_pr_dec_uint(&locked);
		ck_pr_dec_uint(&locked);
		ck_pr_dec_uint(&locked);

		ck_dflock_unlock(&lock);
	}

	return NULL;
}


int
main(int argc, char **argv)
{
	pthread_t *threads;
	int i;

	/* Aim for roughly 2 threads per bin */
	ck_dflock_init(&lock, rdtsc, INTERVAL * 2);

	if (argc != 3) {
		ck_error("Usage: validate <number of threads> <affinity delta>\n");
	}

	nthr = atoi(argv[1]);
	if (nthr == 0) {
		ck_error("ERROR: Number of threads must be greater than 0\n");
	}

	threads = malloc(sizeof(pthread_t) * nthr);
	if (threads == NULL) {
		ck_error("ERROR: Could not allocate threads\n");
	}

	a.delta = atoi(argv[2]);
	if (a.delta == 0) {
		ck_error("ERROR: Affinity delta must be greater than 0\n");
	}
	a.request = 0;

	for (i = 0 ; i < nthr ; ++i) {
		if (pthread_create(threads + i, NULL, thread, NULL) != 0) {
			ck_error("ERROR: Could not create thread %" PRIu64 "\n", i);
		}
	}

	fprintf(stderr, "Running correctness regression...");
	for (i = 0 ; i < nthr ; ++i) {
		pthread_join(threads[i], NULL);
	}
	fprintf(stderr, "done (passed)\n");

	return 0;
}
