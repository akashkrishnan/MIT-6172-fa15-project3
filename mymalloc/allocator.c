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

#define HEADER_SIZE (sizeof(Block))

#define MIN_BLOCK_POW 5
#define MAX_BLOCK_POW 29

#define NUM_BINS (MAX_BLOCK_POW - MIN_BLOCK_POW)
#define BLOCK_SIZE(bin) (1 << ((bin) + MIN_BLOCK_POW))
#define BLOCK(ptr) ((Block*)((char*)ptr - HEADER_SIZE))
#define DATA(ptr) ((void*)((char*)ptr + HEADER_SIZE))
#define BIN(block) (bins[block->bin])
#define LEFT(block) ((Block*)((char*)block - (block - 1)->size))
#define RIGHT(block) ((Block*)((char*)block + block->size))
#define FOOTER(block) (RIGHT(block) - 1)

typedef struct Block {
  unsigned int size : 31; // The bin that this block is in
  unsigned int free : 1;  // Whether or not this block is free
  struct Block* next;     // Pointer to next Block
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

const unsigned int b[] = {0x2, 0xC, 0xF0, 0xFF00, 0xFFFF0000};
const unsigned int S[] = {1, 2, 4, 8, 16};

inline static unsigned int my_heap_size() {
  return (unsigned int)((char*)my_heap_hi() - (char*)my_heap_lo());
}

inline static unsigned int BLOCK_BIN(unsigned int v) {
  v >>= MIN_BLOCK_POW;
  register unsigned int r = 0;
  for (int i = 4; i >= 0; i--) {
    if (v & b[i]) {
      v >>= S[i];
      r |= S[i];
    }
  }

  if (r >= NUM_BINS) printf("r = %u\n", r);
  //assert(r >= 0);
  //assert(r < NUM_BINS);

  return r >= NUM_BINS ? NUM_BINS - 1 : r;
}

inline static void setFree(Block* block, unsigned int free) {
  assert(block);

  // Set the header
  block->free = free;

  // Set the foooter
  FOOTER(block)->free = free;

  return;
}

inline static void setSize(Block* block, unsigned int size) {
  assert(block);
  if (size > my_heap_size()) printf("%u > %u\n", size, my_heap_size());
  assert(size <= my_heap_size());

  // Set the header
  block->size = size;

  // Set the footer
  FOOTER(block)->size = size;

  return;
}

inline static void setNext(Block* block, Block* next) {
  assert(block);

  // Set the header
  block->next = next;

  // Set the footer
  FOOTER(block)->next = next;

  return;
}

inline static void push(Block* block) {
  assert(block);

  setFree(block, 1);

  unsigned int size = block->size;
  unsigned int bin = BLOCK_BIN(size);

  Block* curr = bins[bin];

  // Check if bin is empty
  if (!curr || size <= curr->size) {
    setNext(block, curr);
    bins[bin] = block;
    return;
  }

  // Check remaining blocks
  Block* next;
  while ((next = curr->next)) {
    if (size <= next->size) {
      setNext(block, next);
      setNext(curr, block);
      return;
    }
    curr = next;
  }

  // Add to end
  setNext(block, NULL);
  setNext(curr, block);

  return;
}

inline static Block* pull(unsigned int size, unsigned int bin) {
  assert(bin >= 0);
  assert(bin < NUM_BINS);

  Block* curr = bins[bin];

  // Check if bin is empty
  if (!curr) return NULL;

  // Check first block
  if (curr->size >= size) {
    bins[bin] = curr->next;
    setFree(curr, 0);
    return curr;
  }

  // Check remaining blocks
  Block* next;
  while ((next = curr->next)) {
    if (next->size >= size) {
      setNext(curr, next->next);
      setFree(next, 0);
      return next;
    }
    curr = next;
  }

  return NULL;
}

inline static void extract(Block* block) {
  assert(block);

  unsigned int size = block->size;
  unsigned int bin = BLOCK_BIN(size);

  Block* curr = bins[bin];

  // Check if bin is empty
  if (!curr) return;

  // Check first block
  if (curr == block) {
    bins[bin] = curr->next;
    return;
  }

  // Check remaining blocks
  Block* next;
  while ((next = curr->next)) {
    if (next == block) {
      setNext(curr, next->next);
      return;
    }
    curr = next;
  }

  return;
}

inline static void valid(Block* header) {
  Block* footer = FOOTER(header);
  unsigned int wrong = 0;
  wrong += header->size != footer->size;
  wrong += (header->next != footer->next) << 1;
  wrong += (header->free != footer->free) << 2;
  if (wrong) printf("wrong=%u\n", wrong);
  assert(!wrong);
}

inline static void coalesce(Block* block) {
  assert(block);
  valid(block);

  // Try to merge right into block
  Block* right = RIGHT(block);
  if (((char*)right < (char*)mem_heap_hi()) && right->free) {
    // Remove from bin
    extract(right);

    // Expand block
    setSize(block, block->size + right->size);
  }

  // Try to merge block into left
  Block* left = LEFT(block);
  if ((char*)left >= (char*)mem_heap_lo() && left->free) {
    // Remove from bin
    extract(left);

    // Expand left
    setSize(left, left->size + block->size);

    // Push left into bin
    push(left);
  } else {
    push(block);
  }

  return;
}

inline static void shrink(Block* block, unsigned int size) {
  assert(block);
  assert(size <= block->size);

  // Calculate leftover block size
  unsigned int size_new = block->size - size;

  // Ensure we can actually utilize the leftover block
  if (size_new >= ALIGN(4 * HEADER_SIZE)) {

    // Shrink original block
    setSize(block, size); //|---------------------|-----|---------------|-----------|

    // Create leftover block, coalescing if possible
    Block* block_new = RIGHT(block);
    setSize(block_new, size_new);
    coalesce(block_new);
  }

  return;
}

void* my_malloc(size_t size) {
  Block* block;

  // Determine smallest block size
  size = ALIGN(2 * HEADER_SIZE + size);
  int bin = BLOCK_BIN(size);

  // Try to reuse freed blocks
  for (int i = bin; i < NUM_BINS; i++) {
    block = pull(size, i);
    if (block) {
      //shrink(block, size);
      valid(block);
      return DATA(block);
    }
  }

  // Expand heap by block size
  block = mem_sbrk(size);

  // Return NULL on failure
  if ((void*)block == (void*)-1) return NULL;

  // Initialize block
  setSize(block, size);
  setFree(block, 0);

  valid(block);

  return DATA(block);
}

inline static unsigned int inside_heap(void* ptr) {
  return my_heap_lo() <= ptr && ptr <= my_heap_hi();
}

void my_free(void* ptr) {
  if (!ptr) return;

  assert(inside_heap(ptr));

  // Get block associated with pointer
  Block* block = BLOCK(ptr);

  // Try to coalesce block with freed neighbors
  valid(block);
  if (!block->free) {
    setFree(block, 1);
    coalesce(block);
  }

  return;
}

void* my_realloc(void* ptr, size_t size) {
  // malloc
  if (!ptr) return my_malloc(size);

  assert(inside_heap(ptr));

  // free
  if (!size) {
    my_free(ptr);
    return NULL;
  }

  // Calculate new block size
  unsigned int size_new = ALIGN(2 * HEADER_SIZE + size);
  Block* block = BLOCK(ptr);

  // No change
  if (size_new == block->size) return ptr;

  // Shrink
  if (size_new < block->size) {
    //shrink(block, size_new);
    return ptr;
  }

  // TODO: expand by looking at freed neighbors

  // Move
  void* ptr_new = my_malloc(size);

  // Return NULL if allocation failed
  if (!ptr_new) return NULL;

  // Copy original data into new block
  memcpy(ptr_new, ptr, block->size - 2 * HEADER_SIZE);
  valid(BLOCK(ptr_new));

  // Free old block
  my_free(ptr);

  return ptr_new;
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

