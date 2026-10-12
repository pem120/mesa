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

/**
 * \file ta5_device_create.cc
 *
 * Adreno 5xx Vulkan driver: logical device creation.
 *
 * This wires the Vulkan runtime's device-level dispatch table and validates
 * requested features/extensions against what a5xx supports.  Command buffer
 * and queue submission follow in later phases.
 */

#include <stdio.h>

#include "util/u_math.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_physical_device.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_entrypoints.h"

#define TA5_DBG(fmt, ...)                                                  \
   do {                                                                    \
      fprintf(stderr, "ta5: " fmt "\n", ##__VA_ARGS__);                    \
   } while (0)

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateDevice(VkPhysicalDevice physicalDevice,
                 const VkDeviceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   struct ta5_physical_device *pdevice =
      (struct ta5_physical_device *)physicalDevice;
   struct ta5_device *device;
   struct vk_device_dispatch_table dispatch_table;
   VkResult result;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   device = (struct ta5_device *)vk_zalloc(pAllocator, sizeof(*device), 8,
                                           VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(&pdevice->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   device->instance = pdevice->instance;
   device->physical_device = pdevice;

   /*
    * a5xx is a single generation, so unlike Turnip there is no per-chip
    * entrypoint table to switch on -- one table covers all a5xx parts.
    */
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &ta5_device_entrypoints, true);

   result = vk_device_init(&device->vk, &pdevice->vk, &dispatch_table,
                           pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      TA5_DBG("vk_device_init failed: %d", result);
      vk_free(pAllocator, device);
      return vk_error(&pdevice->instance->vk, result);
   }

   *pDevice = ta5_device_to_handle(device);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   struct ta5_device *device = (struct ta5_device *)_device;

   vk_device_finish(&device->vk);
   vk_free(pAllocator, device);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetDeviceQueue(VkDevice _device, uint32_t queueFamilyIndex,
                   uint32_t queueIndex, VkQueue *pQueue)
{
   /* Queues are created in the queue-submission phase; for now the device
    * exposes no queues, which is valid for an incomplete driver but will be
    * replaced before any rendering path is implemented.
    */
   (void)_device;
   (void)queueFamilyIndex;
   (void)queueIndex;
   (void)pQueue;
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetDeviceQueue2(VkDevice _device, const VkDeviceQueueInfo2 *pQueueInfo,
                    VkQueue *pQueue)
{
   (void)_device;
   (void)pQueueInfo;
   (void)pQueue;
}