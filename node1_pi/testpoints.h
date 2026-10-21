/*
 * Lab 3 test points on the Pi (N1) for the C cockpit code, BCM GPIO numbering.
 * Same pins as testpoints.py; full map in docs/lab3_pin_map.md.
 *
 * Raw register access through /dev/gpiomem (the Lab 2 approach), so a toggle
 * is one store with no syscall - cheap enough to bracket latency measurements.
 *
 *   if (tp_init() < 0) perror("tp_init");
 *   tp_toggle(TP_UDP_RX);
 *   tp_set(TP_ERR, 1);
 */
#ifndef TESTPOINTS_H
#define TESTPOINTS_H

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
	TP_UDP_RX   = 5,   /* row 3, header pin 29 */
	TP_CMD_TX   = 6,   /* row 4, header pin 31 */
	TP_CAN_TX_A = 13,  /* row 5, header pin 33 */
	TP_CAN_TX_B = 19,  /* row 6, header pin 35 */
	TP_DEG_A    = 26,  /* row 7, header pin 37, LED */
	TP_DEG_B    = 16,  /* row 8, header pin 36, LED */
	TP_ERR      = 20,  /* row 9, header pin 38 (added row) */
};

static const int tp_all[] = {TP_UDP_RX, TP_CMD_TX, TP_CAN_TX_A, TP_CAN_TX_B,
			     TP_DEG_A, TP_DEG_B, TP_ERR};

#define TP_GPSET0 7  /* register word offsets, BCM2835/2711 */
#define TP_GPCLR0 10

static volatile uint32_t *tp_gpio;
static uint32_t tp_state; /* current output level, one bit per GPIO */

static inline void tp_set(int gpio, int level)
{
	if (!tp_gpio) {
		return;
	}
	if (level) {
		__atomic_or_fetch(&tp_state, 1u << gpio, __ATOMIC_RELAXED);
		tp_gpio[TP_GPSET0] = 1u << gpio;
	} else {
		__atomic_and_fetch(&tp_state, ~(1u << gpio), __ATOMIC_RELAXED);
		tp_gpio[TP_GPCLR0] = 1u << gpio;
	}
}

/*
 * Safe to call from several threads: the level bookkeeping is atomic, so
 * threads toggling different pins never disturb each other. (Two threads
 * toggling the same pin at the same instant can still swap the order of the
 * two edges; give each latency-critical pin one owner.)
 */
static inline void tp_toggle(int gpio)
{
	if (!tp_gpio) {
		return;
	}
	uint32_t s = __atomic_xor_fetch(&tp_state, 1u << gpio, __ATOMIC_RELAXED);

	tp_gpio[(s >> gpio) & 1u ? TP_GPSET0 : TP_GPCLR0] = 1u << gpio;
}

/* Drives every test point low (call on exit). */
static inline void tp_all_low(void)
{
	for (unsigned i = 0; i < sizeof(tp_all) / sizeof(tp_all[0]); i++) {
		tp_set(tp_all[i], 0);
	}
}

/* Maps the GPIO block and makes every test point an output, driven low. */
static inline int tp_init(void)
{
	int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);

	if (fd < 0) {
		return -1;
	}
	void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return -1;
	}
	tp_gpio = (volatile uint32_t *)map;

	for (unsigned i = 0; i < sizeof(tp_all) / sizeof(tp_all[0]); i++) {
		int g = tp_all[i];
		int reg = g / 10, shift = (g % 10) * 3;

		tp_gpio[reg] = (tp_gpio[reg] & ~(0x7u << shift)) | (0x1u << shift); /* output */
		tp_set(g, 0);
	}
	return 0;
}

#endif /* TESTPOINTS_H */
