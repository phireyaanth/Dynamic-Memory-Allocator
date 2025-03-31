#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "sfmm.h"

#define ENOMEM 12
#define MIN_BLOCK_SIZE 32

// **Function Prototypes** 
void insert_free_block(sf_block *block);
int get_free_list_index(size_t size);
sf_block *find_free_block(size_t size);
void split_block(sf_block *block, size_t requested_size, size_t size);
void remove_free_block(sf_block *block);
bool insert_into_quick_list(sf_block *block);
void flush_quick_list(int index);
sf_block *coalesce_free_block(sf_block *block);
static inline size_t get_block_size(sf_block *block);
static size_t current_payload = 0;
static size_t peak_payload = 0;
static size_t total_heap_size = 0;



static inline size_t get_block_size(sf_block *block) {
    uint64_t decoded = block->header ^ MAGIC;         // Decode the obfuscated header
    return ((uint32_t)(decoded)) & ~0xF;              // Extract 28-bit block size
}

size_t get_payload_size(sf_block *block) {
    uint64_t decoded = block->header ^ MAGIC;         // Decode the header
    return decoded >> 32;                             // Extract upper 32 bits (payload size)
}

/**
 * Initializes all free list heads (sentinel nodes) to point to themselves.
 * This must be called before inserting any free blocks.
 */
void sf_init() {
    for (int i = 0; i < NUM_FREE_LISTS; i++) {
        sf_free_list_heads[i].body.links.next = &sf_free_list_heads[i];
        sf_free_list_heads[i].body.links.prev = &sf_free_list_heads[i];
    }
    for (int i = 0; i < NUM_QUICK_LISTS; i++) {
        sf_quick_lists[i].length = 0;
        sf_quick_lists[i].first = NULL;
    }
}

/**
 * Flushes all blocks from a quick list at the given index.
 * Adds the blocks to the main free lists after clearing their quick list status.
 * 
 * @param index The index of the quick list to flush
 */
void flush_quick_list(int index) {
    if (index < 0 || index >= NUM_QUICK_LISTS) {
        return;
    }

    sf_block *block = sf_quick_lists[index].first;
    sf_block *next = NULL;

    while (block != NULL) {
        next = block->body.links.next;

        // 1) De-obfuscate
        uint64_t temp = block->header ^ MAGIC;

        // 2) Extract the block size from the low 28 bits
        size_t block_size = ((size_t)temp & 0xFFFFFFFFUL) & ~0xF; 

        // 3) Build a new free-block header: top 32 bits = 0, bottom 28 bits = block_size
        uint64_t new_header = (uint64_t)block_size;  // This zeroes out the top half

        // 4) Obfuscate and store
        block->header = new_header ^ MAGIC;

        // 5) Write the identical footer
        sf_footer *footer = (sf_footer *)((char *)block + block_size - sizeof(sf_footer));
        *footer = block->header;


        sf_block *prev_block = NULL;
        sf_block *next_block = NULL;
        bool prev_free = false;
        bool next_free = false;

        // Check previous block
        if ((char *)block > (char *)sf_mem_start()) {
            sf_footer *prev_footer = (sf_footer *)((char *)block - sizeof(sf_footer));
            if (((void *)prev_footer >= sf_mem_start())) {
                uint64_t prev_footer_val = *prev_footer ^ MAGIC;
                size_t prev_size = (uint32_t)prev_footer_val & ~0xF;
                prev_block = (sf_block *)((char *)block - prev_size);
                prev_free = ((prev_footer_val & THIS_BLOCK_ALLOCATED) == 0);
            }
        }

        // Check next block
        next_block = (sf_block *)((char *)block + block_size);
        if ((char *)next_block < (char *)sf_mem_end()) {
            uint64_t next_header_val = next_block->header ^ MAGIC;
            next_free = ((next_header_val & THIS_BLOCK_ALLOCATED) == 0);
        }

        if (prev_free && next_free) {
            remove_free_block(prev_block);
            remove_free_block(next_block);

            size_t new_size = get_block_size(prev_block) + block_size + get_block_size(next_block);
            uint64_t header = ((uint64_t)0 << 32) | (new_size & ~0xF);
            prev_block->header = header ^ MAGIC;
            sf_footer *new_footer = (sf_footer *)((char *)prev_block + new_size - sizeof(sf_footer));
            *new_footer = header ^ MAGIC;

            insert_free_block(prev_block);
        } else if (prev_free) {
            remove_free_block(prev_block);

            size_t new_size = get_block_size(prev_block) + block_size;
            uint64_t header = ((uint64_t)0 << 32) | (new_size & ~0xF);
            prev_block->header = header ^ MAGIC;
            sf_footer *new_footer = (sf_footer *)((char *)prev_block + new_size - sizeof(sf_footer));
            *new_footer = header ^ MAGIC;

            insert_free_block(prev_block);
        } else if (next_free) {
            remove_free_block(next_block);

            size_t new_size = block_size + get_block_size(next_block);
            uint64_t header = ((uint64_t)0 << 32) | (new_size & ~0xF);
            block->header = header ^ MAGIC;
            sf_footer *new_footer = (sf_footer *)((char *)block + new_size - sizeof(sf_footer));
            *new_footer = header ^ MAGIC;

            insert_free_block(block);
        } else {
            insert_free_block(block);
        }

        block = next;
    }

    sf_quick_lists[index].first = NULL;
    sf_quick_lists[index].length = 0;
}


/**
 * Inserts a block into the appropriate quick list.
 * Returns true if the block was successfully added to a quick list,
 * false if the block was too large for any quick list.
 */
bool insert_into_quick_list(sf_block *block) {
    size_t block_size = get_block_size(block);

    if (block_size <= MIN_BLOCK_SIZE + (NUM_QUICK_LISTS - 1) * 16) {
        int index = (block_size - MIN_BLOCK_SIZE) / 16;

        if (index >= 0 && index < NUM_QUICK_LISTS) {
            if (sf_quick_lists[index].length >= QUICK_LIST_MAX) {
                flush_quick_list(index);
            }

            // Create encoded header with IN_QUICK_LIST and THIS_BLOCK_ALLOCATED bits set, payload size = 0
            uint64_t new_header = ((uint64_t)0 << 32) | block_size | THIS_BLOCK_ALLOCATED | IN_QUICK_LIST;
            block->header = new_header ^ MAGIC;

            // Encode footer the same way
            sf_footer *footer = (sf_footer *)((char *)block + block_size - sizeof(sf_footer));
            *footer = block->header;

            block->body.links.next = sf_quick_lists[index].first;
            sf_quick_lists[index].first = block;
            sf_quick_lists[index].length++;

            return true;
        }
    }

    return false;
}


/**
 * Initializes the heap and sets up the free list.
 */
void create_heap() {
    sf_init();

    if (sf_mem_start() != sf_mem_end()) {
        return;
    }

    // Set magic to 0x0 only for debugging
    //sf_set_magic(0x0);

    void *heap_start = sf_mem_grow();
    if (heap_start == NULL) {
        sf_errno = ENOMEM;
        return;
    }

    total_heap_size += PAGE_SZ;

    int padding_size = 0;
    if ((uintptr_t)sf_mem_start() % 16 == 0) {
        padding_size += 8;
    }

    // Setup prologue block
    sf_block *prologue = (sf_block *)((uintptr_t)sf_mem_start() + padding_size);
    prologue->header = (32 | THIS_BLOCK_ALLOCATED) ^ MAGIC; // obfuscated
    // Write footer for prologue to prevent invalid prev_footer reads
    sf_footer *prologue_footer = (sf_footer *)((char *)prologue + 32 - sizeof(sf_footer));
    *prologue_footer = prologue->header;


    size_t free_block_size = PAGE_SZ - padding_size - 32 - 8;

    // Setup initial free block
    sf_block *first_block = (sf_block *)((char *)prologue + 32);
    first_block->header = ((uint64_t)0 << 32 | (free_block_size & ~0xF)) ^ MAGIC; // obfuscated

    sf_footer *footer = (sf_footer *)((char *)first_block + free_block_size - sizeof(sf_footer));
    *footer = first_block->header; // footer identical, obfuscated

    // Setup epilogue block
    sf_block *epilogue = (sf_block *)((char *)first_block + free_block_size);
    epilogue->header = (0 | THIS_BLOCK_ALLOCATED) ^ MAGIC; // obfuscated

    insert_free_block(first_block);
}


/**
 * Inserts a free block into the correct free list based on size class.
 * */
void insert_free_block(sf_block *block) {
    size_t size = get_block_size(block);

    // === Add this ===
    uint64_t raw_header = ((uint64_t)0 << 32) | (size & ~0xF);
    block->header = raw_header ^ MAGIC;

    sf_footer *footer = (sf_footer *)((char *)block + size - sizeof(sf_footer));
    *footer = raw_header ^ MAGIC;

    int index = get_free_list_index(size);
    sf_block *head = &sf_free_list_heads[index];

    if (block->body.links.next != NULL || block->body.links.prev != NULL) {
        return;
    }

    block->body.links.next = head->body.links.next;
    block->body.links.prev = head;

    if (head->body.links.next != NULL) {
        head->body.links.next->body.links.prev = block;
    }

    head->body.links.next = block;
}


/**
 * Returns the index of the free list based on block size.
 */
int get_free_list_index(size_t size) {
    int index = 0;
    size_t block_size = MIN_BLOCK_SIZE;

    while (index < NUM_FREE_LISTS - 1 && size > block_size) {
        block_size *= 2;
        index++;
    }
    return index;
}


void remove_free_block(sf_block *block) {
    if (block == NULL) {
        return;
    }

    sf_block *prev = block->body.links.prev;
    sf_block *next = block->body.links.next;

    if (prev == NULL || next == NULL || prev->body.links.next != block || next->body.links.prev != block) {
        return;
    }

    prev->body.links.next = next;
    next->body.links.prev = prev;

    block->body.links.next = NULL;
    block->body.links.prev = NULL;
}


/**
 * Searches for a free block in the free lists.
 */
sf_block *find_free_block(size_t size) {
    int index = get_free_list_index(size);

    for (; index < NUM_FREE_LISTS; index++) {
        sf_block *head = &sf_free_list_heads[index];

        for (sf_block *curr = head->body.links.next; curr != head; curr = curr->body.links.next) {
            // Decode header safely
            uint64_t header = curr->header ^ MAGIC;
            size_t block_size = (uint32_t)(header) & ~0xF;

            // Validate block size and free status
            if ((header & THIS_BLOCK_ALLOCATED) != 0 || (block_size < MIN_BLOCK_SIZE)) {
                continue; // skip invalid or allocated blocks
            }

            if (block_size >= size) {
                return curr;
            }
        }
    }

    return NULL;
}



/**
 * Splits a free block if it is larger than the requested size.
 * The leftover portion is reinserted into the free list.
 */
void split_block(sf_block *block, size_t requested_size, size_t payload_size) {
    //printf("DEBUG: ENTERING SPLIT BLOCK\n");
    size_t block_size = get_block_size(block);
    size_t leftover = block_size - requested_size;

    remove_free_block(block);

    if (leftover >= MIN_BLOCK_SIZE) {
        sf_block *new_block = (sf_block *)((char *)block + requested_size);
        new_block->header = (((uint64_t)0 << 32) | (leftover & ~0xF)) ^ MAGIC;

        sf_footer *new_footer = (sf_footer *)((char *)new_block + leftover - sizeof(sf_footer));
        *new_footer = new_block->header;

        insert_free_block(new_block);
    }

    // Create obfuscated header with payload size in top 32 bits
    uint64_t header = ((uint64_t)payload_size << 32) | requested_size | THIS_BLOCK_ALLOCATED;
    block->header = header ^ MAGIC;

    sf_footer *footer = (sf_footer *)((char *)block + requested_size - sizeof(sf_footer));
    *footer = block->header;

    current_payload += payload_size;
    if (current_payload > peak_payload)
        peak_payload = current_payload;

    //printf("[DEBUG MALLOC] The payload pointer shows: %zu bytes\n", payload_size);
    //printf("[DEBUG] THE TOTAL SIZE IS : %zu\n", requested_size);
    //printf("[DEBUG]: LEAVING SPLIT BLOCK\n");
}


void *sf_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    if (sf_mem_start() == sf_mem_end()) {
        create_heap();
    }

    size_t total_size = size + sizeof(sf_header) + sizeof(sf_footer);
    size_t aligned_size = (total_size + 15) & ~15;
    if (aligned_size < 32) {
        aligned_size = 32;
    }

    // FIX #3: Try to use quick list first — validate block before using
    if (aligned_size <= MIN_BLOCK_SIZE + (NUM_QUICK_LISTS - 1) * 16) {
        int quick_list_index = (aligned_size - MIN_BLOCK_SIZE) / 16;
        if (quick_list_index >= 0 && quick_list_index < NUM_QUICK_LISTS) {
            if (sf_quick_lists[quick_list_index].length > 0) {
                sf_block *quick_block = sf_quick_lists[quick_list_index].first;
                sf_quick_lists[quick_list_index].first = quick_block->body.links.next;
                sf_quick_lists[quick_list_index].length--;

                // Validate quick block's header
                uint64_t quick_header = quick_block->header ^ MAGIC;
                if ((quick_header & THIS_BLOCK_ALLOCATED) == 0 || (quick_header & IN_QUICK_LIST) == 0) {
                    abort(); // corrupted quick list block
                }

                // Clear IN_QUICK_LIST bit and re-obfuscate
                quick_header &= ~IN_QUICK_LIST;
                quick_block->header = quick_header ^ MAGIC;

                return (void *)((char *)quick_block + sizeof(sf_header));
            }
        }
    }

    sf_block *block = find_free_block(aligned_size);
    while (block == NULL) {
        void *new_page = sf_mem_grow();
        if (new_page == NULL) {
            sf_errno = ENOMEM;
            return NULL;
        }

        total_heap_size += PAGE_SZ;

        sf_block *old_epilogue = (sf_block *)((char *)sf_mem_end() - PAGE_SZ - sizeof(sf_header));
        sf_block *new_epilogue = (sf_block *)((char *)sf_mem_end() - sizeof(sf_header));
        new_epilogue->header = (0 | THIS_BLOCK_ALLOCATED) ^ MAGIC;  // ✅ Obfuscated epilogue

        size_t new_block_size = PAGE_SZ;

        //  FIX #2: Proper coalescing with previous block using obfuscated footer
        sf_footer *prev_footer = (sf_footer *)((char *)old_epilogue - sizeof(sf_footer));
        if ((void *)prev_footer >= sf_mem_start()) {
            uint64_t footer_val = *prev_footer ^ MAGIC;
            size_t prev_block_size = (uint32_t)(footer_val) & ~0xF;

            sf_block *prev_block = (sf_block *)((char *)old_epilogue - prev_block_size);
            uint64_t prev_header = prev_block->header ^ MAGIC;

            if (footer_val == prev_header) {
                remove_free_block(prev_block);

                size_t combined_size = prev_block_size + new_block_size;
                uint64_t new_header = ((uint64_t)0 << 32) | (combined_size & ~0xF);
                prev_block->header = new_header ^ MAGIC;

                sf_footer *new_footer = (sf_footer *)((char *)prev_block + combined_size - sizeof(sf_footer));
                *new_footer = new_header ^ MAGIC;

                insert_free_block(prev_block);
                block = find_free_block(aligned_size);
                continue;
            }
        }

        // No previous coalesce, just turn old_epilogue into a free block
        old_epilogue->header = (((uint64_t)0 << 32) | (new_block_size & ~0xF)) ^ MAGIC;
        sf_footer *footer = (sf_footer *)((char *)old_epilogue + new_block_size - sizeof(sf_footer));
        *footer = old_epilogue->header;

        insert_free_block(old_epilogue);
        block = find_free_block(aligned_size);
    }

    // Block found, split and allocate
    split_block(block, aligned_size, size);  // Handles obfuscation internally

    // Footer already written inside split_block
    return (void *)((char *)block + sizeof(sf_header));
}



void sf_free(void *ptr) {
    //printf("[ENTERS FREE]\n");
    if (ptr == NULL) return;

    sf_block *block = (sf_block *)((char *)ptr - sizeof(sf_header));
    uint64_t unmasked_header = block->header ^ MAGIC;
    size_t block_size = (uint32_t)(unmasked_header) & ~0xF;

    // Subtract payload from current_payload
    size_t payload_size = unmasked_header >> 32;
    current_payload -= payload_size;

    // Check if eligible for quick list
    if (block_size <= MIN_BLOCK_SIZE + (NUM_QUICK_LISTS - 1) * 16) {
        //printf("QUICKLIST\n");
        int quick_list_index = (block_size - MIN_BLOCK_SIZE) / 16;

        if (quick_list_index >= 0 && quick_list_index < NUM_QUICK_LISTS) {
            if (sf_quick_lists[quick_list_index].length >= QUICK_LIST_MAX) {
                sf_block *curr = sf_quick_lists[quick_list_index].first;
                while (curr != NULL) {
                    sf_block *next = curr->body.links.next;

                    // 1) De-obfuscate the header
                    uint64_t temp = curr->header ^ MAGIC;
                    // 2) Extract block size from the bottom 32 bits
                    size_t block_size = (temp & ~0xF);

                    // 3) Build a new free-block header (top 32 bits=0, bottom 28 bits=block_size)
                    uint64_t new_header = (uint64_t)block_size; // Wipes out old payload bits
                    curr->header = new_header ^ MAGIC;

                    // 4) Write the identical footer
                    sf_footer *footer = (sf_footer*)((char*)curr + block_size - sizeof(sf_footer));
                    *footer = curr->header;

                    // 5) Now coalesce + insert into main free list
                    coalesce_free_block(curr);

                    curr = next;
                }

                // Finally, reset the quick list
                sf_quick_lists[quick_list_index].first = NULL;
                sf_quick_lists[quick_list_index].length = 0;
            }


            // Store obfuscated header and footer
            uint64_t new_header = (((uint64_t)0 << 32) | block_size | THIS_BLOCK_ALLOCATED | IN_QUICK_LIST) ^ MAGIC;
            block->header = new_header;

            sf_footer *footer = (sf_footer *)((char *)block + block_size - sizeof(sf_footer));
            *footer = new_header;

            block->body.links.next = sf_quick_lists[quick_list_index].first;
            sf_quick_lists[quick_list_index].first = block;
            sf_quick_lists[quick_list_index].length++;
            return;
        }
    }
    //printf("Not small enought for quicklist\n");

    // Coalescing logic
    sf_footer *prev_footer = (sf_footer *)((char *)block - sizeof(sf_footer));
    //printf("AFter getting SF FOOTER\n");
    bool prev_free = false;
    //printf("Bool free\n");
    size_t prev_size = 0;
    //printf("after the size\n");
    sf_block *prev_block = NULL;
    //printf("After prevblock null shii\n");

    if ((void *)prev_footer >= (sf_mem_start() + 8)) {
        uint64_t unmasked_footer = *prev_footer ^ MAGIC;
        prev_size = (uint32_t)(unmasked_footer) & ~0xF;
        prev_block = (sf_block *)((char *)block - prev_size);
        prev_free = ((unmasked_footer & THIS_BLOCK_ALLOCATED) == 0);
    }


    sf_block *next_block = (sf_block *)((char *)block + block_size);
    uint64_t next_header_unmasked = next_block->header ^ MAGIC;
    size_t next_size = (uint32_t)(next_header_unmasked) & ~0xF;
    bool next_free = ((char *)next_block < (char *)sf_mem_end()) &&
                     ((next_header_unmasked & THIS_BLOCK_ALLOCATED) == 0);

    if (prev_free && next_free) {
        //printf("prev and next if\n");
        remove_free_block(prev_block);
        remove_free_block(next_block);

        size_t new_size = prev_size + block_size + next_size;
        uint64_t new_header = (((uint64_t)0 << 32) | new_size) ^ MAGIC;

        prev_block->header = new_header;
        sf_footer *new_footer = (sf_footer *)((char *)prev_block + new_size - sizeof(sf_footer));
        *new_footer = new_header;

        insert_free_block(prev_block);
    } else if (prev_free) {
                //printf("prev if\n");

        remove_free_block(prev_block);

        size_t new_size = prev_size + block_size;
        uint64_t new_header = (((uint64_t)0 << 32) | new_size) ^ MAGIC;

        prev_block->header = new_header;
        sf_footer *new_footer = (sf_footer *)((char *)prev_block + new_size - sizeof(sf_footer));
        *new_footer = new_header;

        insert_free_block(prev_block);
    } else if (next_free) {
                //printf("next if\n");

        remove_free_block(next_block);

        size_t new_size = block_size + next_size;
        uint64_t new_header = (((uint64_t)0 << 32) | new_size) ^ MAGIC;

        block->header = new_header;
        sf_footer *new_footer = (sf_footer *)((char *)block + new_size - sizeof(sf_footer));
        *new_footer = new_header;

        insert_free_block(block);
    } else {
                //printf("NONE\n");
        // No coalescing
        uint64_t new_header = (((uint64_t)0 << 32) | block_size) ^ MAGIC;

        block->header = new_header;
        sf_footer *footer = (sf_footer *)((char *)block + block_size - sizeof(sf_footer));
        *footer = new_header;

        insert_free_block(block);
    }
    //printf("Leaving FREE\n");
}


void *sf_realloc(void *pp, size_t rsize) {
    if (pp == NULL) {
        return sf_malloc(rsize);
    }

    if (rsize == 0) {
        sf_free(pp);
        return NULL;
    }

    sf_block *current_block = (sf_block *)((char *)pp - sizeof(sf_header));
    uint64_t unmasked_header = current_block->header ^ MAGIC;
    size_t current_block_size = (uint32_t)(unmasked_header) & ~0xF;

    size_t total_size = rsize + sizeof(sf_header) + sizeof(sf_footer);
    size_t aligned_size = (total_size + 15) & ~15;
    if (aligned_size < MIN_BLOCK_SIZE) {
        aligned_size = MIN_BLOCK_SIZE;
    }

    if (aligned_size > current_block_size) {
        void *new_ptr = sf_malloc(rsize);
        if (new_ptr == NULL) {
            return NULL;
        }

        size_t old_payload_size = unmasked_header >> 32;
        size_t copy_size = (rsize < old_payload_size) ? rsize : old_payload_size;
        memcpy(new_ptr, pp, copy_size);

        current_payload += rsize;

        sf_free(pp);
        return new_ptr;
    }

    // New size fits, maybe split
    size_t leftover = current_block_size - aligned_size;

    if (leftover < MIN_BLOCK_SIZE) {
        // No split, just adjust payload size in header
        size_t old_payload_size = unmasked_header >> 32;
        current_payload -= old_payload_size;
        current_payload += rsize;

        uint64_t new_header = ((uint64_t)rsize << 32) | current_block_size | THIS_BLOCK_ALLOCATED;
        current_block->header = new_header ^ MAGIC;

        sf_footer *footer = (sf_footer *)((char *)current_block + current_block_size - sizeof(sf_footer));
        *footer = current_block->header;

        return pp;
    }

    // We can split
    size_t old_payload_size = unmasked_header >> 32;
    current_payload -= old_payload_size;
    current_payload += rsize;

    // Allocated block header
    uint64_t new_header = ((uint64_t)rsize << 32) | aligned_size | THIS_BLOCK_ALLOCATED;
    current_block->header = new_header ^ MAGIC;

    sf_footer *allocated_footer = (sf_footer *)((char *)current_block + aligned_size - sizeof(sf_footer));
    *allocated_footer = current_block->header;

    // Free split block
    sf_block *new_free_block = (sf_block *)((char *)current_block + aligned_size);
    size_t new_free_size = current_block_size - aligned_size;

    uint64_t free_header = ((uint64_t)0 << 32) | (new_free_size & ~0xF);
    new_free_block->header = free_header ^ MAGIC;

    sf_footer *new_free_footer = (sf_footer *)((char *)new_free_block + new_free_size - sizeof(sf_footer));
    *new_free_footer = free_header ^ MAGIC;  //  Make footer match header exactly

    insert_free_block(new_free_block);
    coalesce_free_block(new_free_block);

    return pp;
}

double sf_fragmentation() {
    size_t total_payload = 0;
    size_t total_allocated = 0;

    char *heap_ptr = (char *)sf_mem_start() + 8; // skip padding
    char *heap_end = (char *)sf_mem_end();

    while (heap_ptr + sizeof(sf_header) < heap_end) {
        sf_block *block = (sf_block *)heap_ptr;

        // Decode obfuscated header
        uint64_t header = block->header ^ MAGIC;
        size_t block_size = (uint32_t)(header) & ~0xF;

        // Defensive: break if invalid block size or overflows
        if (block_size == 0 || heap_ptr + block_size > heap_end) {
            break;
        }

        if (header & THIS_BLOCK_ALLOCATED) {
            total_payload += header >> 32;
            total_allocated += block_size;
        }

        heap_ptr += block_size;
    }

    if (total_allocated == 0){
        return 0.0;
        //printf("[TOTAL ALLOC IS 0]\n");
    }
    // printf("[TOTAL PAY]: %ld\n", total_payload);
    // printf("TOTAL HEAP SIZE: %ld\n", total_allocated);
    // printf("MAGIC:%zu\n", MAGIC);
    return (double)total_payload / total_allocated;
}


double sf_utilization() {
    //printf("DEBUG UTIL: INSIDE UTIL\n");
    if (total_heap_size == 0) {
        printf("INSIDE TOTAL HEAP SIZE == 0\n");
        return 0.0;
    }
    //printf("[DEBUG UTIL]Peak Payload:, %ld\n", peak_payload);
    //printf("[DEBUG UTIL]Total payload:, %ld\n", total_heap_size);
  

    return (double)peak_payload / total_heap_size;
}


sf_block *coalesce_free_block(sf_block *block) {
    size_t size = get_block_size(block);

    // Check previous block
    sf_footer *prev_footer = (sf_footer *)((char *)block - sizeof(sf_footer));
    bool prev_free = false;
    sf_block *prev_block = NULL;

    if ((void *)prev_footer >= sf_mem_start()) {
        uint64_t footer_val = *prev_footer ^ MAGIC;
        size_t prev_size = footer_val & ~0xF;
        prev_block = (sf_block *)((char *)block - prev_size);
        prev_free = ((footer_val & THIS_BLOCK_ALLOCATED) == 0);
    }

    // Check next block
    sf_block *next_block = (sf_block *)((char *)block + size);
    bool next_free = false;
    size_t next_size = 0;

    if ((char *)next_block < (char *)sf_mem_end()) {
        uint64_t next_header_val = next_block->header ^ MAGIC;
        next_size = next_header_val & ~0xF;
        next_free = ((next_header_val & THIS_BLOCK_ALLOCATED) == 0);
    }

    // Coalescing cases
    if (prev_free && next_free) {
        remove_free_block(prev_block);
        remove_free_block(next_block);

        size_t combined_size = get_block_size(prev_block) + size + next_size;
        uint64_t new_header = ((uint64_t)0 << 32) | (combined_size & ~0xF);
        prev_block->header = new_header ^ MAGIC;

        sf_footer *new_footer = (sf_footer *)((char *)prev_block + combined_size - sizeof(sf_footer));
        *new_footer = new_header ^ MAGIC;

        insert_free_block(prev_block);
        return prev_block;

    } else if (prev_free) {
        remove_free_block(prev_block);

        size_t combined_size = get_block_size(prev_block) + size;
        uint64_t new_header = ((uint64_t)0 << 32) | (combined_size & ~0xF);
        prev_block->header = new_header ^ MAGIC;

        sf_footer *new_footer = (sf_footer *)((char *)prev_block + combined_size - sizeof(sf_footer));
        *new_footer = new_header ^ MAGIC;

        insert_free_block(prev_block);
        return prev_block;

    } else if (next_free) {
        remove_free_block(next_block);

        size_t combined_size = size + next_size;
        uint64_t new_header = ((uint64_t)0 << 32) | (combined_size & ~0xF);
        block->header = new_header ^ MAGIC;

        sf_footer *new_footer = (sf_footer *)((char *)block + combined_size - sizeof(sf_footer));
        *new_footer = new_header ^ MAGIC;

        insert_free_block(block);
        return block;

    } else {
        uint64_t new_header = ((uint64_t)0 << 32) | (size & ~0xF);
        block->header = new_header ^ MAGIC;

        sf_footer *footer = (sf_footer *)((char *)block + size - sizeof(sf_footer));
        *footer = new_header ^ MAGIC;

        insert_free_block(block);
        return block;
    }
}

