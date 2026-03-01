#ifndef NE_LOG_H
#define NE_LOG_H

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Log severity levels.
 *
 * A logger may filter out messages below its configured minimum level.
 */
typedef enum NELogLevel {
    NE_LOG_LEVEL_TRACE = 0,
    NE_LOG_LEVEL_DEBUG = 1,
    NE_LOG_LEVEL_INFO = 2,
    NE_LOG_LEVEL_WARN = 3,
    NE_LOG_LEVEL_ERROR = 4,
    NE_LOG_LEVEL_FATAL = 5,
    /** Disables all logging for the logger. */
    NE_LOG_LEVEL_OFF = 6,
} NELogLevel;

/** Opaque logger instance. */
typedef struct NELogger NELogger;

/**
 * Log sink callback.
 *
 * Parameters:
 * - `user_data`: The sink user pointer configured by `ne_logger_set_sink`.
 * - `level`: Severity of the message.
 * - `message`: Fully formatted message, including any prefix and trailing
 *   newline. The string is valid only for the duration of the callback.
 */
typedef void (*NELogSinkFn)(void *user_data, NELogLevel level, const char *message);

/**
 * Create a logger instance.
 *
 * The created logger defaults to:
 * - minimum level: `NE_LOG_LEVEL_INFO`
 * - sink: console sink (stdout for INFO and below, stderr for WARN and above)
 *
 * Returns:
 * - A valid `NELogger*` on success.
 * - `NULL` on allocation failure.
 */
NELogger *ne_logger_create(void);

/**
 * Destroy a logger instance.
 *
 * Parameters:
 * - `logger`: Logger instance (may be NULL).
 */
void ne_logger_destroy(NELogger *logger);

/**
 * Set the minimum severity level for a logger.
 *
 * Parameters:
 * - `logger`: Logger instance.
 * - `level`: Minimum level that will be emitted.
 */
void ne_logger_set_level(NELogger *logger, NELogLevel level);

/**
 * Get the minimum severity level for a logger.
 *
 * Parameters:
 * - `logger`: Logger instance.
 *
 * Returns:
 * - The current minimum log level.
 */
NELogLevel ne_logger_get_level(const NELogger *logger);

/**
 * Set a custom sink function.
 *
 * Parameters:
 * - `logger`: Logger instance.
 * - `sink`: Sink callback (if NULL, logging is disabled).
 * - `user_data`: Pointer passed to `sink`.
 */
void ne_logger_set_sink(NELogger *logger, NELogSinkFn sink, void *user_data);

/**
 * Configure the logger to write to stdout.
 *
 * Parameters:
 * - `logger`: Logger instance.
 */
void ne_logger_set_output_stdout(NELogger *logger);

/**
 * Configure the logger to write to stderr.
 *
 * Parameters:
 * - `logger`: Logger instance.
 */
void ne_logger_set_output_stderr(NELogger *logger);

/**
 * Configure the logger to write to a `FILE*`.
 *
 * Parameters:
 * - `logger`: Logger instance.
 * - `file`: Destination file handle (must not be NULL).
 * - `close_on_destroy`: If true, the logger will `fclose(file)` when the logger
 *   is destroyed or when a different output is set.
 *
 * Returns:
 * - `true` on success.
 * - `false` if `logger` or `file` is NULL.
 */
bool ne_logger_set_output_file(NELogger *logger, FILE *file, bool close_on_destroy);

/**
 * Open a file and configure the logger to write to it.
 *
 * Parameters:
 * - `logger`: Logger instance.
 * - `path`: UTF-8 file path.
 * - `append`: If true, append to the file; otherwise overwrite.
 *
 * Returns:
 * - `true` on success.
 * - `false` on failure.
 */
bool ne_logger_open_file(NELogger *logger, const char *path, bool append);

/**
 * Log a formatted message to a logger.
 *
 * Usage:
 * - Call like `printf`: `ne_logger_log(logger, NE_LOG_LEVEL_INFO, "x=%d", x);`
 *
 * Parameters:
 * - `logger`: Logger instance.
 * - `level`: Severity.
 * - `fmt`: `printf`-style format string.
 */
void ne_logger_log(NELogger *logger, NELogLevel level, const char *fmt, ...);

/**
 * `va_list` variant of `ne_logger_log`.
 *
 * Parameters:
 * - `logger`: Logger instance.
 * - `level`: Severity.
 * - `fmt`: `printf`-style format string.
 * - `args`: Format arguments.
 */
void ne_logger_vlog(NELogger *logger, NELogLevel level, const char *fmt, va_list args);

/**
 * Get the current default logger.
 *
 * Returns:
 * - The logger used by the global logging functions (`ne_log`, `NE_LOG_INFO`,
 *   etc.). Never returns NULL.
 */
NELogger *ne_log_get_default_logger(void);

/**
 * Set the default logger.
 *
 * The global logging functions (`ne_log`, `NE_LOG_INFO`, etc.) will route to
 * this logger.
 *
 * Parameters:
 * - `logger`: Logger instance. If NULL, the default is reset to the internal
 *   built-in logger.
 */
void ne_log_set_default_logger(NELogger *logger);

/**
 * Log to the default logger.
 *
 * Usage:
 * - Call like `printf`: `ne_log(NE_LOG_LEVEL_INFO, "hello %s", name);`
 *
 * Parameters:
 * - `level`: Severity.
 * - `fmt`: `printf`-style format string.
 */
void ne_log(NELogLevel level, const char *fmt, ...);

/**
 * `va_list` variant of `ne_log`.
 *
 * Parameters:
 * - `level`: Severity.
 * - `fmt`: `printf`-style format string.
 * - `args`: Format arguments.
 */
void ne_vlog(NELogLevel level, const char *fmt, va_list args);

/** Convenience macros for logging to the default logger. */
#define NE_LOG_TRACE(...) ne_log(NE_LOG_LEVEL_TRACE, __VA_ARGS__)
#define NE_LOG_DEBUG(...) ne_log(NE_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define NE_LOG_INFO(...) ne_log(NE_LOG_LEVEL_INFO, __VA_ARGS__)
#define NE_LOG_WARN(...) ne_log(NE_LOG_LEVEL_WARN, __VA_ARGS__)
#define NE_LOG_ERROR(...) ne_log(NE_LOG_LEVEL_ERROR, __VA_ARGS__)
#define NE_LOG_FATAL(...) ne_log(NE_LOG_LEVEL_FATAL, __VA_ARGS__)

/**
 * Assertion macro.
 *
 * In debug builds, `NE_ASSERT(cond)` logs a fatal message containing the failed
 * condition and then aborts.
 *
 * The macro is compiled out when assertions are disabled.
 */
#if !defined(NE_ENABLE_ASSERTS)
#if !defined(NDEBUG)
#define NE_ENABLE_ASSERTS 1
#else
#define NE_ENABLE_ASSERTS 0
#endif
#endif

#if NE_ENABLE_ASSERTS
#define NE_ASSERT(cond)                                                                                                    \
    do {                                                                                                                   \
        if (!(cond)) {                                                                                                     \
            ne_log(NE_LOG_LEVEL_FATAL, "Assertion failed: %s (%s:%d %s)", #cond, __FILE__, __LINE__, __func__);          \
            abort();                                                                                                       \
        }                                                                                                                  \
    } while (0)
#else
#define NE_ASSERT(cond) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
