/**
 * @file ne_alloc.c
 * @brief Generic slot pool allocator implementation.
 *
 * DESIGN
 * ------
 * The pool is a contiguous array of fixed-size slots. Every slot struct must
 * have `bool occupied` as its very first field — the pool reads/writes this
 * byte to track which slots are in use without knowing the slot's full type.
 *
 * MEMORY LAYOUT
 * -------------
 *   pool->slots
 *   |
 *   v
 *   +-----------+-----------+-----------+-----------+-----
 *   | slot 0    | slot 1    | slot 2    | slot 3    | ...
 *   |[occ|data] |[occ|data] |[occ|data] |[occ|data] |
 *   +-----------+-----------+-----------+-----------+-----
 *   <--slot_size-->
 *
 *   occ = bool occupied (1 byte at offset 0 of each slot)
 *
 * ALLOCATION STRATEGY
 * -------------------
 * A `hint` index tracks where to start searching for a free slot.
 * After each allocation, hint advances past the found slot. After
 * each free, hint rewinds to the freed index if it's lower.
 *
 *   hint
 *   v
 *   [used] [used] [FREE] [used] [used] [FREE] [used]
 *                   ^
 *                   found here, returned
 *
 * This gives O(1) allocation in the common case (sequential fills)
 * and O(n) worst case (heavily fragmented pool with no free slots
 * before the hint — requires a wrap-around scan).
 *
 * GROWTH
 * ------
 * When no free slot exists, the backing array doubles in size (starting
 * from 16 slots). New slots are zero-initialized, so `occupied` starts
 * as false. The first new slot is returned immediately.
 */

#include "ne_alloc.h"

#include <stdlib.h>
#include <string.h>

#define NE_POOL_INITIAL_CAP 16

/**
 * @brief Allocate a slot from the pool.
 *
 * Search order:
 *   1. Scan forward from hint to end of array.
 *   2. Wrap around and scan from 0 to hint.
 *   3. If nothing found, grow the array and use the first new slot.
 */
uint32_t ne_pool_alloc(NEPool *pool, size_t slot_size) {
    uint8_t *slots = (uint8_t *)pool->slots;

    for (uint32_t i = pool->hint; i < pool->cap; i++) {
        bool *occupied = (bool *)(slots + i * slot_size);
        if (!*occupied) {
            *occupied = true;
            pool->count++;
            pool->hint = i + 1;
            return i;
        }
    }

    for (uint32_t i = 0; i < pool->hint; i++) {
        bool *occupied = (bool *)(slots + i * slot_size);
        if (!*occupied) {
            *occupied = true;
            pool->count++;
            pool->hint = i + 1;
            return i;
        }
    }

    uint32_t new_cap = pool->cap == 0 ? NE_POOL_INITIAL_CAP : pool->cap * 2;
    void *new_slots = realloc(pool->slots, new_cap * slot_size);
    if (!new_slots) {
        return UINT32_MAX;
    }

    memset((uint8_t *)new_slots + pool->cap * slot_size, 0, (new_cap - pool->cap) * slot_size);

    uint32_t index = pool->cap;
    pool->slots = new_slots;
    pool->cap = new_cap;
    pool->count++;
    pool->hint = index + 1;

    *(bool *)((uint8_t *)new_slots + index * slot_size) = true;
    return index;
}

/**
 * @brief Release a slot back to the pool for reuse.
 *
 * Clears the occupied flag and rewinds the hint if the freed slot
 * is before the current hint (so the next alloc finds it quickly).
 */
void ne_pool_free(NEPool *pool, uint32_t index, size_t slot_size) {
    if (index >= pool->cap) {
        return;
    }
    bool *occupied = (bool *)((uint8_t *)pool->slots + index * slot_size);
    if (!*occupied) {
        return;
    }
    *occupied = false;
    if (pool->count > 0) {
        pool->count--;
    }
    if (index < pool->hint) {
        pool->hint = index;
    }
}

/**
 * @brief Free all backing memory. The pool is left in a zero state.
 */
void ne_pool_destroy(NEPool *pool) {
    free(pool->slots);
    pool->slots = NULL;
    pool->count = 0;
    pool->cap = 0;
    pool->hint = 0;
}
