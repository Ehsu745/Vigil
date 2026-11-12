CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDLIBS   = -lz -lm
SRC      = src/main.c src/events.c src/geom.c src/session.c src/motion.c src/render.c

vigil: $(SRC) src/vigil.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

demo: vigil
	./dodeca-core --demo

clean:
	rm -f vigil

.PHONY: demo clean
