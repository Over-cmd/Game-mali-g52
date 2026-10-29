#pragma once
#define ANDROID_DLEXT_RESERVED 0
#define ANDROID_DLEXT_RESERVED_ADDRESS 0
struct android_dlextinfo {
    unsigned long flags;
    void* reserved_addr;
    unsigned long reserved_size;
    int relro_fd;
    int library_fd;
    unsigned long library_offset;
    unsigned long low_addr;
    unsigned long high_addr;
    int target_sdk_version;
};
