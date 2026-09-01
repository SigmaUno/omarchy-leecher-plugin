CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2

.PHONY: all clean test

all: app library-handler

app: app.c library_handler.c library_handler.h stream_buffer.c stream_buffer.h ssh_opts.c ssh_opts.h decoder.c decoder.h metadata.c metadata.h third_party/nuklear/nuklear.h third_party/nuklear/nuklear_sdl_renderer.h
	$(CC) $(CFLAGS) -I. app.c library_handler.c stream_buffer.c ssh_opts.c decoder.c metadata.c $$(pkg-config --cflags --libs sdl2 sndfile) -pthread -lm -o $@

library-handler: library_handler.c library_handler.h
	$(CC) $(CFLAGS) -DLIBRARY_HANDLER_STANDALONE $< -o $@

# The test suite lives in ../testing (unit + integration); run it there.
# `make -C backend test` is a convenience wrapper.
test:
	sh ../testing/run.sh

clean:
	rm -f app library-handler
