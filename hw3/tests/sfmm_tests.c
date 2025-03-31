#include <criterion/criterion.h>
#include <errno.h>
#include <signal.h>
#include "debug.h"
#include "sfmm.h"
#define TEST_TIMEOUT 15

/*
 * Assert the total number of free blocks of a specified size.
 * If size == 0, then assert the total number of all free blocks.
 */
void assert_free_block_count(size_t size, int count) {
    int cnt = 0;
    for(int i = 0; i < NUM_FREE_LISTS; i++) {
        sf_block *bp = sf_free_list_heads[i].body.links.next;
        while(bp != &sf_free_list_heads[i]) {
	    if(size == 0 || size == ((bp->header ^ sf_magic()) & ~0xffffffff0000000f))
	        cnt++;
	    bp = bp->body.links.next;
	}
    }
    if(size == 0) {
	cr_assert_eq(cnt, count, "Wrong number of free blocks (exp=%d, found=%d)",
		     count, cnt);
    } else {
	cr_assert_eq(cnt, count, "Wrong number of free blocks of size %ld (exp=%d, found=%d)",
		     size, count, cnt);
    }
}

/*
 * Assert the total number of quick list blocks of a specified size.
 * If size == 0, then assert the total number of all quick list blocks.
 */
void assert_quick_list_block_count(size_t size, int count) {
    int cnt = 0;
    for(int i = 0; i < NUM_QUICK_LISTS; i++) {
	sf_block *bp = sf_quick_lists[i].first;
	while(bp != NULL) {
	    if(size == 0 || size == ((bp->header ^ sf_magic()) & ~0xffffffff0000000f))
		cnt++;
	    bp = bp->body.links.next;
	}
    }
    if(size == 0) {
	cr_assert_eq(cnt, count, "Wrong number of quick list blocks (exp=%d, found=%d)",
		     count, cnt);
    } else {
	cr_assert_eq(cnt, count, "Wrong number of quick list blocks of size %ld (exp=%d, found=%d)",
		     size, count, cnt);
    }
}

Test(sfmm_basecode_suite, malloc_an_int, .timeout = TEST_TIMEOUT) {
	sf_errno = 0;
	size_t sz = sizeof(int);
	//printf("[TEST CASES] right before MALLOC\n");
	int *x = sf_malloc(sz);
	//printf("[TEST CASES] GOT PASSED THE MALLOC\n");

	cr_assert_not_null(x, "x is NULL!");

	*x = 4;

	cr_assert(*x == 4, "sf_malloc failed to give proper space for an int!");

	assert_quick_list_block_count(0, 0);
	assert_free_block_count(0, 1);
	assert_free_block_count(4016, 1);

	cr_assert(sf_errno == 0, "sf_errno is not zero!");
	cr_assert(sf_mem_start() + PAGE_SZ == sf_mem_end(), "Allocated more than necessary!");
}


Test(sfmm_basecode_suite, malloc_four_pages, .timeout = TEST_TIMEOUT) {
	sf_errno = 0;

	// We want to allocate up to exactly four pages, so there has to be space
	// for the header and the link pointers.
	void *x = sf_malloc(16316);
	cr_assert_not_null(x, "x is NULL!");
	assert_quick_list_block_count(0, 0);
	assert_free_block_count(0, 0);
	cr_assert(sf_errno == 0, "sf_errno is not 0!");
}

Test(sfmm_basecode_suite, malloc_too_large, .timeout = TEST_TIMEOUT) {
	sf_errno = 0;
	void *x = sf_malloc(151505);

	cr_assert_null(x, "x is not NULL!");
	assert_quick_list_block_count(0, 0);
	assert_free_block_count(0, 1);
	assert_free_block_count(151504, 1);
	cr_assert(sf_errno == ENOMEM, "sf_errno is not ENOMEM!");
}

Test(sfmm_basecode_suite, free_quick, .timeout = TEST_TIMEOUT) {
	sf_errno = 0;
	size_t sz_x = 8, sz_y = 32, sz_z = 1;
	/* void *x = */ sf_malloc(sz_x);
	void *y = sf_malloc(sz_y);
	/* void *z = */ sf_malloc(sz_z);

	sf_free(y);

	assert_quick_list_block_count(0, 1);
	assert_quick_list_block_count(48, 1);
	assert_free_block_count(0, 1);
	assert_free_block_count(3936, 1);
	cr_assert(sf_errno == 0, "sf_errno is not zero!");
}

Test(sfmm_basecode_suite, free_no_coalesce, .timeout = TEST_TIMEOUT) {
	sf_errno = 0;
	size_t sz_x = 8, sz_y = 200, sz_z = 1;
	/* void *x = */ sf_malloc(sz_x);
	void *y = sf_malloc(sz_y);
	/* void *z = */ sf_malloc(sz_z);

	sf_free(y);

	assert_quick_list_block_count(0, 0);
	assert_free_block_count(0, 2);
	assert_free_block_count(224, 1);
	assert_free_block_count(3760, 1);

	cr_assert(sf_errno == 0, "sf_errno is not zero!");
}

Test(sfmm_basecode_suite, free_coalesce, .timeout = TEST_TIMEOUT) {
	sf_errno = 0;
	size_t sz_w = 8, sz_x = 200, sz_y = 300, sz_z = 4;
	/* void *w = */ sf_malloc(sz_w);
	void *x = sf_malloc(sz_x);
	void *y = sf_malloc(sz_y);
	/* void *z = */ sf_malloc(sz_z);

	sf_free(y);
	sf_free(x);

	assert_quick_list_block_count(0, 0);
	assert_free_block_count(0, 2);
	assert_free_block_count(544, 1);
	assert_free_block_count(3440, 1);

	cr_assert(sf_errno == 0, "sf_errno is not zero!");
}

Test(sfmm_basecode_suite, freelist, .timeout = TEST_TIMEOUT) {
        size_t sz_u = 200, sz_v = 300, sz_w = 200, sz_x = 500, sz_y = 200, sz_z = 700;
	void *u = sf_malloc(sz_u);
	/* void *v = */ sf_malloc(sz_v);
	void *w = sf_malloc(sz_w);
	/* void *x = */ sf_malloc(sz_x);
	void *y = sf_malloc(sz_y);
	/* void *z = */ sf_malloc(sz_z);

	sf_free(u);
	sf_free(w);
	sf_free(y);

	assert_quick_list_block_count(0, 0);
	assert_free_block_count(0, 4);
	assert_free_block_count(224, 3);
	assert_free_block_count(1808, 1);

	// First block in list should be the most recently freed block.
	int i = 3;
	sf_block *bp = sf_free_list_heads[i].body.links.next;
	cr_assert_eq(bp, (char *)y - 8,
		     "Wrong first block in free list %d: (found=%p, exp=%p)",
                     i, bp, (char *)y - 8);
}

Test(sfmm_basecode_suite, realloc_larger_block, .timeout = TEST_TIMEOUT) {
        size_t sz_x = sizeof(int), sz_y = 10, sz_x1 = sizeof(int) * 20;
	void *x = sf_malloc(sz_x);
	/* void *y = */ sf_malloc(sz_y);
	x = sf_realloc(x, sz_x1);

	cr_assert_not_null(x, "x is NULL!");
	sf_block *bp = (sf_block *)((char *)x - 8);
	cr_assert((bp->header ^ sf_magic()) & 0x1, "Allocated bit is not set!");
	cr_assert(((bp->header ^ sf_magic()) & ~0xffffffff0000000f) == 96,
		  "Realloc'ed block size (%ld) not what was expected (%ld)!",
		  (bp->header ^ sf_magic()) & ~0xffffffff0000000f, 96);

	assert_quick_list_block_count(0, 1);
	assert_quick_list_block_count(32, 1);
	assert_free_block_count(0, 1);
	assert_free_block_count(3888, 1);
}

Test(sfmm_basecode_suite, realloc_smaller_block_splinter, .timeout = TEST_TIMEOUT) {
        size_t sz_x = sizeof(int) * 20, sz_y = sizeof(int) * 16;
	void *x = sf_malloc(sz_x);
	void *y = sf_realloc(x, sz_y);

	cr_assert_not_null(y, "y is NULL!");
	cr_assert(x == y, "Payload addresses are different!");

	sf_block *bp = (sf_block *)((char *)x - 8);
	cr_assert((bp->header ^ sf_magic()) & 0x1, "Allocated bit is not set!");
	cr_assert(((bp->header ^ sf_magic()) & ~0xffffffff0000000f) == 96,
		  "Realloc'ed block size (%ld) not what was expected (%ld)!",
		  (bp->header ^ sf_magic()) & ~0xffffffff0000000f, 96);

	// There should be only one free block.
	assert_quick_list_block_count(0, 0);
	assert_free_block_count(0, 1);
	assert_free_block_count(3952, 1);
}

Test(sfmm_basecode_suite, realloc_smaller_block_free_block, .timeout = TEST_TIMEOUT) {
        size_t sz_x = sizeof(double) * 8, sz_y = sizeof(int);
	void *x = sf_malloc(sz_x);
	void *y = sf_realloc(x, sz_y);

	cr_assert_not_null(y, "y is NULL!");

	sf_block *bp = (sf_block *)((char *)x - 8);
	cr_assert((bp->header ^ sf_magic()) & 0x1, "Allocated bit is not set!");
	cr_assert(((bp->header ^ sf_magic()) & ~0xffffffff0000000f) == 32,
		  "Realloc'ed block size (%ld) not what was expected (%ld)!",
		  (bp->header ^ sf_magic()) & ~0xffffffff0000000f, 32);

	// After realloc'ing x, we can return a block of size ADJUSTED_BLOCK_SIZE(sz_x) - ADJUSTED_BLOCK_SIZE(sz_y)
	// to the freelist.  This block will go into the main freelist and be coalesced.
	// Note that we don't put split blocks into the quick lists because their sizes are not sizes
	// that were requested by the client, so they are not very likely to satisfy a new request.
	assert_quick_list_block_count(0, 0);	
	assert_free_block_count(0, 1);
	assert_free_block_count(4016, 1);
}

//############################################
//STUDENT UNIT TESTS SHOULD BE WRITTEN BELOW
//DO NOT DELETE OR MANGLE THESE COMMENTS
//############################################

//Test(sfmm_student_suite, student_test_1, .timeout = TEST_TIMEOUT) {
//}


Test(sfmm_student_suite, student_test_1, .timeout = TEST_TIMEOUT) {
    // Reset errno
    sf_errno = 0;

    // Allocate three blocks of different sizes
    void *p1 = sf_malloc(32);  // Will likely round to 48 or 64
    void *p2 = sf_malloc(100); // Will round to something like 128
    void *p3 = sf_malloc(200); // Will round to ~224

    cr_assert_not_null(p1, "First malloc failed!");
    cr_assert_not_null(p2, "Second malloc failed!");
    cr_assert_not_null(p3, "Third malloc failed!");

    // Calculate expected values based on rounding and header/footer structure
    size_t total_payload = 32 + 100 + 200;
    size_t total_block_size = 0;

    // Walk through heap to compute actual block sizes
    char *heap_ptr = (char *)sf_mem_start() + 8;
    char *heap_end = (char *)sf_mem_end();

    while (heap_ptr + sizeof(sf_header) < heap_end) {
        sf_block *block = (sf_block *)heap_ptr;
		uint64_t header = block->header ^ MAGIC;
		size_t size = (uint32_t)(header) & ~0xF;


        if (size == 0) break;

        if (header & THIS_BLOCK_ALLOCATED) {
            total_block_size += size;
        }

        heap_ptr += size;
    }

    double expected = (double)total_payload / total_block_size;
    double actual = sf_fragmentation();

    // //printf("[DEBUG] Total Payload: %zu\n", total_payload);
    // printf("[DEBUG] Total Block Size: %zu\n", total_block_size);
    // printf("[DEBUG] Expected Fragmentation: %f\n", expected);
    // printf("[DEBUG] Actual Fragmentation:   %f\n", actual);

    cr_assert_float_eq(actual, expected, 0.01, "Fragmentation calculation is off!");
}


Test(sfmm_student_suite, student_test_2, .timeout = TEST_TIMEOUT) {
    sf_errno = 0;
    size_t sz = 200;

    // First malloc
    void *ptr1 = sf_malloc(sz);
    cr_assert_not_null(ptr1, "First malloc(200) returned NULL!");

    // Free the block
    sf_free(ptr1);

    // Second malloc of same size
    void *ptr2 = sf_malloc(sz);
    cr_assert_not_null(ptr2, "Second malloc(200) returned NULL!");

    cr_assert(sf_errno == 0, "sf_errno is not zero after second malloc!");
    cr_assert(sf_mem_start() + PAGE_SZ == sf_mem_end(), "Allocator used more than one page!");
	//sf_show_heap();
}

Test(sfmm_student_suite, student_test_3, .timeout = TEST_TIMEOUT) {
    sf_errno = 0;

    size_t sz1 = 100;
    size_t sz2 = 200;
    size_t sz3 = 300;

    void *p1 = sf_malloc(sz1);
    cr_assert_not_null(p1, "sf_malloc(sz1) failed!");

    void *p2 = sf_malloc(sz2);
    cr_assert_not_null(p2, "sf_malloc(sz2) failed!");

    void *p3 = sf_malloc(sz3);
    cr_assert_not_null(p3, "sf_malloc(sz3) failed!");

    // Now check utilization
    double utilization = sf_utilization();
    cr_assert(utilization > 0.0, "sf_utilization() returned 0.0 unexpectedly!");
    cr_assert(utilization <= 1.0, "sf_utilization() should never exceed 1.0!");

    //printf("[TEST UTILIZATION] Utilization = %.6f\n", utilization);
}

Test(sfmm_student_suite, student_test_4_quicklist_flush, .timeout = TEST_TIMEOUT) {
    sf_errno = 0;
    void *ptrs[QUICK_LIST_MAX + 1]; // Allocate one more than quick list capacity

    // Allocate QUICK_LIST_MAX + 1 blocks of size 32 (rounds up to 48)
    for (int i = 0; i <= QUICK_LIST_MAX; i++) {
        ptrs[i] = sf_malloc(32);
        cr_assert_not_null(ptrs[i], "sf_malloc failed at index %d", i);
    }

    // Free them to fill the quick list and trigger flush on the last one
    for (int i = 0; i <= QUICK_LIST_MAX; i++) {
        sf_free(ptrs[i]);
    }

    // Quick list should now contain only the last freed block
    assert_quick_list_block_count(0, 1);
    assert_quick_list_block_count(48, 1);

    // Flushed blocks coalesced into one large block in the free list
    assert_free_block_count(0, 2);           // 1 big coalesced + remaining large block
    assert_free_block_count(240, 1);         // 5 * 48 = 240 from flushed/coalesced blocks
}


Test(sfmm_student_suite, student_test_5_coalesce_prev, .timeout = TEST_TIMEOUT) {
    sf_errno = 0;
    void *a = sf_malloc(200);
    void *b = sf_malloc(200);
	sf_malloc(4);
    sf_free(a);  // A becomes free
    sf_free(b);  // Should coalesce with A

    assert_free_block_count(0, 2);
    assert_free_block_count(224 + 224, 1); // Combined size of both
	//sf_show_heap();
}

Test(sfmm_student_suite, student_test_6_coalesce_next, .timeout = TEST_TIMEOUT) {
    sf_errno = 0;
    void *a = sf_malloc(300);
	void *b = sf_malloc(300);
	void *c = sf_malloc(300);
	(void)c; // Prevent unused variable warning

	sf_free(b);
	sf_free(a);

	// Expect coalescing between a and b
	assert_free_block_count(0, 2);
	assert_free_block_count(640, 1); // Adjust size if needed

}

Test(sfmm_student_suite, student_test_7_realloc_splinter, .timeout = TEST_TIMEOUT) {
    sf_errno = 0;
    void *a = sf_malloc(64);
    void *b = sf_realloc(a, 60);

    cr_assert(a == b, "Realloc should not move block for small shrink.");
    assert_quick_list_block_count(0, 0);
    assert_free_block_count(0, 1);  // Only end free block
}

Test(sfmm_student_suite, student_test_8_realloc_split, .timeout = TEST_TIMEOUT) {
    sf_errno = 0;
    void *a = sf_malloc(200);  // Alloc 224
	sf_malloc(4);   
    void *b = sf_realloc(a, 100); // Alloc 128
	//sf_malloc(4);
    cr_assert(a == b, "Realloc did not return same pointer after split.");
    assert_free_block_count(96, 1); // Remaining split block
}

Test(sfmm_student_suite, student_test_9_fragmentation_vs_util, .timeout = TEST_TIMEOUT) {
    void *a = sf_malloc(32);   // Alloc 48
	void *b = sf_malloc(100);  // Alloc 128
	void *c = sf_malloc(200);  // Alloc 224
	(void)a;
	(void)b;
	(void)c;

	// Perform whatever test you're planning here
	double frag = sf_fragmentation();
	double util = sf_utilization();

	cr_assert(frag > 0.0, "Fragmentation unexpectedly 0.");
	cr_assert(util > 0.0, "Utilization unexpectedly 0.");

}

Test(sfmm_student_suite, student_test_10_heap_grow, .timeout = TEST_TIMEOUT) {
    sf_errno = 0;
    void *ptr = sf_malloc(PAGE_SZ * 2); // Force grow

    cr_assert_not_null(ptr, "Malloc failed on large request.");
    cr_assert(sf_mem_end() > sf_mem_start() + PAGE_SZ, "Heap did not grow.");
}
