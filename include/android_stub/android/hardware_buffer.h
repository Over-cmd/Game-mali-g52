/*
 * Copyright 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file hardware_buffer.h
 * @brief API for native hardware buffers.
 */
/**
 * @defgroup AHardwareBuffer Native Hardware Buffer
 *
 * AHardwareBuffer objects represent chunks of memory that can be
 * accessed by various hardware components in the system. It can be
 * easily converted to the Java counterpart
 * android.hardware.HardwareBuffer and passed between processes using
 * Binder. All operations involving AHardwareBuffer and HardwareBuffer
 * are zero-copy, i.e., passing AHardwareBuffer to another process
 * creates a shared view of the same region of memory.
 *
 * AHardwareBuffers can be bound to EGL/OpenGL and Vulkan primitives.
 * For EGL, use the extension function eglGetNativeClientBufferANDROID
 * to obtain an EGLClientBuffer and pass it directly to
 * eglCreateImageKHR. Refer to the EGL extensions
 * EGL_ANDROID_get_native_client_buffer and
 * EGL_ANDROID_image_native_buffer for more information. In Vulkan,
 * the contents of the AHardwareBuffer can be accessed as external
 * memory. See the VK_ANDROID_external_memory_android_hardware_buffer
 * extension for details.
 *
 * @{
 */

#ifndef ANDROID_HARDWARE_BUFFER_H
#define ANDROID_HARDWARE_BUFFER_H

#include <inttypes.h>
#include <sys/cdefs.h>
#include <android/rect.h>

#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(x)
#endif

__BEGIN_DECLS

/**
 * Buffer pixel formats.
 */
enum AHardwareBuffer_Format {
    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_R8G8B8A8_UNORM
     *   OpenGL ES: GL_RGBA8
     */
    AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM           = 1,

    /**
     * 32 bits per pixel, 8 bits per channel format where alpha values are
     * ignored (always opaque).
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_R8G8B8A8_UNORM
     *   OpenGL ES: GL_RGB8
     */
    AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM           = 2,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_R8G8B8_UNORM
     *   OpenGL ES: GL_RGB8
     */
    AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM             = 3,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_R5G6B5_UNORM_PACK16
     *   OpenGL ES: GL_RGB565
     */
    AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM             = 4,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_R16G16B16A16_SFLOAT
     *   OpenGL ES: GL_RGBA16F
     */
    AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT       = 0x16,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_A2B10G10R10_UNORM_PACK32
     *   OpenGL ES: GL_RGB10_A2
     */
    AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM        = 0x2b,

    /**
     * Opaque binary blob format.
     * Must have height 1 and one layer, with width equal to the buffer
     * size in bytes. Corresponds to Vulkan buffers and OpenGL buffer
     * objects. Can be bound to the latter using GL_EXT_external_buffer.
     */
    AHARDWAREBUFFER_FORMAT_BLOB                     = 0x21,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_D16_UNORM
     *   OpenGL ES: GL_DEPTH_COMPONENT16
     */
    AHARDWAREBUFFER_FORMAT_D16_UNORM                = 0x30,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_X8_D24_UNORM_PACK32
     *   OpenGL ES: GL_DEPTH_COMPONENT24
     */
    AHARDWAREBUFFER_FORMAT_D24_UNORM                = 0x31,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_D24_UNORM_S8_UINT
     *   OpenGL ES: GL_DEPTH24_STENCIL8
     */
    AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT        = 0x32,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_D32_SFLOAT
     *   OpenGL ES: GL_DEPTH_COMPONENT32F
     */
    AHARDWAREBUFFER_FORMAT_D32_FLOAT                = 0x33,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_D32_SFLOAT_S8_UINT
     *   OpenGL ES: GL_DEPTH32F_STENCIL8
     */
    AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT        = 0x34,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_S8_UINT
     *   OpenGL ES: GL_STENCIL_INDEX8
     */
    AHARDWAREBUFFER_FORMAT_S8_UINT                  = 0x35,

    /**
     * YUV 420 888 format.
     * Must have an even width and height. Can be accessed in OpenGL
     * shaders through an external sampler. Does not support mip-maps
     * cube-maps or multi-layered textures.
     */
    AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420             = 0x23,

    /**
     * Corresponding formats:
     *   Vulkan: VK_FORMAT_R8_UNORM
     *   OpenGL ES: GR_GL_R8
     */
    AHARDWAREBUFFER_FORMAT_R8_UNORM                 = 0x38,
};

/**
 * Buffer usage flags, specifying how the buffer will be accessed.
 */
enum AHardwareBuffer_UsageFlags {
    /// The buffer will never be locked for direct CPU reads using the
    /// AHardwareBuffer_lock() function. Note that reading the buffer
    /// using OpenGL or Vulkan functions or memory mappings is still
    /// allowed.
    AHARDWAREBUFFER_USAGE_CPU_READ_NEVER        = 0UL,
    /// The buffer will sometimes be locked for direct CPU reads using
    /// the AHardwareBuffer_lock() function. Note that reading the
    /// buffer using OpenGL or Vulkan functions or memory mappings
    /// does not require the presence of this flag.
    AHARDWAREBUFFER_USAGE_CPU_READ_RARELY       = 2UL,
    /// The buffer will often be locked for direct CPU reads using
    /// the AHardwareBuffer_lock() function. Note that reading the
    /// buffer using OpenGL or Vulkan functions or memory mappings
    /// does not require the presence of this flag.
    AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN        = 3UL,
    /// CPU read value mask.
    AHARDWAREBUFFER_USAGE_CPU_READ_MASK         = 0xFUL,

    /// The buffer will never be locked for direct CPU writes using the
    /// AHardwareBuffer_lock() function. Note that writing the buffer
    /// using OpenGL or Vulkan functions or memory mappings is still
    /// allowed.
    AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER       = 0UL << 4,
    /// The buffer will sometimes be locked for direct CPU writes using
    /// the AHardwareBuffer_lock() function. Note that writing the
    /// buffer using OpenGL or Vulkan functions or memory mappings
    /// does not require the presence of this flag.
    AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY      = 2UL << 4,
    /// The buffer will often be locked for direct CPU writes using
    /// the AHardwareBuffer_lock() function. Note that writing the
    /// buffer using OpenGL or Vulkan functions or memory mappings
    /// does not require the presence of this flag.
    AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN       = 3UL << 4,
    /// CPU write value mask.
    AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK        = 0xFUL << 4,

    /// The buffer will be read from by the GPU as a texture.
    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE      = 1UL << 8,
    /// The buffer will be written to by the GPU as a framebuffer attachment.
    AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER        = 1UL << 9,
    /**
     * The buffer will be written to by the GPU as a framebuffer
     * attachment.
     *
     * Note that the name of this flag is somewhat misleading: it does
     * not imply that the buffer contains a color format. A buffer with
     * depth or stencil format that will be used as a framebuffer
     * attachment should also have this flag. Use the equivalent flag
     * AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER to avoid this confusion.
     */
    AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT       = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER,
    /**
     * The buffer will be used as a
