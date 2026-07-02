#include "ne_alloc.h"

#include <stdlib.h>
#include <string.h>

#define NE_POOL_INITIAL_CAP 16

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

void ne_pool_destroy(NEPool *pool) {
    free(pool->slots);
    pool->slots = NULL;
    pool->count = 0;
    pool->cap = 0;
    pool->hint = 0;
}
