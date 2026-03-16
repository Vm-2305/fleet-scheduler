CC     = gcc
CFLAGS = -Wall -O2 -I./include -pthread
LFLAGS = -pthread

all: solution simulator

solution: src/solution.c include/shared.h
	$(CC) $(CFLAGS) src/solution.c -o solution $(LFLAGS)

simulator: src/simulator.c include/shared.h
	$(CC) $(CFLAGS) src/simulator.c -o simulator $(LFLAGS)

clean:
	rm -f solution simulator

ipc-clean:
	ipcrm -M 0xCA110001 2>/dev/null; ipcrm -M 0xCA110002 2>/dev/null; \
	ipcrm -Q 0xCA110003 2>/dev/null; ipcrm -Q 0xCA110010 2>/dev/null; \
	ipcrm -Q 0xCA110011 2>/dev/null; true

.PHONY: all clean ipc-clean