#include "jitter.h"

#if JITTER_MEASURE

#include <string.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/time_units.h>

#include "canbus.h"

struct jit_stat {
	uint32_t n, max, max_win; /* cycles */
	uint64_t sum;
};

/* [own / relayed][catalog slot][bus the frame was queued on] */
static struct jit_stat stats[2][CANBUS_NUM_SLOTS][NUM_BUSES];
static struct jit_stat snapshot[2][CANBUS_NUM_SLOTS][NUM_BUSES]; /* off REPORT's stack */
static struct k_spinlock lock;

static const char *name_of(uint16_t id)
{
	switch (id) {
	case ID_CTRL:
		return "CTRL    ";
	case ID_BLINK:
		return "BLINK   ";
	case ID_FORCE:
		return "FORCE   ";
	default:
		break;
	}
	if (catalog_is_hb(id)) {
		static const char *const hb[] = {"HB_N1   ", "HB_N2   ", "HB_N3   "};

		return hb[catalog_sender(id)];
	}
	static const char *const fault[] = {"FAULT_N1", "FAULT_N2", "FAULT_N3"};
	int s = catalog_sender(id);

	return s >= 0 && s < 3 ? fault[s] : "?       ";
}

void jit_note(uint32_t id, int bus, uint32_t release, bool relayed)
{
	uint32_t j = k_cycle_get_32() - release;
	int slot = -1;

	if (release == 0 || bus < 0 || bus >= NUM_BUSES) {
		return;
	}
	for (int i = 0; i < CANBUS_NUM_SLOTS; i++) {
		if (canbus_slot_id[i] == id) {
			slot = i;
		}
	}
	if (slot < 0) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	struct jit_stat *st = &stats[relayed ? 1 : 0][slot][bus];

	st->max = MAX(st->max, j);
	st->max_win = MAX(st->max_win, j);
	st->sum += j;
	st->n++;
	k_spin_unlock(&lock, key);
}

/* Cycles -> microseconds, one decimal, rounded up. */
static void put_us(uint64_t cycles)
{
	uint32_t tenths = (uint32_t)((k_cyc_to_ns_ceil64(cycles) + 99) / 100);

	printk("%u.%u", tenths / 10, tenths % 10);
}

void jit_report(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memcpy(snapshot, stats, sizeof(snapshot));
	for (int r = 0; r < 2; r++) {
		for (int s = 0; s < CANBUS_NUM_SLOTS; s++) {
			for (int b = 0; b < NUM_BUSES; b++) {
				stats[r][s][b].max_win = 0;
			}
		}
	}
	k_spin_unlock(&lock, key);

	printk("  J, trigger -> queued in the MCP2515, us: max since boot / max last %d s / mean (frames)\n",
	       JITTER_REPORT_EVERY);
	for (int r = 0; r < 2; r++) {
		for (int s = 0; s < CANBUS_NUM_SLOTS; s++) {
			bool any = false;

			for (int b = 0; b < NUM_BUSES; b++) {
				struct jit_stat *st = &snapshot[r][s][b];

				if (st->n == 0) {
					continue;
				}
				if (!any) {
					printk("    %s%s", r ? "relayed " : "", name_of(canbus_slot_id[s]));
					any = true;
				}
				printk("  %c ", 'A' + b);
				put_us(st->max);
				printk(" / ");
				put_us(st->max_win);
				printk(" / ");
				put_us(st->sum / st->n);
				printk(" (%u)", st->n);
			}
			if (any) {
				printk("\n");
			}
		}
	}
}

#endif /* JITTER_MEASURE */
