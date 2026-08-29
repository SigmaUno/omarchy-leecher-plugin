#include "assembler.h"

#include <stdlib.h>
#include <string.h>

typedef struct AssemblerNode {
    AssemblerPiece piece;
    struct AssemblerNode *next;
} AssemblerNode;

struct Assembler {
    size_t piece_size;
    size_t max_queued_bytes;
    size_t queued_bytes;
    size_t queued_pieces;
    AssemblerNode *head;
    AssemblerNode *tail;
};

static void destroy_nodes(AssemblerNode *node) {
    while (node) {
        AssemblerNode *next = node->next;
        free(node->piece.data);
        free(node);
        node = next;
    }
}

Assembler *assembler_create(const AssemblerConfig *config) {
    Assembler *assembler = calloc(1, sizeof(*assembler));
    if (!assembler) return NULL;
    assembler->piece_size = config && config->piece_size ? config->piece_size : 64 * 1024;
    assembler->max_queued_bytes = config ? config->max_queued_bytes : 0;
    return assembler;
}

void assembler_piece_destroy(AssemblerPiece *piece) {
    if (!piece) return;
    free(piece->data);
    piece->data = NULL;
    piece->size = 0;
}

void assembler_destroy(Assembler *assembler) {
    if (!assembler) return;
    destroy_nodes(assembler->head);
    free(assembler);
}

int assembler_push(Assembler *assembler, const unsigned char *data, size_t size) {
    size_t offset = 0;
    AssemblerNode *new_head = NULL, *new_tail = NULL;
    if (!assembler || (!data && size)) return -1;
    if (!size) return 1;
    if (assembler->max_queued_bytes && (size > assembler->max_queued_bytes - assembler->queued_bytes)) return 0;
    while (offset < size) {
        AssemblerNode *node;
        size_t piece_size = size - offset;
        if (piece_size > assembler->piece_size) piece_size = assembler->piece_size;
        node = calloc(1, sizeof(*node));
        if (!node) { destroy_nodes(new_head); return -1; }
        node->piece.data = malloc(piece_size);
        if (!node->piece.data) { free(node); destroy_nodes(new_head); return -1; }
        memcpy(node->piece.data, data + offset, piece_size);
        node->piece.size = piece_size;
        if (new_tail) new_tail->next = node;
        else new_head = node;
        new_tail = node;
        offset += piece_size;
    }
    if (assembler->tail) assembler->tail->next = new_head;
    else assembler->head = new_head;
    assembler->tail = new_tail;
    assembler->queued_bytes += size;
    for (new_tail = new_head; new_tail; new_tail = new_tail->next) assembler->queued_pieces++;
    return 1;
}

int assembler_pop(Assembler *assembler, AssemblerPiece *piece) {
    AssemblerNode *node;
    if (!assembler || !piece) return -1;
    node = assembler->head;
    if (!node) return 0;
    assembler->head = node->next;
    if (!assembler->head) assembler->tail = NULL;
    assembler->queued_bytes -= node->piece.size;
    assembler->queued_pieces--;
    *piece = node->piece;
    free(node);
    return 1;
}

size_t assembler_queued_bytes(const Assembler *assembler) { return assembler ? assembler->queued_bytes : 0; }
size_t assembler_queued_pieces(const Assembler *assembler) { return assembler ? assembler->queued_pieces : 0; }
