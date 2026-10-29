#pragma once

#include <cstdint>
#include <sys/types.h>

#define ANDROID_DLEXT_RESERVED          0x00000001
#define ANDROID_DLEXT_RESERVED_ADDRESS  0x00000002
#define ANDROID_DLEXT_FROM_FILE         0x00000004
#define ANDROID_DLEXT_USE_NAMESPACE     0x00000008
#define ANDROID_DLEXT_USE_LIBRARY_FD    0x00000010

struct android_namespace_t;

struct android_dlextinfo {
    uint64_t flags;
    void* reserved_addr;
    size_t reserved_size;
    int relro_fd;
    int library_fd;
    off64_t library_offset;
    void* reserved1;
    void* reserved2;
    int target_sdk_version;
    int unused1;
    void* library_namespace;
};

#ifdef __cplusplus
extern "C" {
#endif

void* android_dlopen_ext(const char* filename, int flags, const android_dlextinfo* info);

#ifdef __cplusplus
}
#endif
