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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file ta5_sync.cc
 *
 * Adreno 5xx Vulkan driver: fences, events and semaphores.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_math.h"
#include "util/u_atomic.h"
#include "util/os_time.h"
#include "vk_alloc.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_device.h"
#include "vk_sync.h"
#include "vk_drm_syncobj.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_sync.h"
#include "ta5_entrypoints.h"

/* ------------------------------------------------------------------ */
/* Fences                                                              */
/* ------------------------------------------------------------------ */

/*
 * DRM syncobj sync type for this device.  a5xx on mainline msm does not use
 * userspace-managed VM binding (freedreno never sets MSM_PARAM_EN_VM_BIND), so
 * the kernel hands back a fence FD from every submission and syncobj is the
 * only thing that can wait on it.
 */
static struct vk_sync_type ta5_syncobj_type_storage;
static const struct vk_sync_type *ta5_syncobj_type;

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateFence(VkDevice _device, const VkFenceCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkFence *pFence)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_fence *fence;
   VkResult result;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   fence = (struct ta5_fence *)vk_zalloc(pAllocator, sizeof(*fence), 8,
                                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!fence)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &fence->base, VK_OBJECT_TYPE_FENCE);
   fence->device = device;

   if (!ta5_syncobj_type) {
      ta5_syncobj_type_storage = vk_drm_syncobj_get_type(device->fd);
      ta5_syncobj_type = &ta5_syncobj_type_storage;
   }

   /*
    * DRM syncobj is used because the msm kernel hands back a fence FD from
    * every submission, and syncobj is the only sync type the driver supports
    * for that.  A created-with-signaled fence starts at value 1, otherwise
    * at 0.
    */
   result = vk_sync_create(&device->vk, ta5_syncobj_type,
                           (enum vk_sync_flags)0, 0, &fence->sync);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, fence);
      return vk_error(&device->instance->vk, result);
   }

   fence->signaled = false;

   /* A binary sync always starts unsignalled; raise it if asked. */
   if (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) {
      result = vk_sync_signal(&device->vk, fence->sync, 1);
      if (result != VK_SUCCESS) {
         vk_sync_destroy(&device->vk, fence->sync);
         vk_free(pAllocator, fence);
         return vk_error(&device->instance->vk, result);
      }
      fence->signaled = true;
   }

   *pFence = ta5_fence_to_handle(fence);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyFence(VkDevice _device, VkFence _fence,
                 const VkAllocationCallbacks *pAllocator)
{
   struct ta5_fence *fence = ta5_fence_from_handle(_fence);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_sync_destroy(&fence->device->vk, fence->sync);
   vk_free(pAllocator, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_ResetFences(VkDevice _device, uint32_t fenceCount, const VkFence *pFences)
{
   struct ta5_device *device = (struct ta5_device *)_device;

   for (uint32_t i = 0; i < fenceCount; i++) {
      struct ta5_fence *fence = ta5_fence_from_handle(pFences[i]);
      VkResult result = vk_sync_reset(&device->vk, fence->sync);

      if (result != VK_SUCCESS)
         return vk_error(&device->instance->vk, result);

      fence->signaled = false;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetFenceStatus(VkDevice _device, VkFence _fence)
{
   struct ta5_fence *fence = ta5_fence_from_handle(_fence);
   VkResult result;

   /*
    * A binary sync has no readable counter -- vk_sync_get_value() is only
    * valid for timelines.  Poll instead, with an already-expired timeout so
    * this never blocks.
    */
   result = vk_sync_wait(&fence->device->vk, fence->sync, 0,
                         VK_SYNC_WAIT_COMPLETE, os_time_get_absolute_timeout(0));
   if (result != VK_SUCCESS && result != VK_TIMEOUT)
      return result;

   fence->signaled = (result == VK_SUCCESS);
   return fence->signaled ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_WaitForFences(VkDevice _device, uint32_t fenceCount, const VkFence *pFences,
                  VkBool32 waitAll, uint64_t timeout)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   VkResult result;

   if (fenceCount == 0)
      return VK_SUCCESS;

   if (fenceCount == 1) {
      struct ta5_fence *fence = ta5_fence_from_handle(pFences[0]);

      result = vk_sync_wait(
         &device->vk, fence->sync, 0, VK_SYNC_WAIT_COMPLETE,
         os_time_get_absolute_timeout(timeout ? timeout * 1000000 : UINT64_MAX));
      if (result == VK_SUCCESS)
         fence->signaled = true;
      return result;
   }

   /* Multi-fence waits use the generic path so waitAll/waitAny and timeouts
    * are handled in one place. */
   return vk_common_WaitForFences(_device, fenceCount, pFences, waitAll,
                                  timeout);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateEvent(VkDevice _device, const VkEventCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkEvent *pEvent)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_event *event;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   event = (struct ta5_event *)vk_zalloc(pAllocator, sizeof(*event), 8,
                                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!event)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &event->base, VK_OBJECT_TYPE_EVENT);
   event->device = device;
   p_atomic_set(&event->signaled, false);

   *pEvent = ta5_event_to_handle(event);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyEvent(VkDevice _device, VkEvent _event,
                 const VkAllocationCallbacks *pAllocator)
{
   struct ta5_event *event = ta5_event_from_handle(_event);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_free(pAllocator, event);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetEventStatus(VkDevice _device, VkEvent _event)
{
   struct ta5_event *event = ta5_event_from_handle(_event);

   return p_atomic_read(&event->signaled) ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_SetEvent(VkDevice _device, VkEvent _event)
{
   struct ta5_event *event = ta5_event_from_handle(_event);

   p_atomic_set(&event->signaled, true);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_ResetEvent(VkDevice _device, VkEvent _event)
{
   struct ta5_event *event = ta5_event_from_handle(_event);

   p_atomic_set(&event->signaled, false);
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Semaphores                                                          */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateSemaphore(VkDevice _device, const VkSemaphoreCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkSemaphore *pSemaphore)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_semaphore *sem;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   sem = (struct ta5_semaphore *)vk_zalloc(pAllocator, sizeof(*sem), 8,
                                           VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!sem)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &sem->base, VK_OBJECT_TYPE_SEMAPHORE);
   sem->device = device;
   p_atomic_set(&sem->counter, 0u);

   *pSemaphore = ta5_semaphore_to_handle(sem);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroySemaphore(VkDevice _device, VkSemaphore _semaphore,
                     const VkAllocationCallbacks *pAllocator)
{
   struct ta5_semaphore *sem = ta5_semaphore_from_handle(_semaphore);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_free(pAllocator, sem);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetSemaphoreCounterValue(VkDevice _device, VkSemaphore _semaphore,
                             uint64_t *pValue)
{
   struct ta5_semaphore *sem = ta5_semaphore_from_handle(_semaphore);

   *pValue = p_atomic_read(&sem->counter);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_SignalSemaphore(VkDevice _device, const VkSemaphoreSignalInfo *pSignalInfo)
{
   struct ta5_semaphore *sem = ta5_semaphore_from_handle(pSignalInfo->semaphore);

   p_atomic_add(&sem->counter, pSignalInfo->value ? pSignalInfo->value : 1);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_WaitSemaphores(VkDevice _device, const VkSemaphoreWaitInfo *pWaitInfo,
                   uint64_t timeout)
{
   if (pWaitInfo->semaphoreCount == 0)
      return VK_SUCCESS;

   /*
    * Semaphores are host-counted in this phase.  If every wait is already
    * satisfied the call is a no-op; otherwise it would have to block on GPU
    * work that does not exist yet, so report the timeout rather than spin.
    */
   for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; i++) {
      struct ta5_semaphore *sem =
         ta5_semaphore_from_handle(pWaitInfo->pSemaphores[i]);
      uint64_t want = pWaitInfo->pValues ? pWaitInfo->pValues[i] : 1;
      uint64_t have = p_atomic_read(&sem->counter);

      if (have < want)
         return timeout ? VK_TIMEOUT : VK_ERROR_DEVICE_LOST;
   }

   for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; i++) {
      struct ta5_semaphore *sem =
         ta5_semaphore_from_handle(pWaitInfo->pSemaphores[i]);
      uint64_t want = pWaitInfo->pValues ? pWaitInfo->pValues[i] : 1;

      p_atomic_add(&sem->counter, -(int64_t)want);
   }

   return VK_SUCCESS;
}