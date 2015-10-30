/**
 * Copyright (c) 2015 MIT License by 6.172 Staff
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 **/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "./allocator_interface.h"
#include "./memlib.h"

// Don't call libc malloc!
#define malloc(...) (USE_MY_MALLOC)
#define free(...) (USE_MY_FREE)
#define realloc(...) (USE_MY_REALLOC)

// All blocks must have a specified minimum alignment.
// The alignment requirement (from config.h) is >= 8 bytes.
#ifndef ALIGNMENT
#define ALIGNMENT 8
#endif

// Rounds up to the nearest multiple of ALIGNMENT.
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))

// The smallest aligned size that will hold a size_t value.
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))

#define CACHE_LINE_SIZE 64
#define CACHE_ALIGN(size) (((size) + (CACHE_LINE_SIZE-1)) & ~(CACHE_LINE_SIZE-1))

#define HEADER_SIZE (sizeof(Block))

#define MIN_BLOCK_POW 5
#define MAX_BLOCK_POW 27

#define NUM_BINS (MAX_BLOCK_POW - MIN_BLOCK_POW)
#define MIN_BLOCK_SIZE (1 << MIN_BLOCK_POW)
#define MAX_BLOCK_SIZE (1 << MAX_BLOCK_POW)
#define BLOCK_SIZE(bin) (1 << ((bin) + MIN_BLOCK_POW))
#define BLOCK_BIN(size) (BLOCK_POW(size) - MIN_BLOCK_POW)
#define BLOCK(ptr) ((Block*)((char*)ptr - HEADER_SIZE))
#define BIN(block) (bins[block->bin])

const unsigned int b[] = {0x2, 0xC, 0xF0, 0xFF00, 0xFFFF0000};
const unsigned int S[] = {1, 2, 4, 8, 16};

inline static unsigned int BLOCK_POW(unsigned int v) {
  v--;
  register unsigned int r = 0;
  for (int i = 4; i >= 0; i--) {
    if (v & b[i]) {
      v >>= S[i];
      r |= S[i];
    }
  }
  return r + 1;
}

typedef struct Block {
  unsigned int bin;   // The bin that this block is in
  unsigned int free;  // Whether or not this block is free
  struct Block* next; // Pointer to next Block
} Block;

Block* bins[NUM_BINS];

int my_check() {
  return 0;
}

int my_init() {
  // Empty bins
  memset(bins, 0, NUM_BINS * sizeof(Block*));

  // Align brk with the cache line
  void* brk = mem_heap_hi() + 1;
  mem_sbrk(CACHE_ALIGN((uint64_t)brk) - (uint64_t)brk);

  return 0;
}

void* my_malloc(size_t size) {
  void* p;

  // Determine smallest block size
  unsigned int b = BLOCK_BIN(HEADER_SIZE + size);

  assert(b >= 0);

  //printf("Allocating block for data size %d.\n", size);

  // Check appropriate bin for free block
  if (bins[b]) {
    //printf("Reusing block of size %d at %p.\n", BLOCK_SIZE(b), bins[b] + HEADER_SIZE);
    p = bins[b];
    bins[b]->free = 0;
    bins[b] = bins[b]->next;
    return p + HEADER_SIZE;
  }

  // TODO: check bins[block_pow+1]...bins[MAX_BLOCK_POW]

  // Expand heap by block size
  //printf("Expanding heap by size %d.\n", BLOCK_SIZE(b));
  p = mem_sbrk(BLOCK_SIZE(b));

  // TODO: add necessary free blocks until we're aligned with the cache line

  // Return NULL on failure
  if (p == (void*)-1) return NULL;

  // Initialize block
  Block* block = p;
  block->bin = b;
  block->free = 0;
  block->next = NULL;

  return p + HEADER_SIZE;
}

void my_free(void* ptr) {
  if (!ptr) return;

  //printf("Freeing %p.\n", ptr);

  // Get block associated with pointer
  Block* block = BLOCK(ptr);

  // Get bin associated with block
  //printf("Freeing block in bin %u.\n", block->bin);
  assert(block->bin < NUM_BINS);
  Block* bin = BIN(block);
  
  // Put block in bin
  block->free = 1;
  block->next = bin;
  BIN(block) = block;

  return;
}

void* my_realloc(void* ptr, size_t size) {
  // malloc
  if (!ptr) return my_malloc(size);

  // free
  if (!size) {
    my_free(ptr);
    return NULL;
  }

  //printf("realloc\n");

  unsigned int b = BLOCK_BIN(HEADER_SIZE + size);
  Block* block = BLOCK(ptr);

  // No change
  if (b == block->bin) return ptr;

  // Shrink & chunk
  if (b < block->bin) {
    // TODO: shrink & chunk
    return ptr;
  }

  // Expand via malloc
  void* ptr_new = my_malloc(size);

  if (!ptr_new) return NULL;

  // Copy original data
  Block* block_new = ptr_new;
  memcpy(block_new, block, BLOCK_SIZE(block->bin));
  block_new->bin = b;

  // Free old block
  my_free(ptr);

  return ptr_new + HEADER_SIZE;
}

void my_reset_brk() {
  mem_reset_brk();
}

void* my_heap_lo() {
  return mem_heap_lo();
}

void* my_heap_hi() {
  return mem_heap_hi();
}

