#ifndef DREAM_PUBLIC_API
#define DREAM_PUBLIC_API

#include <stddef.h>
#include <stdint.h>

typedef struct DreamLoggerConfig DreamLoggerConfig;

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*DreamUserAllocFn)(
    size_t size, size_t alignment, void *user_data
);
typedef void (*DreamUserFreeFn)(void *ptr, void *user_data);

typedef struct DreamUserAllocator {
    DreamUserAllocFn alloc;
    DreamUserFreeFn free;
    void *user_data;
} DreamUserAllocator;

typedef struct DreamConfig {
    bool enable_logging;
    const DreamLoggerConfig *loggerConfig;
    bool enable_windowing_subsystem;
    bool enable_audio_subsystem;
    bool use_custom_allocator;
    const DreamUserAllocator *allocator;
    void *user_data;
} DreamConfig;

bool DreamInit(const DreamConfig *config);
void DreamShutdown();

#ifdef __cplusplus
}
#endif

#endif // !DREAM_PUBLIC_API
