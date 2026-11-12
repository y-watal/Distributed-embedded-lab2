#include "evlog.h"

#include <stdarg.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "lab3_config.h"
#include "node_config.h"

#define EVLOG_TEXT  80
#define EVLOG_DEPTH 16

struct evlog_item {
	uint32_t ms;
	char text[EVLOG_TEXT];
};

K_MSGQ_DEFINE(evlog_q, sizeof(struct evlog_item), EVLOG_DEPTH, 4);
atomic_t evlog_dropped;

void evlog(const char *fmt, ...)
{
	struct evlog_item item = {.ms = k_uptime_get_32()};
	va_list ap;

	va_start(ap, fmt);
	vsnprintk(item.text, sizeof(item.text), fmt, ap);
	va_end(ap);
	if (k_msgq_put(&evlog_q, &item, K_NO_WAIT) != 0) {
		atomic_inc(&evlog_dropped);
	}
}

static void evlog_thread(void *p1, void *p2, void *p3)
{
	struct evlog_item item;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_msgq_get(&evlog_q, &item, K_FOREVER);
		printk("[%s %u.%03us] >> %s\n", NODE_NAME, item.ms / 1000, item.ms % 1000, item.text);
	}
}

K_THREAD_DEFINE(evlog_tid, 1024, evlog_thread, NULL, NULL, NULL, PRIO_EVLOG, 0, 0);
