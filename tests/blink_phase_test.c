#include <assert.h>
#include <stdio.h>

#include "blink_phase.h"

/* Global time of the first lamp edge after t (1 us scan). */
static unsigned long long next_edge(unsigned long long t, uint32_t half_ms)
{
	bool start = blink_phase_on(t, half_ms);

	while (blink_phase_on(t, half_ms) == start) {
		t += 1;
	}
	return t;
}

int main(void)
{
	const uint32_t halves[] = {BLINK_TURN_HALF_MS, BLINK_HAZARD_HALF_MS, BLINK_FAILURE_HALF_MS};

	assert(!blink_phase_on(123456789, 0)); /* off */
	for (unsigned i = 0; i < 3; i++) {
		uint32_t h = halves[i];

		/* Lamp turns on exactly at every whole global second. */
		for (unsigned long long s = 1; s < 5; s++) {
			assert(blink_phase_on(s * 1000000, h));
			assert(!blink_phase_on(s * 1000000 - 1, h));
		}
		/* Toggles every half period. */
		unsigned long long e1 = next_edge(3000001, h), e2 = next_edge(e1, h);

		assert(e2 - e1 == h * 1000ull);
		/* blink_us_to_edge agrees with the scan. */
		assert(3000001 + blink_us_to_edge(3000001, h) == e1);

		/* Two nodes whose global clocks differ by 50 us switch within 50 us. */
		unsigned long long front = next_edge(7000123, h);
		unsigned long long rear = next_edge(7000123 + 50, h) - 50;

		assert(front - rear <= 50 || rear - front <= 50);
	}
	printf("blink_phase: 1/2/4 Hz edges on global seconds\n");
	return 0;
}
