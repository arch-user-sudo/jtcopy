CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pedantic
PREFIX ?= /usr/local

jtcopy: Code.c
	$(CC) $(CFLAGS) -o $@ Code.c

clean:
	rm -f jtcopy

install: jtcopy
	install -Dm755 jtcopy $(DESTDIR)$(PREFIX)/bin/jtcopy

.PHONY: clean install