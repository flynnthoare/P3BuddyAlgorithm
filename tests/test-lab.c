#include <assert.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif
#include "harness/unity.h"
#include "../src/lab.h"


void setUp(void) {
  // set stuff up here
}

void tearDown(void) {
  // clean stuff up here
}



/**
 * Check the pool to ensure it is full.
 */
void check_buddy_pool_full(struct buddy_pool *pool)
{
  //A full pool should have all values 0-(kval-1) as empty
  for (size_t i = 0; i < pool->kval_m; i++)
    {
      assert(pool->avail[i].next == &pool->avail[i]);
      assert(pool->avail[i].prev == &pool->avail[i]);
      assert(pool->avail[i].tag == BLOCK_UNUSED);
      assert(pool->avail[i].kval == i);
    }

  //The avail array at kval should have the base block
  assert(pool->avail[pool->kval_m].next->tag == BLOCK_AVAIL);
  assert(pool->avail[pool->kval_m].next->next == &pool->avail[pool->kval_m]);
  assert(pool->avail[pool->kval_m].prev->prev == &pool->avail[pool->kval_m]);

  //Check to make sure the base address points to the starting pool
  //If this fails either buddy_init is wrong or we have corrupted the
  //buddy_pool struct.
  assert(pool->avail[pool->kval_m].next == pool->base);
}

/**
 * Check the pool to ensure it is empty.
 */
void check_buddy_pool_empty(struct buddy_pool *pool)
{
  for (size_t i = 0; i <= pool->kval_m; i++)
  {
    if (pool->avail[i].next != &pool->avail[i]) {
      fprintf(stderr,
              "! Non-empty free list at index %zu (2^%zu bytes):\n",
              i, i);
      struct avail *cur = pool->avail[i].next;
      while (cur != &pool->avail[i]) {
        fprintf(stderr,
                "  -> Block at %p: tag=%d kval=%d next=%p prev=%p\n",
                (void *)cur, cur->tag, cur->kval,
                (void *)cur->next, (void *)cur->prev);
        cur = cur->next;
      }
    }
  }
  
  
  //An empty pool should have all values 0-(kval) as empty
  for (size_t i = 0; i <= pool->kval_m; i++)
    {
      assert(pool->avail[i].next == &pool->avail[i]);
      assert(pool->avail[i].prev == &pool->avail[i]);
      assert(pool->avail[i].tag == BLOCK_UNUSED);
      assert(pool->avail[i].kval == i);
    }
}

/**
 * Test allocating 1 byte to make sure we split the blocks all the way down
 * to MIN_K size. Then free the block and ensure we end up with a full
 * memory pool again
 */
void test_buddy_malloc_one_byte(void)
{
  fprintf(stderr, "->Test allocating and freeing 1 byte\n");
  struct buddy_pool pool;
  int kval = MIN_K;
  size_t size = UINT64_C(1) << kval;
  buddy_init(&pool, size);
  void *mem = buddy_malloc(&pool, 1);
  //Make sure correct kval was allocated
  buddy_free(&pool, mem);
  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
}

/**
 * Tests the allocation of one massive block that should consume the entire memory
 * pool and makes sure that after the pool is empty we correctly fail subsequent calls.
 */
void test_buddy_malloc_one_large(void)
{
  fprintf(stderr, "->Testing size that will consume entire memory pool\n");
  struct buddy_pool pool;
  size_t bytes = UINT64_C(1) << MIN_K;
  buddy_init(&pool, bytes);

  //Ask for an exact K value to be allocated. This test makes assumptions on
  //the internal details of buddy_init.
  size_t ask = bytes - sizeof(struct avail);
  void *mem = buddy_malloc(&pool, ask);
  printf("made it here 1\n");
  assert(mem != NULL);
  printf("made it here 2\n");

  //Move the pointer back and make sure we got what we expected
  struct avail *tmp = (struct avail *)mem - 1;
  printf("made it here 3\n");
  assert(tmp->kval == MIN_K);
  printf("made it here 4\n");
  assert(tmp->tag == BLOCK_RESERVED);
  printf("made it here 5\n");
  check_buddy_pool_empty(&pool);

  //Verify that a call on an empty tool fails as expected and errno is set to ENOMEM.
  void *fail = buddy_malloc(&pool, 5);
  printf("made it here 6\n");
  assert(fail == NULL);
  printf("made it here 7\n");
  assert(errno = ENOMEM);
  printf("made it here 8\n");

  //Free the memory and then check to make sure everything is OK
  buddy_free(&pool, mem);
  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
  printf("made it here 9\n");
}

/**
 * Tests to make sure that the struct buddy_pool is correct and all fields
 * have been properly set kval_m, avail[kval_m], and base pointer after a
 * call to init
 */
void test_buddy_init(void)
{
  fprintf(stderr, "->Testing buddy init\n");
  //Loop through all kval MIN_k-DEFAULT_K and make sure we get the correct amount allocated.
  //We will check all the pointer offsets to ensure the pool is all configured correctly
  for (size_t i = MIN_K; i <= DEFAULT_K; i++)
    {
      size_t size = UINT64_C(1) << i;
      struct buddy_pool pool;
      buddy_init(&pool, size);
      check_buddy_pool_full(&pool);
      buddy_destroy(&pool);
    }
}

void test_btok_boundaries(void) {
  fprintf(stderr, "-> Testing btok boundaries\n");

  size_t size1 = (UINT64_C(1) << 6) - 1; // 63
  size_t size2 = (UINT64_C(1) << 6);     // 64
  size_t size3 = (UINT64_C(1) << 6) + 1; // 65

  size_t k1 = btok(size1);
  size_t k2 = btok(size2);
  size_t k3 = btok(size3);

  assert(k1 == 6); 
  assert(k2 == 6); 
  assert(k3 == 7); 
}

void test_buddy_calc(void) {
  fprintf(stderr, "-> Testing buddy_calc correctness\n");

  struct buddy_pool pool;
  buddy_init(&pool, UINT64_C(1) << MIN_K);

  // Allocate two blocks that will be buddies
  void *a = buddy_malloc(&pool, 64);
  void *b = buddy_malloc(&pool, 64);

  struct avail *block_a = ((struct avail *)a) - 1;
  struct avail *block_b = ((struct avail *)b) - 1;

  struct avail *calc_buddy = buddy_calc(&pool, block_a);
  struct avail *expected_buddy = (block_a < block_b) ? block_b : block_a;

  // Check that buddy_calc returns the other block
  assert(calc_buddy == (expected_buddy)); // returning adr at head (not data)

  buddy_destroy(&pool);
}

void test_malloc_minimum_block(void) {
  struct buddy_pool pool;
  size_t size = UINT64_C(1) << MIN_K;
  buddy_init(&pool, size);

  size_t min_block_size = (UINT64_C(1) << SMALLEST_K) - sizeof(struct avail);
  void *mem = buddy_malloc(&pool, min_block_size);
  TEST_ASSERT_NOT_NULL(mem);

  struct avail *header = (struct avail *)mem - 1;
  TEST_ASSERT_EQUAL_UINT16(SMALLEST_K, header->kval);
  TEST_ASSERT_EQUAL_UINT16(BLOCK_RESERVED, header->tag);

  buddy_free(&pool, mem);
  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
}

void test_malloc_multiple_small_blocks(void) {
  struct buddy_pool pool;
  size_t pool_size = UINT64_C(1) << MIN_K;
  buddy_init(&pool, pool_size);

  // Allocate an initial array to store pointers
  size_t capacity = 128;
  size_t count = 0;
  void **allocations = malloc(sizeof(void *) * capacity);

  // Each block asks for the smallest possible user size
  size_t user_size = (1 << SMALLEST_K) - sizeof(struct avail);

  while (true) {
    // Expand array if needed
    if (count >= capacity) {
      capacity *= 2;
      void **new_allocs = realloc(allocations, sizeof(void *) * capacity);
      TEST_ASSERT_NOT_NULL(new_allocs);
      allocations = new_allocs;
    }

    void *ptr = buddy_malloc(&pool, user_size);
    if (!ptr) break;

    allocations[count++] = ptr;
  }

  TEST_ASSERT(count > 0); // Ensure we allocated at least one block

  // Free all allocations
  for (size_t i = 0; i < count; i++) {
    buddy_free(&pool, allocations[i]);
  }

  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
  free(allocations);
}

void test_malloc_mixed_sizes(void) {
  struct buddy_pool pool;
  size_t pool_size = UINT64_C(1) << MIN_K;
  buddy_init(&pool, pool_size);

  // Set up array for pointers
  size_t capacity = 128;
  size_t count = 0;
  void **allocations = malloc(sizeof(void *) * capacity);
  size_t *sizes = malloc(sizeof(size_t) * capacity);  // track sizes

  // Define a small set of allocation sizes
  size_t requests[] = {
    8, 24, 32, 64, 100, 128, 200, 400, 512, 800
  };
  size_t num_sizes = sizeof(requests) / sizeof(size_t);

  // Repeatedly allocate random sizes from the list until memory runs out
  while (true) {
    if (count >= capacity) {
      capacity *= 2;
      allocations = realloc(allocations, sizeof(void *) * capacity);
      sizes = realloc(sizes, sizeof(size_t) * capacity);
      TEST_ASSERT_NOT_NULL(allocations);
      TEST_ASSERT_NOT_NULL(sizes);
    }

    size_t size = requests[rand() % num_sizes];
    void *ptr = buddy_malloc(&pool, size);
    if (!ptr) break;

    allocations[count] = ptr;
    sizes[count] = size;
    count++;
  }

  TEST_ASSERT(count > 0); // Allocated at least one block

  // validate the header of each block
  for (size_t i = 0; i < count; i++) {
    struct avail *header = ((struct avail *)allocations[i]) - 1;
    TEST_ASSERT_EQUAL_UINT16(BLOCK_RESERVED, header->tag);
  }

  // Free in reverse order
  for (ssize_t i = (ssize_t)count - 1; i >= 0; i--) {
    buddy_free(&pool, allocations[i]);
  }

  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
  free(allocations);
  free(sizes);
}

int main(void) {
  time_t t;
  unsigned seed = (unsigned)time(&t);
  fprintf(stderr, "Random seed:%d\n", seed);
  srand(seed);
  printf("Running memory tests.\n");

  UNITY_BEGIN();
  RUN_TEST(test_buddy_init);
  RUN_TEST(test_buddy_malloc_one_byte);
  RUN_TEST(test_buddy_malloc_one_large);
  RUN_TEST(test_btok_boundaries);
  RUN_TEST(test_buddy_calc);
  RUN_TEST(test_malloc_minimum_block);
  RUN_TEST(test_malloc_multiple_small_blocks);
  RUN_TEST(test_malloc_mixed_sizes);
return UNITY_END();
}
