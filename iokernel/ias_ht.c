/*
 * ias_ht.c - the hyperthread subcontroller
 */

#include <base/stddef.h>
#include <base/log.h>

#include "defs.h"
#include "sched.h"
#include "ksched.h"
#include "pmc.h"
#include "ias.h"

#define WARMUP_US 10

static void ias_ht_poll_one(struct ias_data *sd, struct thread *th)
{
	float ipc, us, run_us, idle_us;
	uint64_t last_tsc, last_instr, cur_tsc, cur_instr;
	int core, sib;

	core = th->core;
	sib = sched_siblings[core];

	/* calculate IPC and update counters */
	last_tsc = sd->ht_last_tsc[core];
	last_instr = sd->ht_last_instr[core];
	cur_tsc = th->q_ptrs->tsc;
	cur_instr = th->q_ptrs->instr;
	sd->ht_last_tsc[core] = cur_tsc;
	sd->ht_last_instr[core] = cur_instr;
	if (cur_tsc == last_tsc)
		return;
	if (ias_gen[core] != sd->ht_last_gen[core]) {
		sd->ht_last_gen[core] = ias_gen[core];
		return;
	}

	ipc = (float)(cur_instr - last_instr) / (float)(cur_tsc - last_tsc);
	if (ipc > 5.0 || ipc < 1E-3)
		return; /* bad sample */
	us = (float)(cur_tsc - last_tsc) / (float)cycles_per_us;

	/* update unpaired IPC metrics */
	run_us = ((float)cur_tsc - sd->ht_start_running_tsc[core]) /
		 cycles_per_us;
	idle_us = ((float)cur_tsc - cores_idle_tsc[sib]) /
		 cycles_per_us;
	if (run_us - us < WARMUP_US || idle_us - us < WARMUP_US)
		return;
	if (!cores[sib]) {
		ias_ewma(&sd->ht_unpaired_ipc, ipc,
			 MIN(100.0, us) * IAS_EWMA_FACTOR);
		return;
	}

	/* update paired IPC metrics */
	run_us = ((float)cur_tsc - cores[sib]->ht_start_running_tsc[sib]) /
		 cycles_per_us;
	if (run_us - us < WARMUP_US)
		return;
	ias_ewma(&sd->ht_pairing_ipc[cores[sib]->idx], ipc,
		 MIN(100.0, us) * IAS_EWMA_FACTOR);
}

static inline bool is_bad_pairing(struct ias_data *sd, struct ias_data *sib_sd)
{
	double cur_ipc =
		(!sib_sd) ? sd->ht_unpaired_ipc : sd->ht_pairing_ipc[sib_sd->idx];
	double ratio = (cur_ipc > 1E-3) ? cur_ipc / sd->ht_max_ipc : 1;
	return ratio <= 1 - IAS_HT_MAX_IPC_DEGRADE_RATIO;
}

/**
 * ias_ht_detect_bad_pairing - detect the bad pairing and kicked out the 
 * culprit sibling.
 */
void ias_ht_detect_bad_pairing() {
	int core, tmp;
	uint64_t now_tsc = rdtsc();
	sched_for_each_allowed_core(core, tmp) {
		struct ias_data *sd = cores[core];
		if (!sd)
			continue;
		int sib = sched_siblings[core];
		struct ias_data *sib_sd = cores[sib];
		bool sd_is_lc = is_lc(sd);
		bool sib_sd_is_lc = is_lc(sib_sd);
		/* never kick out an LC kthread */
		if (sib_sd_is_lc)
			continue;
		if (!is_bad_pairing(sd, sib_sd))
			continue;
		if (sd_is_lc) {
			if (sib_sd) {
				/* sd is LC and sib_sd is BE, ban the sibing */
				sib_sd->ht_last_banned_tsc[sd->idx] = now_tsc;
				/* find a better pairing for the banned sibling */
				ias_discover_better_pairing(sib_sd, sib, sd, now_tsc);
			}
		} else {
			/* sd is BE and sib_sd is BE|NULL, try to migrate sd */
			ias_discover_better_pairing(sd, core, sib_sd, now_tsc);
		}
	}
}

void ias_ht_poll(uint64_t now_us)
{
	struct ias_data *sd, *sd2;
	int i;

	/* update the IPC estimation for each core */
	ias_for_each_proc(sd) {
		for (i = 0; i < sd->p->active_thread_count; i++)
			ias_ht_poll_one(sd, sd->p->active_threads[i]);
	}

	/* refresh the maximum IPC for each process */
	ias_for_each_proc(sd) {
		sd->ht_max_ipc = 0;
		ias_for_each_proc(sd2) {
			sd->ht_max_ipc = MAX(sd->ht_max_ipc,
					     sd->ht_pairing_ipc[sd2->idx]);
		}
		sd->ht_max_ipc = MAX(sd->ht_max_ipc, sd->ht_unpaired_ipc);
	}
	ias_ht_detect_bad_pairing();
}
