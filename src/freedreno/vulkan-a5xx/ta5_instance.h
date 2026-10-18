/*
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef TA5_INSTANCE_H
#define TA5_INSTANCE_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vk_object.h"
#include "vk_device.h"
#include "vk_physical_device.h"
#include "vk_sync.h"

#include "util/simple_mtx.h"
#include "wsi_common.h"

#include "ta5_device.h"

struct ta5_display;
struct ta5_display_mode;

/** a5xx exposes a single universal graphics/compute/transfer queue family. */
#define TA5_MAX_QUEUE_FAMILIES 1

struct ta5_instance
{
   struct vk_instance vk;
   uint32_t api_version;
};
VK_DEFINE_HANDLE_CASTS(ta5_instance, vk.base, VkInstance,
                       VK_OBJECT_TYPE_INSTANCE)

struct ta5_physical_device
{
   struct vk_physical_device vk;
   struct ta5_instance *instance;

   int dev_fd;
   struct fd_dev_id dev_id;
   struct ta5_gpu_info gpu_info;

   VkQueueFamilyProperties queue_props[TA5_MAX_QUEUE_FAMILIES];
   uint32_t num_queue_families;

   /*
    * a5xx has no dedicated VRAM -- buffers and images are system memory, so
    * there is a single heap sized from the host's available RAM.
    */
   uint64_t heap_size;

   char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];

   /*
    * The shared window-system-integration device.
    *
    * Wayland, KMS/display and headless surfaces and swapchains are all
    * libvulkan_wsi's, reached through the common entrypoints that
    * vk_device_init() merges in without overwriting driver-provided ones.
    * Turnip's tu_wsi.cc is 67 lines for the same thing: this struct, one
    * wsi_device_init() call, and two flags.
    *
    * master_fd is the DRM master node, which wsi_common needs for the KMS
    * path; -1 means "no KMS on this device" and is what the Wayland path
    * wants.
    */
   struct wsi_device wsi_device;
   int master_fd;

   /*
    * DRM syncobj provider, published as vk_physical_device.supported_sync_types.
    *
    * The array must be storage rather than a temporary because
    * supported_sync_types is a pointer to a NULL-terminated list of types that
    * lives as long as the physical device.
    */
   struct vk_sync_type syncobj_sync_type;
   const struct vk_sync_type *sync_types[2];
};
VK_DEFINE_HANDLE_CASTS(ta5_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

struct ir3_compiler;

struct ta5_device
{
   struct vk_device vk;
   struct ta5_instance *instance;
   struct ta5_physical_device *physical_device;
   uint32_t device_idx;

   /** The DRM render node fd opened for the physical device. */
   int fd;

   /**
    * Live queue objects, keyed by (family, index).
    *
    * vkGetDeviceQueue must return the same handle for the same family and
    * index -- applications rely on that identity, and it is what lets
    * vkDeviceWaitIdle find every queue the application has asked for.
    */
   struct list_head queues;

   /**
    * Live device-memory allocations.
    *
    * Nothing needs to walk this to make the driver work; it exists so
    * TA5_DUMP_IMAGES can report the contents of every image the application
    * created.  See ta5_dump_images().
    */
   struct list_head memories;

   /**
    * IR3 compiler, generation-gated: a5xx (gen 5) selects IR3's a4xx back
    * end, the same one the GL backend uses.  It emits CP_LOAD_STATE4, which
    * is the only state-load family this hardware has -- Turnip only speaks
    * CP_LOAD_STATE6 and cannot be shared.
    */
   struct ir3_compiler *compiler;

   /**
    * Scratch buffer for timestamped CP_EVENT_WRITE.
    *
    * A timestamped CCU_RESOLVE has to write somewhere; GL uses a dedicated
    * blit_mem allocation for it.  Allocated on first use.
    */
   struct ta5_bo *blit_bo;

   /**
    * DRM syncobj sync type for this device.
    *
    * Probed once at device create rather than lazily on first fence: a
    * process-global lazy init has two problems this avoids.  Fence and
    * semaphore creation may race across threads, and a plain `if (!ptr)`
    * check-then-set on a global `struct vk_sync_type` lets one thread publish
    * the pointer before the struct stores are visible -- a table of function
    * pointers read half-initialised.  And the probed feature bits come from
    * one fd, so a global reuses the first device's answer for every later
    * device in the process.
    */
   struct vk_sync_type sync_type;
};
VK_DEFINE_HANDLE_CASTS(ta5_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

extern const struct vk_instance_extension_table ta5_instance_extensions_supported;
extern const struct vk_device_extension_table ta5_device_extensions_supported;

VkResult ta5_wsi_init(struct ta5_physical_device *pdevice);
void ta5_wsi_finish(struct ta5_physical_device *pdevice);

VkResult ta5_physical_device_init(struct ta5_physical_device *device,
                                  struct ta5_instance *instance, int dev_fd);

VkResult ta5_physical_device_try_create(struct vk_instance *vk_instance,
                                        struct _drmDevice *drm_device,
                                        struct vk_physical_device **out);

#endif /* TA5_INSTANCE_H */
