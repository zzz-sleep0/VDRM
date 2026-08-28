#ifndef VDRM_ANW_HIDDEN_H
#define VDRM_ANW_HIDDEN_H

#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>

#define ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS 3

typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;

typedef struct android_native_base_t {
    int magic;
    int version;
    void *reserved[4];
    void (*incRef)(struct android_native_base_t *base);
    void (*decRef)(struct android_native_base_t *base);
} android_native_base_t;

typedef struct ANativeWindowBuffer {
    android_native_base_t common;
    int width;
    int height;
    int stride;
    int format;
    int usage_deprecated;
    uintptr_t layerCount;
    void *reserved[1];
    const native_handle_t *handle;
    uint64_t usage;
    void *reserved_proc[8 - (sizeof(uint64_t) / sizeof(void *))];
} ANativeWindowBuffer;

typedef int (*pfn_ANativeWindow_setBufferCount)(ANativeWindow *, size_t);
typedef int (*pfn_ANativeWindow_query)(const ANativeWindow *, int, int *);
typedef int (*pfn_ANativeWindow_dequeueBuffer)(ANativeWindow *, ANativeWindowBuffer **, int *);
typedef int (*pfn_ANativeWindow_queueBuffer)(ANativeWindow *, ANativeWindowBuffer *, int);
typedef int (*pfn_ANativeWindow_cancelBuffer)(ANativeWindow *, ANativeWindowBuffer *, int);

struct anw_api {
    pfn_ANativeWindow_setBufferCount setBufferCount;
    pfn_ANativeWindow_query          query;
    pfn_ANativeWindow_dequeueBuffer  dequeueBuffer;
    pfn_ANativeWindow_queueBuffer    queueBuffer;
    pfn_ANativeWindow_cancelBuffer   cancelBuffer;
};

static inline int anw_api_load(struct anw_api *api)
{
    void *lib = dlopen("libnativewindow.so", RTLD_NOW);
    if (!lib) return -1;

    api->setBufferCount = (pfn_ANativeWindow_setBufferCount)dlsym(lib, "ANativeWindow_setBufferCount");
    api->query          = (pfn_ANativeWindow_query)dlsym(lib, "ANativeWindow_query");
    api->dequeueBuffer  = (pfn_ANativeWindow_dequeueBuffer)dlsym(lib, "ANativeWindow_dequeueBuffer");
    api->queueBuffer    = (pfn_ANativeWindow_queueBuffer)dlsym(lib, "ANativeWindow_queueBuffer");
    api->cancelBuffer   = (pfn_ANativeWindow_cancelBuffer)dlsym(lib, "ANativeWindow_cancelBuffer");

    if (!api->setBufferCount || !api->query ||
        !api->dequeueBuffer || !api->queueBuffer || !api->cancelBuffer)
        return -1;

    return 0;
}

#endif