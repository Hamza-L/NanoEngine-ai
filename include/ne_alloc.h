/**
 * @file ne_alloc.h
 * @brief Generic slot pool allocator for handle-backed resources.
 *
 * An NEPool manages a flat array of fixed-size slots. Each slot's first byte
 * is a `bool occupied` flag. The pool hands out integer indices (used as
 * 1-based handles by the renderer) and reuses freed slots efficiently.
 *
 * Usage:
 *   NEPool pool = {0};
 *   uint32_t i = ne_pool_alloc(&pool, sizeof(MySlot));   // allocate
 *   MySlot *s = &((MySlot*)pool.slots)[i];               // access
 *   ne_pool_free(&pool, i, sizeof(MySlot));              // release
 *   ne_pool_destroy(&pool);                              // teardown
 */
#ifndef NE_ALLOC_H
#define NE_ALLOC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief A pool of fixed-size slots identified by index.
 *
 * Zero-initialize to start empty: `NEPool pool = {0};`
 */
typedef struct NEPool {
    void *slots;      /**< Flat array of slot_size-byte elements. */
    uint32_t count;   /**< Number of currently occupied slots. */
    uint32_t cap;     /**< Total allocated slot capacity. */
    uint32_t hint;    /**< Search start index for the next allocation. */
} NEPool;

/**
 * @brief Allocate a slot from the pool.
 *
 * Finds the first unoccupied slot (starting from an internal hint),
 * marks it occupied, and returns its index. Grows the pool if full.
 *
 * @param pool       Pool instance (must not be NULL).
 * @param slot_size  Size in bytes of each slot (e.g. sizeof(MySlot)).
 * @return           Slot index on success, UINT32_MAX on allocation failure.
 */
uint32_t ne_pool_alloc(NEPool *pool, size_t slot_size);

/**
 * @brief Release a slot back to the pool.
 *
 * Marks the slot as unoccupied so it can be reused by future allocations.
 *
 * @param pool       Pool instance.
 * @param index      Slot index previously returned by ne_pool_alloc.
 * @param slot_size  Size in bytes of each slot.
 */
void ne_pool_free(NEPool *pool, uint32_t index, size_t slot_size);

/**
 * @brief Destroy the pool and free all backing memory.
 *
 * Does not call destructors on individual slots — the caller must
 * clean up slot contents before calling this.
 *
 * @param pool  Pool instance.
 */
void ne_pool_destroy(NEPool *pool);

#endif //NE_ALLOC_H
