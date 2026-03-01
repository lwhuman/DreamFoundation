#ifndef DREAM_LOGGER_LITE
#define DREAM_LOGGER_LITE

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DreamLogLevel {
    DREAM_LOG_TRACE,
    DREAM_LOG_INFO,
    DREAM_LOG_DEBUG,
    DREAM_LOG_WARNING,
    DREAM_LOG_CRITICAL,
    DREAM_LOG_FATAL,
} DreamLogLevel;

typedef enum DreamLoggerSinkTo {
    DREAM_LOG_SINK_STDOUT        = 1 << 0,
    DREAM_LOG_SINK_STDERR        = 1 << 1,
    DREAM_LOG_SINK_FILE          = 1 << 2,
    DREAM_LOG_SINK_RING_BUFFER   = 1 << 3,
    DREAM_LOG_SINK_USER_CALLBACK = 1 << 4,
} DreamLoggerSinkTo;

// typedef enum DreamLogFormatTokens {
//     TIME,
//     THREAD_ID,
//     CATAGORY,
//     LOG_LEVEL,
// } DreamLogFormatTokens;

typedef struct DreamLoggerSink DreamLoggerSink;
typedef struct DreamLogMsg DreamLogMsg;
typedef void (*DreamSinkWriteFn)(DreamLoggerSink *sink, const DreamLogMsg *log);
struct DreamLoggerSink {
    DreamLoggerSinkTo sink_into;
    DreamLogLevel min_level;
    // DreamLogFormatTokens format[4];
    DreamSinkWriteFn __write;
};

typedef enum DreamLogAsyncOverflowPolicy {
    BLOCK,
    DROP_OLDEST,
    DROP_NEWEST,
} DreamLogAsyncOverflowPolicy;

typedef void (*DreamLogCallbackFn)(
    DreamLogLevel level,
    const char *category,
    const char *message,
    const char *formatted_line,
    void *user_data
);

typedef uint8_t DreamLogSinksBitmask;

typedef struct DreamLoggerConfig {
    bool enabled;
    bool use_color;
    bool use_emoji;
    bool show_time;
    bool show_thread;
    DreamLogLevel global_min_log_level;
    DreamLoggerSink **sinks;
    uint16_t sink_count;
    const char *logfile_path;

    uint32_t ring_buffer_lines;
    uint32_t ring_buffer_line_len;

    DreamLogCallbackFn callback;
    void *callback_user_data;

    bool async;
    size_t async_queue_capacity; // no of DreamLogMsg elements
    DreamLogAsyncOverflowPolicy async_log_overflow_policy;
} DreamLoggerConfig;

#if !defined(REMOVE_DREAM_LOGGER)

void DreamLoggerInit(const DreamLoggerConfig *config);
void DreamLoggerShutdown();

void DreamLog(DreamLogLevel level, const char *tags, const char *fmt, ...);

void DreamLoggerDumpRingBuffer(FILE *out);

#define dTrace(tag, ...)    DreamLog(DREAM_LOG_TRACE, tag, __VA_ARGS__)
#define dDebug(tag, ...)    DreamLog(DREAM_LOG_DEBUG, tag, __VA_ARGS__)
#define dInfo(tag, ...)     DreamLog(DREAM_LOG_INFO, tag, __VA_ARGS__)
#define dWarn(tag, ...)     DreamLog(DREAM_LOG_WARNING, tag, __VA_ARGS__)
#define dCritical(tag, ...) DreamLog(DREAM_LOG_CRITICAL, tag, __VA_ARGS__)
#define dFatal(tag, ...)    DreamLog(DREAM_LOG_FATAL, tag, __VA_ARGS__)

#else
#define dTrace(tag, ...)    ((void)0)
#define dDebug(tag, ...)    ((void)0)
#define dInfo(tag, ...)     ((void)0)
#define dWarn(tag, ...)     ((void)0)
#define dCritical(tag, ...) ((void)0)
#define dFatal(tag, ...)    ((void)0)
#endif // !REMOVE_DREAM_LOGGER

#ifdef __cplusplus
}
#endif

#endif // !DREAM_LOGGER_LITE
