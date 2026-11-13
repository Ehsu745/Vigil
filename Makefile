CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDLIBS   = -lz -lm
SRC      = src/main.c src/events.c src/geom.c src/session.c src/motion.c src/render.c

vigil: $(SRC) src/vigil.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

# the shared C as WebAssembly for the Core Bestiary page (web/bestiary.html); needs clang, lld and wasi-libc
WASM_CC  ?= clang
WASM_SRC  = src/wasm.c src/geom.c src/session.c src/motion.c src/render.c
wasm: web/vigil-wasm.js

web/vigil.wasm: $(WASM_SRC) src/vigil.h
	$(WASM_CC) --target=wasm32-wasi --sysroot=/usr -isystem /usr/include/wasm32-wasi -L/usr/lib/wasm32-wasi \
	  -O2 -std=c11 -Wall -Wextra -D_DEFAULT_SOURCE -mexec-model=reactor -Wl,--strip-all -o $@ $(WASM_SRC) -lm

# the module inlined as base64, so the page also works opened straight from disk (file://)
web/vigil-wasm.js: web/vigil.wasm
	printf 'window.VIGIL_WASM = "' > $@ && base64 -w0 $< >> $@ && printf '";\n' >> $@

demo: vigil
	./vigil --demo

clean:
	rm -f vigil web/vigil.wasm web/vigil-wasm.js

.PHONY: demo clean wasm
