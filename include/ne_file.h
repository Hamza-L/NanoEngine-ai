#ifndef NE_FILE_H
#define NE_FILE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read an entire file into a heap-allocated buffer.
 *
 * The returned buffer is always null-terminated so it can be used directly as
 * a C string (useful for shader source).  The actual file size (excluding the
 * null terminator) is written to @p out_size when non-NULL.
 *
 * @param path      Absolute or relative file path.
 * @param out_size  Optional.  Receives the file size in bytes.
 * @return A pointer to the file contents, or NULL on failure.
 *         Free with ne_file_free().
 */
void *ne_file_read(const char *path, size_t *out_size);

/**
 * Write a block of data to a file, replacing any existing contents.
 *
 * @param path  Absolute or relative file path.
 * @param data  Pointer to the data to write.
 * @param size  Number of bytes to write.
 * @return true on success, false on failure.
 */
bool ne_file_write(const char *path, const void *data, size_t size);

/**
 * Free a buffer previously returned by ne_file_read().
 */
void ne_file_free(void *data);

#ifdef __cplusplus
}
#endif

#endif
