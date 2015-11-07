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

/////////////////////////////////////////////////////////////////////////////////
// macros, helper functions

#define HEADER_SIZE 8
#define MIN_DATA_SIZE 16
#define FOOTER_SIZE (sizeof(Footer))

#define MIN_BLOCK_POW 5
#define MAX_BLOCK_POW 29
#define SHRINK_MIN_SIZE 64

#define NUM_BINS (MAX_BLOCK_POW - MIN_BLOCK_POW)
#define BLOCK(ptr) ((Block*)((uint8_t*)(ptr) - HEADER_SIZE))
#define DATA(ptr) ((void*)((uint8_t*)(ptr) + HEADER_SIZE))
#define FOOTER(block) (((Footer*)(RIGHT(block))) - 1)
#define LEFT(block) ((Block*)((uint8_t*)(block) - ((Footer*)(block) - 1)->size))
#define RIGHT(block) ((Block*)((uint8_t*)(block) + (block)->size))
#define UNDER_HI(ptr) ((uint8_t*)(ptr) < (uint8_t*)heap_hi)
#define OVER_LO(ptr) ((uint8_t*)(ptr) > (uint8_t*)heap_lo)

inline static uint32_t BLOCK_BIN(uint32_t size) {
  return 32 - __builtin_clz((size) >> MIN_BLOCK_POW);
}

/////////////////////////////////////////////////////////////////////////////////
// types:
typedef struct Block {
  unsigned int size : 31;   // The size of this block
                            // including the header, data, footer
  unsigned int free : 1;    // Whether or not this block is free
  struct Block* prev;
  struct Block* next;
} Block;

typedef struct Footer {
  unsigned int size;    // The size of this block
                        // same value as that in the header
} Footer;

/////////////////////////////////////////////////////////////////////////////////
// static functions:
static uint32_t BLOCK_BIN(uint32_t size);
static void Block_init(Block* block, uint32_t size);
static void Block_set_size(Block* block, uint32_t size);
static void push(Block* block);
static Block* pull(uint32_t size, uint32_t bin);
static void extract(Block* Block);
static void coalesce(Block* Block);
static void shrink(Block* block, uint32_t size);

/////////////////////////////////////////////////////////////////////////////////

Block* bins[NUM_BINS];

uint8_t* heap_lo;
uint8_t* heap_hi;

#ifdef DEBUG
#define valid(header) __valid(header)
inline static void __valid(Block* header) {
  assert(header);
  Footer* footer = FOOTER(header);
  assert((uint8_t*)header >= heap_lo);
  assert(((uint8_t*)footer + FOOTER_SIZE) <= heap_hi);
  assert(header->size == footer->size);
}
#else
#define valid(header);
#endif

// initialize the block: set the header and footer to the specified size
inline static void Block_init(Block* block, uint32_t size) {
  assert(block);
  assert(size <= (heap_hi - heap_lo));

  Block_set_size(block, size);
  block->free = 0;

  return;
}

inline static void Block_set_size(Block* block, uint32_t size) {
  assert(block);
  assert(size <= (heap_hi - heap_lo));

  block->size = size;
  FOOTER(block)->size = size;

  return;
}

static void push(Block* block) {
  assert(block);
  valid(block);

  uint32_t bin = BLOCK_BIN(block->size);

  block->free = 1;
  block->prev = NULL;
  block->next = bins[bin];
  if (bins[bin]) bins[bin]->prev = block;
  bins[bin] = block;

  valid(block);
  return;
}

static Block* pull(uint32_t size, uint32_t bin) {
  assert(bin >= 0);
  assert(bin < NUM_BINS);

  Block* curr = bins[bin];

  // Check if bin is empty
  if (!curr) return NULL;

  // Check first block
  if (curr->size >= size) {
    bins[bin] = curr->next;
    if (curr->next) curr->next->prev = NULL;
    return curr;
  }

  // Check remaining blocks
  Block* next;
  while ((next = curr->next)) {
    if (next->size >= size) {
      curr->next = next->next;
      if (curr->next) curr->next->prev = curr;
      return next;
    }
    curr = next;
  }

  return NULL;
}

static void extract(Block* block) {
  assert(block);
  valid(block);

  if (block->prev) {
    block->prev->next = block->next;
    if (block->next) block->next->prev = block->prev;
    return;
  }

  uint32_t bin = BLOCK_BIN(block->size);
  bins[bin] = block->next;
  if (block->next) block->next->prev = NULL;
  return;
}

static void coalesce(Block* block) {
  assert(block);
  valid(block);

  // Try to merge right into block
  Block* right = RIGHT(block);
  if (UNDER_HI(right) && right->free) {
    // Remove from bin
    extract(right);

    // Expand block
    Block_set_size(block, block->size + right->size);
  }

  // Try to merge block into left
  Block* left = LEFT(block);
  if (OVER_LO(left) && left->free) {
    // Remove from bin
    extract(left);

    // Expand left
    Block_set_size(left, left->size + block->size);

    // Push left into bin
    push(left);
  } else {
    push(block);
  }
}

static void shrink(Block* block, uint32_t size) {
  assert(block);
  assert(size <= block->size);
  valid(block);

  // Calculate leftover block size
  uint32_t size_new = block->size - size;

  // Ensure we can actually utilize the leftover block
  if (size_new >= ALIGN(SHRINK_MIN_SIZE)) {
    // Shrink original block
    Block_set_size(block, size);

    // Create leftover block, coalescing if possible
    Block* block_new = RIGHT(block);
    Block_set_size(block_new, size_new);

    coalesce(block_new);
  }
}

int my_check() {
  return 0;
}

int my_init() {
  // Empty bins
  memset(bins, 0, NUM_BINS * sizeof(Block*));

  // Align brk with the cache line
  void* brk = mem_heap_hi() + 1;
  uint64_t size = CACHE_ALIGN((uint64_t)brk) - (uint64_t)brk;

  // set the initial boundaries of the heap
  heap_lo = heap_hi = (uint8_t*)mem_sbrk(size) + size;

  return 0;
}


void* my_malloc(size_t size) {
  Block* block;

  // Determine smallest block size
  if (size < MIN_DATA_SIZE) {
    size = MIN_DATA_SIZE;
  }

  size = ALIGN(HEADER_SIZE + size + FOOTER_SIZE);

  if (heap_lo != heap_hi) {

    // Try to reuse freed blocks
    for (int bin = BLOCK_BIN(size); bin < NUM_BINS; bin++) {
      block = pull(size, bin);
      if (block) {
        block->free = 0;
        shrink(block, size);
        return DATA(block);
      }
    }

    // Check if last block can be extended
    Block* last = LEFT((Block*)heap_hi);
    if (last->free) {
      // Determine amount to expand/extend
      uint32_t diff = size - last->size;

      // Expand heap
      mem_sbrk(diff);
      heap_hi += diff;

      // Extend block
      extract(last);
      Block_init(last, size);

      return DATA(last);
    }
  }

  // Expand heap by block size
  block = mem_sbrk(size);
  heap_hi += size;

  // Return NULL on failure
  if ((void*)block == (void*)-1) return NULL;

  // Initialize block
  Block_init(block, size);

  return DATA(block);
}

void my_free(void* ptr) {
  if (!ptr) return;
  valid(BLOCK(ptr));

  // Try to coalesce block with freed neighbors
  coalesce(BLOCK(ptr));
}

void* my_realloc(void* ptr, size_t size) {
  assert(size <= heap_hi - heap_lo);

  // malloc
  if (!ptr) return my_malloc(size);

  // free
  if (!size) {
    my_free(ptr);
    return NULL;
  }

  // Calculate new block size
  uint32_t size_new = ALIGN(HEADER_SIZE + size + FOOTER_SIZE);

  Block* block = BLOCK(ptr);

  // No change
  if (size_new == block->size) return ptr;

  // Shrink
  if (size_new < block->size) {
    shrink(block, size_new);
    return ptr;
  }

  uint32_t diff = size_new - block->size;
  Block* right = RIGHT(block);

  // Expand if at end of heap
  if ((uint8_t*)right == heap_hi) {
    mem_sbrk(diff);
    heap_hi += diff;
    Block_set_size(block, size_new);
    return ptr;
  }

  // Expand if large enough free neighbor
  #ifdef EXPAND_INTO_FREE_NEIGHBOR
  if ((uint8_t*)right < heap_hi && right->free && right->size >= diff) {
    extract(right);
    Block_set_size(block, block->size + right->size);
    shrink(block, size_new);
    return ptr;
  }
  #endif

  // Move
  void* ptr_new = my_malloc(size);

  // Return NULL if allocation failed
  if (!ptr_new) return NULL;

  // Copy original data into new block
  memcpy(ptr_new, ptr, block->size - HEADER_SIZE - FOOTER_SIZE);

  // Free old block
  my_free(ptr);

  return ptr_new;
}

void my_reset_brk() {
  mem_reset_brk();
}

void* my_heap_lo() {
  return heap_lo;
}

void* my_heap_hi() {
  return heap_hi;
}

