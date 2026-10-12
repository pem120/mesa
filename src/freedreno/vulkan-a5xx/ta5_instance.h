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

#include "ta5_device.h"

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

   VkQueueFamilyProperties queue_props[1];

   char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
};
VK_DEFINE_HANDLE_CASTS(ta5_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

struct ta5_device
{
   struct vk_device vk;
   struct ta5_instance *instance;
   struct ta5_physical_device *physical_device;
   uint32_t device_idx;
};
VK_DEFINE_HANDLE_CASTS(ta5_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

extern const struct vk_instance_extension_table ta5_instance_extensions_supported;
extern const struct vk_device_extension_table ta5_device_extensions_supported;

VkResult ta5_physical_device_init(struct ta5_physical_device *device,
                                  struct ta5_instance *instance, int dev_fd);

VkResult ta5_physical_device_try_create(struct vk_instance *vk_instance,
                                        struct _drmDevice *drm_device,
                                        struct vk_physical_device **out);

#endif /* TA5_INSTANCE_H */