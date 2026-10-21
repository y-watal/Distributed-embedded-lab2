CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS = -pthread

cockpit: cockpit.c catalog_pi.h state.h testpoints.h
	$(CC) $(CFLAGS) -o $@ cockpit.c $(LDLIBS)

clean:
	rm -f cockpit

.PHONY: clean
