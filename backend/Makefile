CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2

.PHONY: all clean test

all: app library-handler

app: app.c library_handler.c library_handler.h music_ripper.c music_ripper.h assembler.c assembler.h stream_buffer.c stream_buffer.h ssh_opts.c ssh_opts.h decoder.c decoder.h metadata.c metadata.h third_party/nuklear/nuklear.h third_party/nuklear/nuklear_sdl_renderer.h
	$(CC) $(CFLAGS) -I. app.c library_handler.c music_ripper.c assembler.c stream_buffer.c ssh_opts.c decoder.c metadata.c $$(pkg-config --cflags --libs sdl2 sndfile) -pthread -lm -o $@

library-handler: library_handler.c
	$(CC) $(CFLAGS) -DLIBRARY_HANDLER_STANDALONE $< -o $@

music-ripper.o: music_ripper.c music_ripper.h library_handler.h
	$(CC) $(CFLAGS) -c music_ripper.c -o $@

assembler.o: assembler.c assembler.h
	$(CC) $(CFLAGS) -c assembler.c -o $@

stream-buffer.o: stream_buffer.c stream_buffer.h
	$(CC) $(CFLAGS) -c stream_buffer.c -o $@

ssh-opts.o: ssh_opts.c ssh_opts.h
	$(CC) $(CFLAGS) -c ssh_opts.c -o $@

decoder.o: decoder.c decoder.h stream_buffer.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags sndfile) -c decoder.c -o $@

library-handler.o: library_handler.c library_handler.h
	$(CC) $(CFLAGS) -c library_handler.c -o $@

test-music-ripper: tests/test_music_ripper.c library_handler.c library_handler.h music_ripper.c music_ripper.h
	$(CC) $(CFLAGS) -I. tests/test_music_ripper.c library_handler.c music_ripper.c -o $@

test-library-mutation: tests/test_library_mutation.c library_handler.c library_handler.h
	$(CC) $(CFLAGS) -I. tests/test_library_mutation.c library_handler.c -o $@

test-assembler: tests/test_assembler.c assembler.c assembler.h
	$(CC) $(CFLAGS) -I. tests/test_assembler.c assembler.c -o $@

test-decoder: tests/test_decoder.c decoder.c decoder.h assembler.c assembler.h
	$(CC) $(CFLAGS) -I. tests/test_decoder.c decoder.c assembler.c $$(pkg-config --cflags --libs sndfile) -o $@

test-metadata: tests/test_metadata.c metadata.c metadata.h
	$(CC) $(CFLAGS) -I. tests/test_metadata.c metadata.c -o $@

test: library-handler test-music-ripper test-library-mutation test-assembler test-decoder test-metadata
	./tests/test_library_handler.sh
	./test-music-ripper
	./tests/test_library_mutation.sh
	./test-assembler
	./test-decoder
	./test-metadata

clean:
	rm -f app library-handler library-handler.o music-ripper.o assembler.o decoder.o test-music-ripper test-library-mutation test-assembler test-decoder test-metadata
