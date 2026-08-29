#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <stddef.h>

typedef struct Assembler Assembler;

typedef struct {
    size_t piece_size;       /* Defaults to 64 KiB when zero. */
    size_t max_queued_bytes; /* Zero permits an unbounded queue. */
} AssemblerConfig;

typedef struct {
    unsigned char *data;
    size_t size;
} AssemblerPiece;

Assembler *assembler_create(const AssemblerConfig *config);
void assembler_destroy(Assembler *assembler);

/* Returns 1 when queued, 0 for configured backpressure, or -1 on error. */
int assembler_push(Assembler *assembler, const unsigned char *data, size_t size);

/* Returns 1 with a piece, 0 when the queue is empty, or -1 on invalid input. */
int assembler_pop(Assembler *assembler, AssemblerPiece *piece);
void assembler_piece_destroy(AssemblerPiece *piece);
size_t assembler_queued_bytes(const Assembler *assembler);
size_t assembler_queued_pieces(const Assembler *assembler);

#endif
