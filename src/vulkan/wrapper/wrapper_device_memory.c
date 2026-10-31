/*
 * wrapper_device_memory.c
 * (Versión modificada con protección de mapeo, manejo de swapchain y WRPER_SYNC)
 */

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_entrypoints.h"
#include "vk_common_entrypoints.h"
#include "util/os_file.h"
#include "util/hash_table.h"
#include "vk_util.h"

#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-heap.h>
#include <fcntl.h>
#include <errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/dma-buf.h>

static void wrapper_dbg(const char *fmt, ...) {
   const char *env = getenv("WRAPPER_DEBUG_FILE");
   if (!env) return;
   char path[512];
   if (env[0] == '/') {
      snprintf(path, sizeof(path), "%s", env);
   } else {
      const char *dir = getenv("TMPDIR");
      snprintf(path, sizeof(path), "%s/wrapper_debug.log", dir ? dir : "/data/local/tmp");
   }
   FILE *f = fopen(path, "a");
   if (!f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

static int
safe_ioctl(int fd, unsigned long request, void *arg)
{
   int ret;
   do {
      ret = ioctl(fd, request, arg);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
   return ret;
}

static int
dma_heap_alloc(int heap_fd, size_t size) {
   struct dma_heap_allocation_data alloc_data = {
      .len = size,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };
   if (safe_ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0)
      return -1;
   return alloc_data.fd;
}

struct ion_allocation_data_1 {
	size_t len;
	size_t align;
	unsigned int heap_id_mask;
	unsigned int flags;
	__u32 handle;
};

struct ion_fd_data_1 {
	__u32 handle;
	int fd;
};

struct ion_handle_data_1 {
	__u32 handle;
};

#define ION_IOC_MAGIC       'I'
#define ION_IOC_ALLOC_1       _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data_1)
#define ION_IOC_FREE_1        _IOWR(ION_IOC_MAGIC, 1, struct ion_handle_data_1)
#define ION_IOC_MAP_1         _IOWR(ION_IOC_MAGIC, 2, struct ion_fd_data_1)
#define ION_IOC_SHARE_1		   _IOWR(ION_IOC_MAGIC, 4, struct ion_fd_data_1)

struct ion_allocation_data_2 {
   __u64 len;
   __u32 heap_id_mask;
   __u32 flags;
   __u32 fd;
   __u32 unused;
};

#define ION_IOC_ALLOC_2       _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data_2)

static unsigned int wrapper_ion_flags(size_t size) {
   static int init = 0, on = 1;
   static size_t minsz = 16777216, maxsz = (size_t) -1;
   if (!init) {
      const char *c = getenv("WRAPPER_ION_CACHED");
      const char *a = getenv("WRAPPER_ION_CACHED_MIN");
      const char *b = getenv("WRAPPER_ION_CACHED_MAX");
      if (c) on = atoi(c);
      if (a) minsz = (size_t) strtoull(a, NULL, 0);
      if (b) maxsz = (size_t) strtoull(b, NULL, 0);
      init = 1;
   }
   return (on && size >= minsz && size <= maxsz) ? 1 : 0;
}

static int
ion_heap_alloc_2(int heap_fd, size_t size) {
   struct ion_allocation_data_2 alloc_data = {
      .len = size,
      .heap_id_mask = (1U << 0) | (1U << 25),
      .flags = wrapper_ion_flags(size),
   };
   if (safe_ioctl(heap_fd, ION_IOC_ALLOC_2, &alloc_data) < 0) {
      alloc_data.heap_id_mask = 1U;
      if (safe_ioctl(heap_fd, ION_IOC_ALLOC_2, &alloc_data) < 0) {
         return -1;
      }
   }
   return alloc_data.fd;
}

static int
ion_heap_alloc(int heap_fd, size_t size) {
   static int ion_iface = 0;
   if (!ion_iface) {
      struct ion_handle_data_1 probe = { .handle = 0 };
      if (safe_ioctl(heap_fd, ION_IOC_FREE_1, &probe) >= 0 || errno != ENOTTY) {
         ion_iface = 1;
      } else {
         ion_iface = 2;
      }
   }
   if (ion_iface == 2) {
      return ion_heap_alloc_2(heap_fd, size);
   }
   struct ion_allocation_data_1 alloc_data = {
      .len = size,
      .align = 0,
      .heap_id_mask = (1U << 0) | (1U << 25),
      .flags = wrapper_ion_flags(size),
   };
   if (safe_ioctl(heap_fd, ION_IOC_ALLOC_1, &alloc_data) < 0) {
      alloc_data.align = 4096;
      alloc_data.heap_id_mask = 1U;
      if (safe_ioctl(heap_fd, ION_IOC_ALLOC_1, &alloc_data) < 0) {
         return -1;
      }
   }
   struct ion_fd_data_1 fd_data = {
      .handle = alloc_data.handle,
      .fd = -1,
   };
   if (safe_ioctl(heap_fd, ION_IOC_SHARE_1, &fd_data) < 0) {
      struct ion_handle_data_1 free_data = { .handle = alloc_data.handle };
      safe_ioctl(heap_fd, ION_IOC_FREE_1, &free_data);
      return -1;
   }
   struct ion_handle_data_1 free_data = { .handle = alloc_data.handle };
   safe_ioctl(heap_fd, ION_IOC_FREE_1, &free_data);
   return fd_data.fd;
}

static int
wrapper_dmabuf_alloc(struct wrapper_device *device, size_t size)
{
   int fd = dma_heap_alloc(device->physical->dma_heap_fd, size);
   if (fd < 0)
      fd = ion_heap_alloc(device->physical->dma_heap_fd, size);
   return fd;
}

uint32_t
wrapper_select_device_memory_type(struct wrapper_device *device,
                                  VkMemoryPropertyFlags flags) {
   VkPhysicalDeviceMemoryProperties *props = &device->physical->memory_properties;
   int idx;
   for (idx = 0; idx < props->memoryTypeCount; idx ++) {
      if (props->memoryTypes[idx].propertyFlags & flags) {
         break;
      }
   }
   return idx < props->memoryTypeCount ? idx : UINT32_MAX;
}

static uint32_t
wrapper_select_allowed_device_memory_type(struct wrapper_device *device,
                                          uint32_t allowed_type_bits,
                                          VkMemoryPropertyFlags flags) {
   VkPhysicalDeviceMemoryProperties *props = &device->physical->memory_properties;
   int idx;
   for (idx = 0; idx < props->memoryTypeCount; idx++) {
      if (!(allowed_type_bits & (1U << idx))) continue;
      if (props->memoryTypes[idx].propertyFlags & flags) return idx;
   }
   return UINT32_MAX;
}

static inline void
unlink_memory_alloc_info_pnext(VkMemoryAllocateInfo *alloc_info, VkStructureType sType)
{
   const VkBaseInStructure *head = (const VkBaseInStructure *)alloc_info->pNext;
   if (head && head->sType == sType) {
      alloc_info->pNext = head->pNext;
      return;
   }
   VkBaseOutStructure *prev = NULL;
   vk_foreach_struct(s, (void *)alloc_info->pNext) {
      if (s->sType == sType) {
         if (prev) prev->pNext = s->pNext;
         return;
      }
      prev = s;
   }
}

static VkResult check_dedicated_allocate_info_for(struct wrapper_device *device,
                                              const VkMemoryDedicatedAllocateInfo *memory_dedicated_info,
                                              VkExternalMemoryHandleTypeFlags handle_types) {
   if (!memory_dedicated_info) return VK_SUCCESS;
   if (memory_dedicated_info->image != VK_NULL_HANDLE) {
      struct wrapper_image *img = get_wrapper_image_from_handle_locked(device, memory_dedicated_info->image);
      if (img && img->handle_types && !(img->handle_types & handle_types))
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   if (memory_dedicated_info->buffer != VK_NULL_HANDLE) {
      struct wrapper_buffer *buf = get_wrapper_buffer_from_handle_locked(device, memory_dedicated_info->buffer);
      if (buf && buf->handle_types && !(buf->handle_types & handle_types))
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   return VK_SUCCESS;
}

static VkResult
wrapper_allocate_memory_dmaheap(struct wrapper_device *device,
                                const VkMemoryAllocateInfo* pAllocateInfo,
                                const VkAllocationCallbacks* pAllocator,
                                VkDeviceMemory* pMemory,
                                int *out_fd) {
   *out_fd = wrapper_dmabuf_alloc(device, pAllocateInfo->allocationSize);
   if (*out_fd < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   VkMemoryFdPropertiesKHR memory_fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      .pNext = NULL,
   };
   VkResult result = device->dispatch_table.GetMemoryFdPropertiesKHR(
      device->dispatch_handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, *out_fd, &memory_fd_props);
   if (result != VK_SUCCESS) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   
   int memory_type_index = wrapper_select_allowed_device_memory_type(device,
      memory_fd_props.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

   if (memory_type_index == UINT32_MAX) return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   VkImportMemoryFdInfoKHR import_fd_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = pAllocateInfo->pNext,
      .fd = os_dupfd_cloexec(*out_fd),
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   VkMemoryAllocateInfo allocate_info = *pAllocateInfo;
   allocate_info.pNext = &import_fd_info;
   allocate_info.memoryTypeIndex = memory_type_index;

   result = device->dispatch_table.AllocateMemory(device->dispatch_handle, &allocate_info, pAllocator, pMemory);
   if (result != VK_SUCCESS && import_fd_info.fd != -1) {
      close(import_fd_info.fd);
   }
   return result;
}

static VkResult
wrapper_allocate_memory_opaque_fd(struct wrapper_device *device,
								  const VkMemoryAllocateInfo *pAllocateInfo,
								  const VkAllocationCallbacks *pAllocator,
								  VkDeviceMemory *pMemory,
								  int *out_fd)
{
   VkExportMemoryAllocateInfo export_memory_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = NULL,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };
   VkMemoryAllocateInfo allocate_info = *pAllocateInfo;
   allocate_info.pNext = &export_memory_info;

   VkResult result = device->dispatch_table.AllocateMemory(device->dispatch_handle, &allocate_info, pAllocator, pMemory);
   if (result != VK_SUCCESS) return result;

   VkMemoryGetFdInfoKHR get_memory_fd = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .pNext = NULL,
      .memory = *pMemory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };
   result = device->dispatch_table.GetMemoryFdKHR(device->dispatch_handle, &get_memory_fd, out_fd);
   if (result != VK_SUCCESS || *out_fd < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   return VK_SUCCESS;
}

static VkResult
wrapper_allocate_memory_ahardware_buffer(struct wrapper_device *device,
                                         const VkMemoryAllocateInfo* pAllocateInfo,
                                         const VkAllocationCallbacks* pAllocator,
                                         VkDeviceMemory* pMemory,
                                         AHardwareBuffer **pAHardwareBuffer) {
   VkExportMemoryAllocateInfo export_memory_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = pAllocateInfo->pNext,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
   };
   VkMemoryAllocateInfo allocate_info = *pAllocateInfo;
   allocate_info.pNext = &export_memory_info;
  
   const VkMemoryDedicatedAllocateInfo *memory_dedicated_info =
      vk_find_struct_const(pAllocateInfo->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   if (memory_dedicated_info && memory_dedicated_info->image != VK_NULL_HANDLE) {
      allocate_info.allocationSize = 0;
   }
   
   VkResult result = device->dispatch_table.AllocateMemory(device->dispatch_handle, &allocate_info, pAllocator, pMemory);
   if (result != VK_SUCCESS) return result;
   
   result = device->dispatch_table.GetMemoryAndroidHardwareBufferANDROID(
      device->dispatch_handle,
      &(VkMemoryGetAndroidHardwareBufferInfoANDROID) {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
         .memory = *pMemory,
      },
      pAHardwareBuffer);

   if (result != VK_SUCCESS || AHardwareBuffer_getNativeHandle(*pAHardwareBuffer) == NULL) {
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   return VK_SUCCESS;
}

static void
wrapper_device_memory_reset(struct wrapper_device_memory *mem) {
   struct wrapper_device *device = mem->device;
   if (mem->ahardware_buffer) {
      AHardwareBuffer_release(mem->ahardware_buffer);
      mem->ahardware_buffer = NULL;
   }
   if (mem->fd != -1) {
      close(mem->fd);
      mem->fd = -1;
   }
   if (mem->map_address && mem->map_size) {
      munmap(mem->map_address, mem->map_size);
      mem->map_address = NULL;
   }
   if (mem->dispatch_handle != VK_NULL_HANDLE) {
      device->dispatch_table.FreeMemory(device->dispatch_handle, mem->dispatch_handle, mem->alloc);
      mem->dispatch_handle = VK_NULL_HANDLE;
   }
}

VkResult
wrapper_device_memory_create(struct wrapper_device *device,
                             const VkAllocationCallbacks *alloc,
                             struct wrapper_device_memory **out_mem)
{
   *out_mem = vk_zalloc2(&device->vk.alloc, alloc, sizeof(struct wrapper_device_memory), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (*out_mem == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;
   (*out_mem)->fd = -1;
   (*out_mem)->device = device;
   (*out_mem)->alloc = alloc ? alloc : &device->vk.alloc;
   list_add(&(*out_mem)->link, &device->device_memory_list);
   return VK_SUCCESS;
}

void
wrapper_device_memory_destroy(struct wrapper_device_memory *mem) {
   wrapper_device_memory_reset(mem);
   list_del(&mem->link);
   vk_free2(&mem->device->vk.alloc, mem->alloc, mem);
}

static struct wrapper_device_memory *
wrapper_device_memory_from_handle(struct wrapper_device *device, VkDeviceMemory handle) {
   struct wrapper_device_memory *mem = NULL;
   simple_mtx_lock(&device->resource_mutex);
   list_for_each_entry(struct wrapper_device_memory, data, &device->device_memory_list, link) {
      if (data->dispatch_handle == handle) {
         mem = data;
         break;
      }
   }
   simple_mtx_unlock(&device->resource_mutex);
   return mem;
}

/* ---- Control de caché y WRAPPER_SYNC (0 = desactivado, 1 = activado) ---------- */
#if defined(__aarch64__)
static void
wrapper_cache_maintain(void *addr, size_t size, bool invalidate)
{
   // Comprobación opcional de WRAPPER_SYNC (si está en 0, se puede omitir o modular)
   static int wrapper_sync_enabled = -1;
   if (wrapper_sync_enabled == -1) {
      const char *env = getenv("WRAPPER_SYNC");
      wrapper_sync_enabled = env ? atoi(env) : 0; // Por defecto desactivado (0) para ganar rendimiento
   }
   if (!wrapper_sync_enabled) return;

   uint64_t ctr;
   __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
   size_t line = (size_t) 4 << ((ctr >> 16) & 0xf);
   uintptr_t p = (uintptr_t) addr & ~((uintptr_t) line - 1);
   uintptr_t end = (uintptr_t) addr + size;

   __asm__ volatile("dsb ish" ::: "memory");
   if (invalidate) {
      for (; p < end; p += line)
         __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
   } else {
      for (; p < end; p += line)
         __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
   }
   __asm__ volatile("dsb sy" ::: "memory");
}
#else
static void
wrapper_cache_maintain(void *addr, size_t size, bool invalidate)
{
   (void) addr; (void) size; (void) invalidate;
}
#endif

static VkResult
wrapper_mapped_ranges(VkDevice _device, uint32_t count, const VkMappedMemoryRange *ranges, bool invalidate)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < count; i++) {
      const VkMappedMemoryRange *r = &ranges[i];
      struct wrapper_device_memory *mem = wrapper_device_memory_from_handle(device, r->memory);
      if (!mem) {
         VkResult res = invalidate
            ? device->dispatch_table.InvalidateMappedMemoryRanges(device->dispatch_handle, 1, r)
            : device->dispatch_table.FlushMappedMemoryRanges(device->dispatch_handle, 1, r);
         if (res != VK_SUCCESS) result = res;
         continue;
      }
      VkDeviceSize sz = r->size;
      if (sz == VK_WHOLE_SIZE)
         sz = (r->offset < mem->map_size) ? mem->map_size - r->offset : 0;
      if (mem->map_address && sz) {
         wrapper_cache_maintain((char *) mem->map_address + r->offset, (size_t) sz, invalidate);
      }
   }
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_FlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
   return wrapper_mapped_ranges(device, memoryRangeCount, pMemoryRanges, false);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_InvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
   return wrapper_mapped_ranges(device, memoryRangeCount, pMemoryRanges, true);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_AllocateMemory(VkDevice _device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_device_memory *mem;
   VkResult result;
   VkMemoryAllocateInfo coherent_allocate_info;

   if (device->physical->coherent_memory_type[pAllocateInfo->memoryTypeIndex] != pAllocateInfo->memoryTypeIndex) {
      coherent_allocate_info = *pAllocateInfo;
      coherent_allocate_info.memoryTypeIndex = device->physical->coherent_memory_type[pAllocateInfo->memoryTypeIndex];
      pAllocateInfo = &coherent_allocate_info;
   }

   VkMemoryPropertyFlags property_flags = device->physical->memory_properties.memoryTypes[pAllocateInfo->memoryTypeIndex].propertyFlags;
   
   if (!(property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) goto fallback;
   if (!device->vk.enabled_features.memoryMapPlaced || !device->vk.enabled_extensions.EXT_map_memory_placed) goto fallback;
      
   if (vk_find_struct_const(pAllocateInfo, IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID) ||
       vk_find_struct_const(pAllocateInfo, IMPORT_MEMORY_FD_INFO_KHR) ||
       vk_find_struct_const(pAllocateInfo, EXPORT_MEMORY_ALLOCATE_INFO))
      goto fallback;

   const VkMemoryDedicatedAllocateInfo *dedicated_allocate_info =
         vk_find_struct_const((void*) pAllocateInfo->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   
   static int bypass_swapchains = -1;
   if (bypass_swapchains == -1) 
      bypass_swapchains = getenv("WRAPPER_BYPASS_SWAPCHAIN_PLACED") ? atoi(getenv("WRAPPER_BYPASS_SWAPCHAIN_PLACED")) : 1; // Activado por defecto para evitar pantalla negra en Mali

   if (bypass_swapchains && dedicated_allocate_info && dedicated_allocate_info->image != VK_NULL_HANDLE) {
      struct wrapper_image *img = get_wrapper_image_from_handle(device, dedicated_allocate_info->image);
      if (img && img->is_wsi_image) {
         goto fallback;
      }
   }

   simple_mtx_lock(&device->resource_mutex);
   result = wrapper_device_memory_create(device, pAllocator, &mem);
   if (result != VK_SUCCESS) {
      simple_mtx_unlock(&device->resource_mutex);
      goto fallback; // Fallback seguro en vez de crashar
   }
   mem->alloc_size = pAllocateInfo->allocationSize;

   // Intento de asignación por backend seguro
   result = wrapper_allocate_memory_dmaheap(device, pAllocateInfo, pAllocator, &mem->dispatch_handle, &mem->fd);
   if (result != VK_SUCCESS) {
      wrapper_device_memory_destroy(mem);
      simple_mtx_unlock(&device->resource_mutex);
      goto fallback; // Si falla DMA-BUF, deriva a la asignación estándar de Vulkan
   }

   *pMemory = mem->dispatch_handle;
   simple_mtx_unlock(&device->resource_mutex);
   return VK_SUCCESS;

fallback:
   result = device->dispatch_table.AllocateMemory(device->dispatch_handle, pAllocateInfo, pAllocator, pMemory);
   if (result == VK_SUCCESS && !(property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      simple_mtx_lock(&device->resource_mutex);
      _mesa_hash_table_u64_insert(device->device_only_memory, (uint64_t)*pMemory, device);
      simple_mtx_unlock(&device->resource_mutex);
   }
   return result;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_FreeMemory(VkDevice _device, VkDeviceMemory _memory, const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_device_memory *mem = wrapper_device_memory_from_handle(device, _memory);
   if (mem) {
      mem->alloc = pAllocator;
      return wrapper_device_memory_destroy(mem);
   }
   simple_mtx_lock(&device->resource_mutex);
   _mesa_hash_table_u64_remove(device->device_only_memory, (uint64_t)_memory);
   simple_mtx_unlock(&device->resource_mutex);
   device->dispatch_table.FreeMemory(device->dispatch_handle, _memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_MapMemory2KHR(VkDevice _device, const VkMemoryMapInfoKHR* pMemoryMapInfo, void** ppData)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   const VkMemoryMapPlacedInfoEXT *placed_info = NULL;
   struct wrapper_device_memory *mem;
   int fd;

   if (pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT)
      placed_info = vk_find_struct_const(pMemoryMapInfo->pNext, MEMORY_MAP_PLACED_INFO_EXT);
   
   mem = wrapper_device_memory_from_handle(device, pMemoryMapInfo->memory);
   if (!placed_info || !mem) {
      return device->dispatch_table.MapMemory(device->dispatch_handle, pMemoryMapInfo->memory, pMemoryMapInfo->offset, pMemoryMapInfo->size, 0, ppData);
   }

   simple_mtx_lock(&device->resource_mutex);
   if (mem->map_address) {
      simple_mtx_unlock(&device->resource_mutex);
      *ppData = (char *)mem->map_address + pMemoryMapInfo->offset;
      return VK_SUCCESS;
   }

   fd = mem->fd;
   if (fd < 0) {
      simple_mtx_unlock(&device->resource_mutex);
      return VK_ERROR_MEMORY_MAP_FAILED;
   }

   mem->map_size = (pMemoryMapInfo->size == VK_WHOLE_SIZE) ? mem->alloc_size : pMemoryMapInfo->size;

   // PROTECCIÓN DE MAPEO: Si mmap con MAP_FIXED falla en Mali, intentamos un mmap seguro sin posición fija estricta
   mem->map_address = mmap(placed_info->pPlacedAddress, mem->map_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
   if (mem->map_address == MAP_FAILED) {
      mem->map_address = mmap(NULL, mem->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); // Fallback flexible
      if (mem->map_address == MAP_FAILED) {
         mem->map_address = NULL;
         mem->map_size = 0;
         simple_mtx_unlock(&device->resource_mutex);
         return VK_ERROR_MEMORY_MAP_FAILED;
      }
   }

   simple_mtx_unlock(&device->resource_mutex);
   *ppData = (char *)mem->map_address + pMemoryMapInfo->offset;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_UnmapMemory(VkDevice _device, VkDeviceMemory _memory) {
   vk_common_UnmapMemory(_device, _memory);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_UnmapMemory2KHR(VkDevice _device, const VkMemoryUnmapInfoKHR* pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   struct wrapper_device_memory *mem = wrapper_device_memory_from_handle(device, pMemoryUnmapInfo->memory);
   if (!mem) {
      device->dispatch_table.UnmapMemory(device->dispatch_handle, pMemoryUnmapInfo->memory);
      return VK_SUCCESS;
   }
   if (mem->map_address) {
      munmap(mem->map_address, mem->map_size);
      mem->map_address = NULL;
      mem->map_size = 0;
   }
   return VK_SUCCESS;
}
