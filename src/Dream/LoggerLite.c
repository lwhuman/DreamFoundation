#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
#include <threads.h>
#ifndef REMOVE_DREAM_LOGGER

#include <Dream/LoggerLite.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "Platform.h"

#if defined(DREAM_PLATFORM_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#define DREAM_LOG_MAX_MESSAGE 1024
typedef struct DreamLogMsg {
    DreamLogLevel level;
    uint32_t timestamp;
    uint32_t threadid;
    uint32_t pid;
    char category[32];
    char message[DREAM_LOG_MAX_MESSAGE];
} DreamLogMsg;

typedef struct DreamAsyncRingBuffer {
    DreamLogMsg *buffer;
    _Atomic size_t head;
    _Atomic size_t tail;
} DreamAsyncRingBuffer;

typedef struct DreamRingBuffer {
    bool initialized;
    bool dump_after_async_thread_join;
    char *buffer;      /* contiguous memory */
    uint32_t capacity; /* number of lines */
    uint32_t line_len; /* max chars per line */
    uint32_t head;     /* next write index */
    uint32_t count;    /* valid lines */
} DreamRingBuffer;

typedef struct DreamLoggerState {
    bool initialized;
    bool enabled;
    bool use_color;
    bool use_emoji;
    bool show_time;
    bool show_thread;
    DreamLogLevel global_min_log_level;
    DreamLoggerSink **sinks;
    uint16_t sink_count;
    FILE *logfile;
    DreamRingBuffer ring;
    DreamLogCallbackFn callback;
    void *callback_user_data;

    bool async_enabled;
    DreamAsyncRingBuffer async_ringbuff;
    size_t async_queue_capacity; // no of DreamLogMsg elements
    DreamLogAsyncOverflowPolicy async_log_overflow_policy;

    thrd_t async_worker;
    atomic_bool async_thread_running;
    mtx_t sleep_mutex;
    cnd_t sleep_cond;

#ifdef DREAM_PLATFORM_WIN32
    HANDLE console;
    WORD default_attributes;
#endif
} DreamLoggerState;

static DreamLoggerState g_logger;

static const char *_dream_log_level_string(DreamLogLevel level) {
    switch (level) {
        case DREAM_LOG_TRACE:    return "TRACE";
        case DREAM_LOG_DEBUG:    return "DEBUG";
        case DREAM_LOG_INFO:     return "INFO ";
        case DREAM_LOG_WARNING:  return "WARN ";
        case DREAM_LOG_CRITICAL: return "CRITICAL";
        case DREAM_LOG_FATAL:    return "FATAL";
        default:                 return "?????";
    }
}

#if defined(DREAM_PLATFORM_WIN32)

static WORD __dream_log_level_color(DreamLogLevel level) {
    switch (level) {
        case DREAM_LOG_TRACE: return FOREGROUND_INTENSITY;
        case DREAM_LOG_DEBUG:
            return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        case DREAM_LOG_INFO: return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case DREAM_LOG_WARN:
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case DREAM_LOG_ERROR: return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case DREAM_LOG_FATAL:
            return FOREGROUND_RED | FOREGROUND_INTENSITY | BACKGROUND_RED;
        default: return g_logger.default_attributes;
    }
}

static void _dream_set_color(DreamLogLevel level) {
    if (!g_logger.use_color) return;
    SetConsoleTextAttribute(g_logger.console, __dream_log_level_color(level));
}

static void _dream_reset_color(void) {
    if (!g_logger.use_color) return;
    SetConsoleTextAttribute(g_logger.console, g_logger.default_attributes);
}

static uint32_t _dream_thread_id(void) {
    return (uint32_t)GetCurrentThreadId();
}

static void _dream_time_string(char *out, size_t size) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(
        out,
        size,
        "%02d:%02d:%02d.%03d",
        t.wHour,
        t.wMinute,
        t.wSecond,
        t.wMilliseconds
    );
}

#else /* POSIX */

static const char *__dream_log_level_color_ansi(DreamLogLevel level) {
    switch (level) {
        case DREAM_LOG_TRACE:    return "\033[90m";
        case DREAM_LOG_DEBUG:    return "\033[36m";
        case DREAM_LOG_INFO:     return "\033[32m";
        case DREAM_LOG_WARNING:  return "\033[33m";
        case DREAM_LOG_CRITICAL: return "\033[31m";
        case DREAM_LOG_FATAL:    return "\033[1;31m";
        default:                 return "";
    }
}

static void _dream_set_color(DreamLogLevel level) {
    if (g_logger.use_color) fputs(__dream_log_level_color_ansi(level), stdout);
}

static void _dream_reset_color(void) {
    if (g_logger.use_color) fputs("\033[0m", stdout);
}

static uint32_t _dream_thread_id(void) {
    return (uint32_t)(uintptr_t)pthread_self();
}

static void _dream_time_string(char *out, size_t size) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    snprintf(
        out,
        size,
        "%02d:%02d:%02d.%03ld",
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        tv.tv_usec / 1000
    );
}

#endif // !DREAM_PLATFORM_WIN32

static void _dream_write_stdout(const char *text) {
    fputs(text, stdout);
    fflush(stdout);
}

static void _dream_write_stderr(const char *text) {
    fputs(text, stderr);
    fflush(stderr);
}

static void _dream_write_file(const char *text) {
    if (g_logger.logfile) {
        fputs(text, g_logger.logfile);
        fflush(g_logger.logfile);
    }
}

static int _dream_async_worker_fn(void *args) {
    for (;;) {
        size_t head = atomic_load_explicit(
            &g_logger.async_ringbuff.head, memory_order_acquire
        );
        size_t tail = atomic_load_explicit(
            &g_logger.async_ringbuff.tail, memory_order_acquire
        );
        if (head == tail) {
            // No work available
            if (!atomic_load_explicit(
                    &g_logger.async_thread_running, memory_order_acquire
                ))
                break;

            // Sleep until signaled
            mtx_lock(&g_logger.sleep_mutex);
            head = atomic_load_explicit(
                &g_logger.async_ringbuff.head, memory_order_acquire
            );
            tail = atomic_load_explicit(
                &g_logger.async_ringbuff.tail, memory_order_acquire
            );
            if (head == tail &&
                atomic_load_explicit(
                    &g_logger.async_thread_running, memory_order_acquire
                )) {
                cnd_wait(&g_logger.sleep_cond, &g_logger.sleep_mutex);
            }
            mtx_unlock(&g_logger.sleep_mutex);

            continue;
        }

        DreamLogMsg *event = &g_logger.async_ringbuff
                                  .buffer[head % g_logger.async_queue_capacity];

        for (size_t i = 0; i < g_logger.sink_count; ++i) {
            DreamLoggerSink *sink = g_logger.sinks[i];
            if (event->level >= sink->min_level) sink->__write(sink, event);
        }

        atomic_fetch_add_explicit(
            &g_logger.async_ringbuff.head, 1, memory_order_release
        );
    }

    return 0;
}

static bool _dream_async_push(const DreamLogMsg *log) {
    size_t tail = atomic_fetch_add_explicit(
        &g_logger.async_ringbuff.tail, 1, memory_order_acq_rel
    );
    size_t head = atomic_load_explicit(
        &g_logger.async_ringbuff.head, memory_order_acquire
    );

    if ((tail - head) >= g_logger.async_queue_capacity) {
        switch (g_logger.async_log_overflow_policy) {
            case DROP_NEWEST: return false;
            case DROP_OLDEST: {
                atomic_fetch_add_explicit(
                    &g_logger.async_ringbuff.head, 1, memory_order_release
                );
                break;
            }
            case BLOCK: {
                while (
                    (tail -
                         atomic_load_explicit(
                             &g_logger.async_ringbuff.head, memory_order_acquire
                         ) >=
                     g_logger.async_queue_capacity)
                ) {
                    thrd_yield();
                }
                break;
            }
        }
    }

    g_logger.async_ringbuff.buffer[tail % g_logger.async_queue_capacity] = *log;

    // TODO: Wake up the worker thread only when
    // more than 90% of the async ring buffer is full.
    mtx_lock(&g_logger.sleep_mutex);
    cnd_signal(&g_logger.sleep_cond);
    mtx_unlock(&g_logger.sleep_mutex);

    return true;
}

static void _dream_ring_buffer_push(const char *line) {
    DreamRingBuffer *r = &g_logger.ring;

    if (!r->buffer) return;

    char *dst = r->buffer + (r->head * r->line_len);
    strncpy(dst, line, r->line_len - 1);
    dst[r->line_len - 1] = '\0';

    r->head = (r->head + 1) % r->capacity;
    if (r->count < r->capacity) r->count++;
}

static void __get_formatted_log(
    const DreamLogMsg *log, char *output_buffer, size_t output_buffer_size
) {
    char time_buf[32] = {0};

    if (g_logger.show_time) {
        _dream_time_string(time_buf, sizeof(time_buf));
    }

    int offset = 0;

    if (g_logger.show_time)
        offset += snprintf(
            output_buffer + offset,
            output_buffer_size - offset,
            "[%s] ",
            time_buf
        );

    offset += snprintf(
        output_buffer + offset,
        output_buffer_size - offset,
        "[%s] ",
        _dream_log_level_string(log->level)
    );

    offset += snprintf(
        output_buffer + offset,
        output_buffer_size - offset,
        "[%s] ",
        log->category
    );

    if (g_logger.show_thread)
        offset += snprintf(
            output_buffer + offset,
            output_buffer_size - offset,
            "[T:%u] ",
            _dream_thread_id()
        );

    snprintf(
        output_buffer + offset,
        output_buffer_size - offset,
        "%s\n",
        log->message
    );
}

static void
_dream_sink_stdout_write(DreamLoggerSink *this, const DreamLogMsg *log) {
    char formatted_line[1400];
    __get_formatted_log(log, formatted_line, sizeof(formatted_line));
    _dream_set_color(log->level);
    _dream_write_stdout(formatted_line);
    _dream_reset_color();
}
static void
_dream_sink_stderr_write(DreamLoggerSink *this, const DreamLogMsg *log) {
    char formatted_line[1400];
    __get_formatted_log(log, formatted_line, sizeof(formatted_line));
    _dream_set_color(log->level);
    _dream_write_stdout(formatted_line);
    _dream_reset_color();
}
static void
_dream_sink_file_write(DreamLoggerSink *this, const DreamLogMsg *log) {
    char formatted_line[1400];
    __get_formatted_log(log, formatted_line, sizeof(formatted_line));
    _dream_write_file(formatted_line);
}
static void
_dream_sink_ringbuff_write(DreamLoggerSink *this, const DreamLogMsg *log) {
    char formatted_line[1400];
    __get_formatted_log(log, formatted_line, sizeof(formatted_line));
    _dream_ring_buffer_push(formatted_line);
}
static void
_dream_sink_callback_write(DreamLoggerSink *this, const DreamLogMsg *log) {
    char formatted_line[1400];
    __get_formatted_log(log, formatted_line, sizeof(formatted_line));
    g_logger.callback(
        log->level,
        log->category,
        log->message,
        formatted_line,
        g_logger.callback_user_data
    );
}

void DreamLoggerInit(const DreamLoggerConfig *config) {
    memset(&g_logger, 0, sizeof(g_logger));

    g_logger.enabled                           = config->enabled;
    g_logger.use_color                         = config->use_color;
    g_logger.use_emoji                         = config->use_emoji;
    g_logger.show_time                         = config->show_time;
    g_logger.show_thread                       = config->show_thread;
    g_logger.global_min_log_level              = config->global_min_log_level;
    g_logger.initialized                       = true;
    g_logger.ring.initialized                  = false;
    g_logger.ring.dump_after_async_thread_join = false;

#ifdef DREAM_PLATFORM_WIN32
    g_logger.console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(g_logger.console, &info);
    g_logger.default_attributes = info.wAttributes;
#endif

    g_logger.sinks      = config->sinks;
    g_logger.sink_count = config->sink_count;

    for (size_t i = 0; i < g_logger.sink_count; ++i) {
        DreamLoggerSink *sink = g_logger.sinks[i];
        switch (sink->sink_into) {
            case DREAM_LOG_SINK_STDOUT: {
                sink->__write = _dream_sink_stdout_write;
                break;
            }
            case DREAM_LOG_SINK_STDERR: {
                sink->__write = _dream_sink_stderr_write;
                break;
            }
            case DREAM_LOG_SINK_FILE: {
                g_logger.logfile = fopen(config->logfile_path, "w");
                if (g_logger.logfile)
                    sink->__write = _dream_sink_file_write;
                else
                    sink->__write = nullptr;
                break;
            }
            case DREAM_LOG_SINK_RING_BUFFER: {
                uint32_t total_size =
                    config->ring_buffer_lines * config->ring_buffer_line_len;
                g_logger.ring.buffer = malloc(total_size);
                if (g_logger.ring.buffer) {
                    memset(g_logger.ring.buffer, 0, total_size);
                    g_logger.ring.capacity    = config->ring_buffer_lines;
                    g_logger.ring.line_len    = config->ring_buffer_line_len;
                    g_logger.ring.head        = 0;
                    g_logger.ring.count       = 0;
                    g_logger.ring.initialized = true;
                    sink->__write             = _dream_sink_ringbuff_write;
                } else {
                    g_logger.ring.buffer      = nullptr;
                    g_logger.ring.initialized = false;
                    sink->__write             = nullptr;
                }
                break;
            }
            case DREAM_LOG_SINK_USER_CALLBACK: {
                g_logger.callback           = config->callback;
                g_logger.callback_user_data = config->callback_user_data;
                sink->__write               = _dream_sink_callback_write;
                break;
            }
        }
    }

    g_logger.async_enabled = false;
    if (config->async) {
        g_logger.async_enabled             = true;
        g_logger.async_log_overflow_policy = config->async_log_overflow_policy;
        g_logger.async_queue_capacity      = config->async_queue_capacity;
        g_logger.async_ringbuff.buffer     = malloc(
            sizeof(DreamAsyncRingBuffer) * g_logger.async_queue_capacity
        );
        atomic_init(&g_logger.async_ringbuff.head, 0);
        atomic_init(&g_logger.async_ringbuff.tail, 0);
        mtx_init(&g_logger.sleep_mutex, mtx_plain);
        cnd_init(&g_logger.sleep_cond);
        atomic_store(&g_logger.async_thread_running, true);
        thrd_create(&g_logger.async_worker, _dream_async_worker_fn, nullptr);
    }
}

void DreamLoggerShutdown(void) {
    if (g_logger.async_enabled) {
        atomic_store_explicit(
            &g_logger.async_thread_running, false, memory_order_release
        );
        mtx_lock(&g_logger.sleep_mutex);
        cnd_signal(&g_logger.sleep_cond);
        mtx_unlock(&g_logger.sleep_mutex);
        thrd_join(g_logger.async_worker, nullptr);
        cnd_destroy(&g_logger.sleep_cond);
        mtx_destroy(&g_logger.sleep_mutex);
        free(g_logger.async_ringbuff.buffer);
        g_logger.async_ringbuff.buffer = nullptr;
    }
    if (g_logger.logfile) {
        fclose(g_logger.logfile);
        g_logger.logfile = nullptr;
    }
    if (g_logger.ring.initialized && g_logger.ring.buffer) {
        if (g_logger.ring.dump_after_async_thread_join)
            DreamLoggerDumpRingBuffer(stderr);
        free(g_logger.ring.buffer);
        g_logger.ring.buffer      = nullptr;
        g_logger.ring.initialized = false;
    }
    g_logger.initialized = false;
}

void DreamLog(DreamLogLevel level, const char *category, const char *fmt, ...) {
    if (!g_logger.initialized || !g_logger.enabled) return;
    if (level < g_logger.global_min_log_level) return;

    DreamLogMsg log;
    log.level     = level;
    log.threadid  = _dream_thread_id();
    log.pid       = 0; // do later
    log.timestamp = 0; // do later
    snprintf(log.category, sizeof(log.category), "%s", category);
    va_list args;
    va_start(args, fmt);
    vsnprintf(log.message, sizeof(log.message), fmt, args);
    va_end(args);

    if (g_logger.async_enabled) {
        _dream_async_push(&log);
    } else {
        // synchronous dispatch
        for (size_t i = 0; i < g_logger.sink_count; ++i) {
            DreamLoggerSink *sink = g_logger.sinks[i];
            if (log.level >= sink->min_level) sink->__write(sink, &log);
        }
    }

    if (level == DREAM_LOG_FATAL) {
        if (!g_logger.async_enabled) {
            DreamLoggerDumpRingBuffer(stderr);
        } else {
            g_logger.ring.dump_after_async_thread_join = true;
        }
        DreamLoggerShutdown();
#if defined(_WIN32)
        DebugBreak();
#else
        __builtin_trap();
#endif
    }
}

void DreamLoggerDumpRingBuffer(FILE *out) {
    DreamRingBuffer *r = &g_logger.ring;

    if (!r->buffer || r->count == 0) return;

    fprintf(out, "---- Dream Ring Buffer Dump (%u entries) ----\n", r->count);

    uint32_t start = (r->head + r->capacity - r->count) % r->capacity;

    for (uint32_t i = 0; i < r->count; ++i) {
        uint32_t idx     = (start + i) % r->capacity;
        const char *line = r->buffer + (idx * r->line_len);
        fputs(line, out);
    }

    fprintf(out, "--------------------------------------------\n");
    fflush(out);
}

#endif // !REMOVE_DREAM_LOGGER
