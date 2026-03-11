#include "ne_alloc.h"

#include <stdlib.h>
#include <string.h>

#define NE_VK_POOL_INITIAL_CAP 16

/**
 * Generic pool allocation helper.
 * Works for any slot type whose first field is `bool occupied`.
 * Returns the slot index, or UINT32_MAX on failure.
 */
uint32_t ne_pool_alloc(void **pool_ptr, uint32_t *count_ptr, uint32_t *cap_ptr, size_t slot_size) {
    uint8_t *pool = (uint8_t *)*pool_ptr;
    uint32_t cap = *cap_ptr;

    /* Search for free slot */
    for (uint32_t i = 0; i < cap; i++) {
        bool *occupied = (bool *)(pool + i * slot_size);
        if (!*occupied) {
            return i;
        }
    }

    /* No free slot; grow pool */
    uint32_t new_cap = cap == 0 ? NE_VK_POOL_INITIAL_CAP : cap * 2;
    void *new_pool = realloc(*pool_ptr, new_cap * slot_size);
    if (!new_pool) {
        return UINT32_MAX;
    }

    /* Zero-initialize new slots */
    memset((uint8_t *)new_pool + cap * slot_size, 0, (new_cap - cap) * slot_size);

    uint32_t index = cap;
    *pool_ptr = new_pool;
    *cap_ptr = new_cap;
    *count_ptr = *count_ptr + 1;

    return index;
}
