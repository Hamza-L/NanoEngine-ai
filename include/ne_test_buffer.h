#ifndef NE_TEST_BUFFER_H
#define NE_TEST_BUFFER_H

#include <stdbool.h>

typedef struct NEApp NEApp;
typedef struct NEWindow NEWindow;

/**
 * Run the GPU buffer test suite.
 *
 * Tests:
 * - Buffer creation and destruction
 * - Buffer creation with initial data
 * - Buffer updates with partial writes
 * - Buffer usage flags (VERTEX, INDEX, UNIFORM, STORAGE)
 *
 * Parameters:
 * - `app`: Application instance
 * - `window`: Window for rendering surface
 *
 * Returns:
 * - true if all tests pass
 * - false if any test fails
 */
bool test_buffer(NEApp *app, NEWindow *window);

#endif
