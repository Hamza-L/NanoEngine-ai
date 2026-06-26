#ifndef NE_ALLOC_H
#define NE_ALLOC_H

#include <stdint.h>
#include <stddef.h>

uint32_t ne_pool_alloc(void **pool_ptr, uint32_t *count_ptr, uint32_t *cap_ptr, size_t slot_size);

#endif //NE_ALLOC_H
