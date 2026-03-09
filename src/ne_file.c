#include "ne_file.h"
#include "ne_log.h"

#include <stdio.h>
#include <stdlib.h>

void *ne_file_read(const char *path, size_t *out_size) {
    if (!path) {
        return NULL;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        NE_LOG_ERROR("failed to open file for reading: %s", path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        NE_LOG_ERROR("failed to seek file: %s", path);
        fclose(file);
        return NULL;
    }

    long length = ftell(file);
    if (length < 0) {
        NE_LOG_ERROR("failed to determine file size: %s", path);
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        NE_LOG_ERROR("failed to rewind file: %s", path);
        fclose(file);
        return NULL;
    }

    size_t size = (size_t)length;

    /* Allocate size + 1 so the buffer is always null-terminated. */
    void *data = malloc(size + 1);
    if (!data) {
        NE_LOG_ERROR("failed to allocate %zu bytes for file: %s", size, path);
        fclose(file);
        return NULL;
    }

    size_t read = fread(data, 1, size, file);
    fclose(file);

    if (read != size) {
        NE_LOG_ERROR("partial read (%zu of %zu bytes): %s", read, size, path);
        free(data);
        return NULL;
    }

    ((char *)data)[size] = '\0';

    if (out_size) {
        *out_size = size;
    }

    return data;
}

bool ne_file_write(const char *path, const void *data, size_t size) {
    if (!path || (!data && size > 0)) {
        return false;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        NE_LOG_ERROR("failed to open file for writing: %s", path);
        return false;
    }

    if (size > 0) {
        size_t written = fwrite(data, 1, size, file);
        fclose(file);

        if (written != size) {
            NE_LOG_ERROR("partial write (%zu of %zu bytes): %s", written, size, path);
            return false;
        }
    } else {
        fclose(file);
    }

    return true;
}

void ne_file_free(void *data) {
    free(data);
}
