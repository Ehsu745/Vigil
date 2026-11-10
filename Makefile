CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDLIBS   = -lz -lm
SRC      = src/main.c src/events.c src/geom.c src/session.c src/render.c

dodeca-core: $(SRC) src/dodeca.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

demo: dodeca-core
	./dodeca-core --demo

clean:
	rm -f dodeca-core

.PHONY: demo clean
