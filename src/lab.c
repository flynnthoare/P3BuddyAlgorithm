#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "lab.h"

#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);          \
    } while (0)

/**
 * @brief Convert bytes to the correct K value
 *
 * @param bytes the number of bytes
 * @return size_t the K value that will fit bytes
 */
 size_t btok(size_t bytes)
 {
     size_t total = bytes;
     size_t k = SMALLEST_K;
 
     while ((UINT64_C(1) << k) < total) {
         k++;
     }

     return k;
 }

 /**
   * Find the buddy of a given pointer and kval relative to the base address we got from mmap
   * @param pool The memory pool to work on (needed for the base addresses)
   * @param buddy The memory block that we want to find the buddy for
   * @return A pointer to the buddy
   */
struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy)
{
    // Convert pointers to raw numbers
    uintptr_t base_addr = (uintptr_t)(pool->base);
    uintptr_t block_addr = (uintptr_t)(buddy);

    // Get how far this block is from the base of memory
    uintptr_t offset = block_addr - base_addr;

    // Flip the bit at position buddy->kval
    uintptr_t buddy_offset = offset ^ (UINT64_C(1) << buddy->kval);

    // Add offset back to base and convert to struct avail*
    return (struct avail *)(base_addr + buddy_offset);
}

void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    if (!pool || size == 0) {
        return NULL;
    }

    // add header to total
    size_t total = size + sizeof(struct avail);
    size_t req_k = btok(total);
    size_t k = req_k;

    // Find the first avail index that has a block available
    while (k < pool->kval_m && pool->avail[k].next == &pool->avail[k]) {
        k++;
    }

    printf("k val after searching for avail block: %zu\n", k);
    printf("req_k val: %zu\n", req_k);
    
    if (pool->avail[k].next == &pool->avail[k]) {
        errno = ENOMEM;
        return NULL;
    }

    struct avail *block = pool->avail[k].next;

    // guard against grabbing the sentinel
    if (block == &pool->avail[k] || block->tag != BLOCK_AVAIL) {
        errno = ENOMEM;
        return NULL; 
    }

    // Unlink from free list
    block->prev->next = block->next;
    block->next->prev = block->prev;  

    // Clean up block's old pointers
    block->next = NULL;
    block->prev = NULL;

    // set block->kval even if we don’t split
    block->kval = k;

    // prevent splitting if block is right size already
    if (k == req_k) {
        block->tag = BLOCK_RESERVED;
        fprintf(stderr, "Returned block has kval=%d\n", block->kval);
        return (void *)(block + 1); //skip header
    }

    // Split block down to req_k
    while (k > req_k) {
        k--;
    
        block->kval = k;
        struct avail *buddy = buddy_calc(pool, block);
    
        buddy->tag = BLOCK_AVAIL;
        buddy->kval = k;
    
        buddy->next = pool->avail[k].next;
        buddy->prev = &pool->avail[k];
        pool->avail[k].next->prev = buddy;
        pool->avail[k].next = buddy;
    }
    

    block->tag = BLOCK_RESERVED;
    fprintf(stderr, "Returned block has kval=%d\n", block->kval);
    return (void *)(block + 1);  // skip header
}

void buddy_free(struct buddy_pool *pool, void *ptr)
{
    if (!pool || !ptr) {
        return;
    }

    //Get the header
    struct avail *block = ((struct avail *)ptr) - 1;

    size_t k = block->kval;
    block->tag = BLOCK_AVAIL;

    while (k < pool->kval_m) {
        struct avail *buddy = buddy_calc(pool, block);

        // Make sure buddy is free and same size
        if (buddy->tag != BLOCK_AVAIL || buddy->kval != k) {
            break;
        }

        // Remove buddy from free list
        buddy->prev->next = buddy->next;
        buddy->next->prev = buddy->prev;

        // Decide who becomes the parent block (lower address)
        if (buddy < block) {
            block = buddy;
        }

        k++;
        block->kval = k;
    }

    // Insert merged block into free list
    block->tag = BLOCK_AVAIL;
    block->next = pool->avail[k].next;
    block->prev = &pool->avail[k];
    pool->avail[k].next->prev = block;
    pool->avail[k].next = block;
}

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        kval = MAX_K - 1;

    //make sure pool struct is cleared out
    memset(pool,0,sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    //Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                               /*addr to map to*/
        pool->numbytes,                     /*length*/
        PROT_READ | PROT_WRITE,             /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS,        /*flags*/
        -1,                                 /*fd -1 when using MAP_ANONYMOUS*/
        0                                   /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }

    //Set all blocks to empty. We are using circular lists so the first elements just point
    //to an available block. Thus the tag, and kval feild are unused burning a small bit of
    //memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }

    //Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}

void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    //Zero out the array so it can be reused it needed
    memset(pool,0,sizeof(struct buddy_pool));
}

#define UNUSED(x) (void)x

