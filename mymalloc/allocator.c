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
#define ALIGNMENT 8

// Rounds up to the nearest multiple of ALIGNMENT.
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))

// The smallest aligned size that will hold a size_t value.
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))

#define CACHE_LINE_SIZE 64
#define CACHE_ALIGN(size) (((size) + (CACHE_LINE_SIZE-1)) & ~(CACHE_LINE_SIZE-1))

////////////////////////////////////////////////////////////////////////////////
// macros, helper functions

/* Useful constants */
#define FREE 1
#define NOT_FREE 0

#define PREV_ALLOC_INIT ((block_t*)(-1))

#define PTR_SIZE (sizeof(block_t*))
#define BLOCK_SIZE (sizeof(block_t))
#define HEADER_SIZE ((size_t)8)
#define LINKS_SIZE (2 * PTR_SIZE)

#define FREE_BIT (0x1U)
#define UNDER_BIT (0x2U)
#define INFO_BITS (FREE_BIT)

#define MIN_BLOCK_POW 4
#define MAX_BLOCK_POW 29

#define MIN_STORAGE (round_up(LINKS_SIZE))
#define SHRINK_MIN_SIZE 24
#define NUM_BINS (MAX_BLOCK_POW - MIN_BLOCK_POW)


/* Given a pointer, get the different perspectives corresponding to it */
#define block(ptr) ((block_t*)((uint8_t*)ptr - HEADER_SIZE))
#define data(ptr) ((void*)((uint8_t*)ptr + HEADER_SIZE))

/* Used for testing conditions  */
#define under_hi(ptr) ((uint8_t*)(ptr) < (uint8_t*)heap_hi)
#define over_lo(ptr) ((uint8_t*)(ptr) >= (uint8_t*)heap_lo)
#define size_fits(size) ((size) < LINKS_SIZE)
#define block_is_set(block) ((block) != NULL)

/* Getters for a block_t. The *_free macros are used to determine whether
 * or not a block has been set free or not. */
#define block_size(block) ((block)->size & ~INFO_BITS)
#define block_prev_size(block) ((block)->prev_size & ~INFO_BITS)
#define block_is_free(block) ((block)->size & FREE_BIT)
#define prev_is_free(block) ((block)->prev_size & FREE_BIT)

/* Setters for the fields of a block_t */
#define set_freeness(block, free) \
  (block->size = (((block)->size & ~FREE_BIT) | free))
#define mask_and_set_size(block, size) \
  (block->size = (((block)->size & INFO_BITS) | (size & ~INFO_BITS)))

/* Finding a block_t's neighbors */
#define right(block) \
  ((block_t*)((uint8_t*)block + block_size(block)))
#define left(block) \
  ((block_t*)((uint8_t*)(block) - block_prev_size(block)))

/* Other useful macros */
#define round_up(size) ALIGN((size) + HEADER_SIZE)
#define clear_block(block) ((block) = NULL)

#define INLINE inline __attribute__ ((always_inline))


////////////////////////////////////////////////////////////////////////////////
// types:

/* Keeps the state of a block of memory. When a block is not free, keeps track
 * of its size and it's left neighbor's size. When it is free, it also stores
 * pointers to the next block and previous block in its corresponding free list
 */
typedef struct block_t {
  uint32_t prev_size;    // keep track of size of the previous block in space
  uint32_t size;         // The size of this block
                         // including the header and data

  /* The following two field used only when the block is free */
  struct block_t* next;  // Points to the next block in the free list
  struct block_t* prev;  // Points to the previous block in the free list
} block_t;


////////////////////////////////////////////////////////////////////////////////
// static functions:

/* Operations pertaining to block_t */
static void block_init(block_t* block, uint32_t size);
static void block_set_free(block_t* block, uint8_t free);
static void block_set_size(block_t* block, uint32_t size);
static uint32_t block_bin(uint32_t size);

/* Operations on the free lists */
static void push(block_t* block);
static block_t* pull(uint32_t size, uint32_t bin);
static void extract(block_t* block_t);
static void coalesce(block_t* block_t);
static void shrink(block_t* block, uint32_t size);

////////////////////////////////////////////////////////////////////////////////
// Globals, actual functions

/* The array of free lists */
block_t* bins[NUM_BINS];

/* The lowest address in the heap */
uint8_t* heap_lo;

/* The address after the last byte in the heap */
uint8_t* heap_hi;

/* prev_alloc is the first block_t before the brk pointer */
block_t* prev_alloc;

/* Used to keep track of invariants */
#ifdef DEBUG
#define valid(header) __valid(header)
static void __valid(block_t* header) {
  assert(header);
  assert(((uint8_t*)prev_alloc + ((size_t)block_size(prev_alloc))) == heap_hi);
  assert((uint8_t*)header >= heap_lo);
  assert((uint8_t*)header + block_size(header) <= heap_hi);

  if (under_hi(right(header))) {
    assert(block_size(header) == block_prev_size(right(header)));
    assert(block_is_free(header) == prev_is_free(right(header)));
  }
}
#else
#define valid(header);
#endif

/**
 * Called the first time a block has been allocated in memory. By the first
 * time, we refer to an allocation corresponding to when the brk pointer has
 * been changed, rather than an allocation by taking from a free list. It sets
 * the size of the block in the block's size field and the next block's
 * prev_size field.
 */
inline static void block_init(block_t* block, uint32_t size) {
  assert(block);
  assert(size <= (heap_hi - heap_lo));

  // at init block is not free
  block->size = size;

  // check if there was a previously allocated block, and update its info
  if (prev_alloc == PREV_ALLOC_INIT) {
    block->prev_size = 0;
  } else {
    block->prev_size = prev_alloc->size;
  }
}

/**
 * Sets the block as free is free == 1, and not free if free == 0. The FREE_BIT
 * is just the last bit in the size field.
 */
INLINE static void block_set_free(block_t* block, uint8_t free) {
  set_freeness(block, free);

  // update in the next block, if it exists.
  if (under_hi(right(block))) {
    right(block)->prev_size = block->size;
  }
}

/**
 * Sets the block's size field and the next block in memory's prev_size field.
 */
INLINE static void block_set_size(block_t* block, uint32_t size) {
  assert(block);
  assert(size <= (heap_hi - heap_lo));

  mask_and_set_size(block, size);

  // update size in the next block, if it exists
  if (under_hi(right(block)))
    right(block)->prev_size = block->size;
}

/** Check if block is the prev_alloc, and update the global variable if so. */
INLINE static void block_update_last(block_t* block) {
  if ((uint8_t*)right(block) == heap_hi) {
    prev_alloc = block;
  }
}

/**
 * Given a size, returns the corresponsing bin to which the size belongs to.
 */
inline static uint32_t block_bin(uint32_t size) {
  return 32 - __builtin_clz((size) >> MIN_BLOCK_POW);
}


/**
 * Add the given block to the free list to which it belongs.
 */
INLINE static void push(block_t* block) {
  assert(block);
  uint32_t bin = block_bin(block_size(block));

  block_set_free(block, FREE);
  clear_block(block->prev);

  if (bins[bin]) {
    bins[bin]->prev = block;
  }

  block->next = bins[bin];
  bins[bin] = block;
}

/**
 * Remove the first block from the free list corresponding to bin.
 * The returned block's size is at least as big as the size given to the
 * function.
 */
static block_t* pull(uint32_t size, uint32_t bin) {
  assert(bin >= 0);
  assert(bin < NUM_BINS);

  block_t* curr = bins[bin];

  // Check if bin is empty
  if (!curr) return NULL;

  // Check first block
  if (block_size(curr) >= size) {
    bins[bin] = curr->next;
    if (bins[bin]) {
      clear_block(bins[bin]->prev);
    }
    block_set_free(curr, NOT_FREE);
    return curr;
  }

  // Check remaining blocks
  block_t* next;
  while ((next = curr->next)) {
    if (block_size(next) >= size) {
        curr->next = next->next;
      if (curr->next)
        curr->next->prev = curr;
      block_set_free(next, NOT_FREE);
      return next;
    }
    curr = next;
  }
  return NULL;
}

/**
 * Given a block, remove it from the free list that its in. The block must be in
 * the free list before calling this function.
 */
static void extract(block_t* block) {
  assert(block_is_free(block));

  uint32_t bin = block_bin(block_size(block));

  if (block->prev) {
    block->prev->next = block->next;
    if (block->next) {
      block->next->prev = block->prev;
    }
    return;
  }

  bins[bin] = block->next;
  if (block->next) {
    clear_block(block->next->prev);
  }
}

/**
 * Given a block in memory, and assuming that it is free, check if any of its
 * neighboring blocks are free and merge them together to make a bigger free
 * block.
 */
static void coalesce(block_t* block) {
  assert(block);

  // Try to merge right into block
  block_t* right = right(block);
  if (under_hi(right) && block_is_free(right)) {
    // Remove from bin
    extract(right);

    // Expand block
    block_set_size(block, block_size(block) + block_size(right));
    block_update_last(block);
  }

  // Try to merge block into left
  block_t* left = left(block);
  if (over_lo(left) && block_is_free(left)) {
    // Remove from bin
    extract(left);

    // Expand left
    block_set_size(left, block_size(left) + block_size(block));
    block_update_last(left);

    // Push left into bin
    push(left);
  } else {
    push(block);
  }
}

/**
 * Given a block, shrink the block down to the requested size. Assuming that the
 * given size is smaller than the block's initial size. If the remaining space
 * from splitting is smaller than the SHRINK_MIN_SIZE threshold, the
 * block is not split.
 */
static void shrink(block_t* block, uint32_t size) {
  assert(size <= block_size(block));

  // Calculate leftover block size
  uint32_t size_new = block_size(block) - size;

  // Ensure we can actually utilize the leftover block
  if (size_new >= ALIGN(SHRINK_MIN_SIZE)) {
    // Shrink original block
    block_set_size(block, size);

    // Create leftover block, coalescing if possible
    block_t* block_new = right(block);
    block_set_size(block_new, size_new);
    block_update_last(block_new);

    coalesce(block_new);
  }
}

int my_check() {
  return 0;
}

/**
 * init - Initialize the malloc package.  Called once before any other
 * calls are made.  Since this is a very simple implementation, we just
 * return success.
 */
int my_init() {
  // Empty bins, initialize globals
  memset(bins, 0, NUM_BINS * sizeof(block_t*));

  // Align brk with the cache line
  void* brk = mem_heap_hi() + 1;
  uint64_t size = CACHE_ALIGN((uint64_t)brk) - (uint64_t)brk;

  // set the initial boundaries of the heap
  heap_lo = heap_hi = (uint8_t*)mem_sbrk(size) + size;
  prev_alloc = PREV_ALLOC_INIT;
  return 0;
}

/**
 * malloc - Allocate a block by incrementing the brk pointer.
 * Always allocate a block whose size is a multiple of the alignment.
 */
void* my_malloc(size_t size) {
  block_t* block;

  // make sure we have space to store
  size = size_fits(size) ?  MIN_STORAGE : round_up(size);

  if (heap_hi != heap_lo) {
    // Try to reuse freed blocks
    for (int bin = block_bin(size); bin < NUM_BINS; bin++) {
      block = pull(size, bin);
      if (block) {
        shrink(block, size);
        return data(block);
      }
    }

    if (block_is_free(prev_alloc)) {
      extract(prev_alloc);
      size_t diff = ALIGN(size - block_size(prev_alloc));
      heap_hi = (uint8_t*)mem_sbrk(diff) + diff;

      // automatically sets the FREE_BIT to zero
      prev_alloc->size = block_size(prev_alloc) + diff;
      return data(prev_alloc);
    }
  }


  // Expand heap by block size
  block = mem_sbrk(size);

  // Return NULL on failure
  if ((void*)block == (void*)-1) return NULL;

  heap_hi = (uint8_t*)block + size;

  // Initialize block
  block_init(block, size);

  prev_alloc = block;
  return data(block);
}

/**
 * Add the block to its appropriate free list and coalesce if possible.
 */
void my_free(void* ptr) {
  if (!ptr) return;

  // Try to coalesce block with freed neighbors
  coalesce(block(ptr));
}

/** realloc - Implemented simply in terms of malloc and free */
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
  uint32_t size_new = round_up(size);

  block_t* block = block(ptr);

  // No change
  if (size_new == block_size(block)) return ptr;

  // Shrink
  if (size_new < block_size(block)) {
    shrink(block, size_new);
    return ptr;
  }

  uint32_t diff = size_new - block_size(block);
  block_t* right = right(block);

  // Expand if at end of heap
  if ((uint8_t*)right == heap_hi) {
    mem_sbrk(diff);
    heap_hi += diff;
    block_set_size(block, size_new);
    return ptr;
  }

  // Move
  void* ptr_new = my_malloc(size);

  // Return NULL if allocation failed
  if (!ptr_new) return NULL;

  // Copy original data into new block
  memcpy(ptr_new, ptr, block_size(block) - HEADER_SIZE);

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

