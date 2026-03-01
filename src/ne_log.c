#include "ne_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct NELogger {
    NELogLevel level;
    NELogSinkFn sink;
    void *sink_user_data;

    /* If non-NULL, this file will be closed when output changes or logger is destroyed. */
    FILE *owned_file;
};

static void ne_log_sink_console(void *user_data, NELogLevel level, const char *message) {
    (void)user_data;

    FILE *out = stdout;
    if (level >= NE_LOG_LEVEL_WARN) {
        out = stderr;
    }

    fputs(message, out);
    fflush(out);
}

static void ne_log_sink_file(void *user_data, NELogLevel level, const char *message) {
    (void)level;
    FILE *file = (FILE *)user_data;
    if (!file) {
        return;
    }
    fputs(message, file);
    fflush(file);
}

static const char *ne_log_level_string(NELogLevel level) {
    switch (level) {
    case NE_LOG_LEVEL_TRACE:
        return "TRACE";
    case NE_LOG_LEVEL_DEBUG:
        return "DEBUG";
    case NE_LOG_LEVEL_INFO:
        return "INFO";
    case NE_LOG_LEVEL_WARN:
        return "WARN";
    case NE_LOG_LEVEL_ERROR:
        return "ERROR";
    case NE_LOG_LEVEL_FATAL:
        return "FATAL";
    case NE_LOG_LEVEL_OFF:
        return "OFF";
    default:
        return "UNKNOWN";
    }
}

static void ne_logger_close_owned_file(NELogger *logger) {
    if (!logger || !logger->owned_file) {
        return;
    }
    fclose(logger->owned_file);
    logger->owned_file = NULL;
}

static void ne_logger_init_defaults(NELogger *logger) {
    logger->level = NE_LOG_LEVEL_INFO;
    logger->sink = ne_log_sink_console;
    logger->sink_user_data = NULL;
    logger->owned_file = NULL;
}

static NELogger g_builtin_logger;
static bool g_builtin_logger_initialized = false;
static NELogger *g_default_logger = NULL;

NELogger *ne_logger_create(void) {
    NELogger *logger = (NELogger *)calloc(1, sizeof(NELogger));
    if (!logger) {
        return NULL;
    }
    ne_logger_init_defaults(logger);
    return logger;
}

void ne_logger_destroy(NELogger *logger) {
    if (!logger) {
        return;
    }

    /* Prevent dangling default logger pointer. */
    if (logger == g_default_logger) {
        ne_log_set_default_logger(NULL);
    }

    ne_logger_close_owned_file(logger);
    free(logger);
}

void ne_logger_set_level(NELogger *logger, NELogLevel level) {
    if (!logger) {
        return;
    }
    logger->level = level;
}

NELogLevel ne_logger_get_level(const NELogger *logger) {
    return logger ? logger->level : NE_LOG_LEVEL_OFF;
}

void ne_logger_set_sink(NELogger *logger, NELogSinkFn sink, void *user_data) {
    if (!logger) {
        return;
    }

    ne_logger_close_owned_file(logger);

    logger->sink = sink;
    logger->sink_user_data = user_data;
}

void ne_logger_set_output_stdout(NELogger *logger) {
    if (!logger) {
        return;
    }

    ne_logger_close_owned_file(logger);

    logger->sink = ne_log_sink_file;
    logger->sink_user_data = stdout;
}

void ne_logger_set_output_stderr(NELogger *logger) {
    if (!logger) {
        return;
    }

    ne_logger_close_owned_file(logger);

    logger->sink = ne_log_sink_file;
    logger->sink_user_data = stderr;
}

bool ne_logger_set_output_file(NELogger *logger, FILE *file, bool close_on_destroy) {
    if (!logger || !file) {
        return false;
    }

    ne_logger_close_owned_file(logger);

    logger->sink = ne_log_sink_file;
    logger->sink_user_data = file;
    logger->owned_file = close_on_destroy ? file : NULL;
    return true;
}

bool ne_logger_open_file(NELogger *logger, const char *path, bool append) {
    if (!logger || !path) {
        return false;
    }

    FILE *file = fopen(path, append ? "ab" : "wb");
    if (!file) {
        return false;
    }

    return ne_logger_set_output_file(logger, file, true);
}

static bool ne_logger_should_log(const NELogger *logger, NELogLevel level) {
    if (!logger || !logger->sink) {
        return false;
    }
    if (logger->level == NE_LOG_LEVEL_OFF) {
        return false;
    }
    return level >= logger->level;
}

static void ne_logger_emit_formatted(NELogger *logger, NELogLevel level, const char *fmt, va_list args) {
    if (!ne_logger_should_log(logger, level) || !fmt) {
        return;
    }

    char stack_msg[1024];
    va_list args_copy;
    va_copy(args_copy, args);
    const int msg_len = vsnprintf(stack_msg, sizeof(stack_msg), fmt, args_copy);
    va_end(args_copy);

    if (msg_len < 0) {
        return;
    }

    const char *msg = stack_msg;
    char *heap_msg = NULL;

    if ((size_t)msg_len >= sizeof(stack_msg)) {
        heap_msg = (char *)malloc((size_t)msg_len + 1u);
        if (!heap_msg) {
            return;
        }
        (void)vsnprintf(heap_msg, (size_t)msg_len + 1u, fmt, args);
        msg = heap_msg;
    }

    const char *level_str = ne_log_level_string(level);

    char prefix[32];
    const int prefix_len = snprintf(prefix, sizeof(prefix), "[%s] ", level_str);
    if (prefix_len < 0) {
        free(heap_msg);
        return;
    }

    const bool has_newline = (msg_len > 0) && (msg[msg_len - 1] == '\n');
    const size_t out_len = (size_t)prefix_len + (size_t)msg_len + (has_newline ? 0u : 1u);

    char *out = (char *)malloc(out_len + 1u);
    if (!out) {
        free(heap_msg);
        return;
    }

    memcpy(out, prefix, (size_t)prefix_len);
    memcpy(out + (size_t)prefix_len, msg, (size_t)msg_len);
    size_t cursor = (size_t)prefix_len + (size_t)msg_len;

    if (!has_newline) {
        out[cursor++] = '\n';
    }
    out[cursor] = '\0';

    logger->sink(logger->sink_user_data, level, out);

    free(out);
    free(heap_msg);
}

void ne_logger_vlog(NELogger *logger, NELogLevel level, const char *fmt, va_list args) {
    if (!logger) {
        logger = ne_log_get_default_logger();
    }

    /*
     * `args` may be used multiple times in `ne_logger_emit_formatted` depending
     * on whether the stack buffer is large enough.
     */
    va_list args_copy;
    va_copy(args_copy, args);
    ne_logger_emit_formatted(logger, level, fmt, args_copy);
    va_end(args_copy);
}

void ne_logger_log(NELogger *logger, NELogLevel level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ne_logger_vlog(logger, level, fmt, args);
    va_end(args);
}

NELogger *ne_log_get_default_logger(void) {
    if (!g_builtin_logger_initialized) {
        ne_logger_init_defaults(&g_builtin_logger);
        g_builtin_logger_initialized = true;
    }

    return g_default_logger ? g_default_logger : &g_builtin_logger;
}

void ne_log_set_default_logger(NELogger *logger) {
    g_default_logger = logger;
}

void ne_vlog(NELogLevel level, const char *fmt, va_list args) {
    ne_logger_vlog(ne_log_get_default_logger(), level, fmt, args);
}

void ne_log(NELogLevel level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ne_vlog(level, fmt, args);
    va_end(args);
}
