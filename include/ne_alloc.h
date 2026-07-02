#ifndef NE_ALLOC_H
#define NE_ALLOC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct NEPool {
    void *slots;
    uint32_t count;
    uint32_t cap;
    uint32_t hint;
} NEPool;

uint32_t ne_pool_alloc(NEPool *pool, size_t slot_size);
void     ne_pool_free(NEPool *pool, uint32_t index, size_t slot_size);
void     ne_pool_destroy(NEPool *pool);

#endif //NE_ALLOC_H
