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
#include "vk_fence.h"
#include "ta5_entrypoints.h"

/* ------------------------------------------------------------------ */
/* Fences                                                              */
/* ------------------------------------------------------------------ */

/*
 * DRM syncobj sync type.  a5xx on mainline msm does not use userspace-managed
 * VM binding (freedreno never sets MSM_PARAM_EN_VM_BIND), so the kernel hands
 * back a fence FD from every submission and syncobj is the only thing that can
 * wait on it.
 *
 * The type itself lives on ta5_device and is probed once in ta5_CreateDevice
 * -- see the note on ta5_device::sync_type for why it is not a lazily
 * initialised global.
 */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateFence(VkDevice _device, const VkFenceCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkFence *pFence)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_fence *fence;
   VkResult result;
   size_t size;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   /*
    * The size is not sizeof(*fence).  vk_sync_init zeroes and initialises
    * sync_type->size bytes at &fence->base.permanent, and for DRM syncobj that
    * is a struct vk_sync plus a trailing syncobj handle
    * (vk_drm_syncobj.h:34) -- larger than the struct vk_sync we embedded.
    * Allocating only sizeof(struct ta5_fence) both overflowed that write and
    * left our trailing fields inside the region being initialised.  This is
    * the same arithmetic vk_fence_create() uses (vk_fence.c:126); base is the
    * last member precisely so that offsetof() gives the right start.
    */
   size = offsetof(struct ta5_fence, base.permanent) + device->sync_type.size;
   fence = (struct ta5_fence *)vk_zalloc(pAllocator, size, 8,
                                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!fence)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &fence->base.base, VK_OBJECT_TYPE_FENCE);

   /*
    * DRM syncobj is used because the msm kernel hands back a fence FD from
    * every submission, and syncobj is the only sync type the driver supports
    * for that.  A created-with-signaled fence starts at value 1, otherwise
    * at 0.
    */
   /* vk_sync_init, not vk_sync_create: vk_fence::permanent is embedded by
    * value, and vk_sync_create allocates a struct vk_sync instead. */
   result = vk_sync_init(&device->vk, &fence->base.permanent,
                         &device->sync_type, (enum vk_sync_flags)0, 0);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, fence);
      return vk_error(&device->instance->vk, result);
   }

   /* A binary sync always starts unsignalled; raise it if asked.  This is
    * vk_sync_init(..., initial_value=1) done the long way because the sync has
    * to exist before it can be signalled; vk_fence_create() has the same shape.
    */
   if (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) {
      result = vk_sync_signal(&device->vk, &fence->base.permanent, 1);
      if (result != VK_SUCCESS) {
         vk_sync_finish(&device->vk, &fence->base.permanent);
         vk_free(pAllocator, fence);
         return vk_error(&device->instance->vk, result);
      }
   }

   *pFence = ta5_fence_to_handle(fence);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyFence(VkDevice _device, VkFence _fence,
                 const VkAllocationCallbacks *pAllocator)
{
   struct ta5_fence *fence = ta5_fence_from_handle(_fence);

   /* vkDestroyFence(VK_NULL_HANDLE) is legal and must be a no-op; wsi_common
    * unwinds partially-built swapchains by destroying resources it may never
    * have created. */
   if (fence == NULL)
      return;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   /*
    * Reset the temporary sync before finishing the permanent one, exactly as
    * vk_fence_destroy() does (vk_fence.c:182).  libvulkan_wsi attaches one in
    * wsi_signal_fence_for_image() -- a dma-buf wait -- and it has to be
    * released or the sync object outlives the fence.  The device comes from
    * _device rather than any field of our own, which no longer exists.
    */
   vk_fence_reset_temporary((struct vk_device *)_device, &fence->base);
   vk_sync_finish((struct vk_device *)_device, &fence->base.permanent);
   vk_free(pAllocator, fence);
}

/*
 * The three entry points below delegate to the runtime rather than open-coding
 * them, and that is a correctness fix rather than an economy.
 *
 * Each used to wait on fence->base.permanent directly.  But
 * wsi_signal_fence_for_image() installs a *temporary* sync on the fence for the
 * acquire (a dma-buf wait, or a syncobj), and the active payload of a fence is
 * whichever of the two is present -- the runtime expresses that as
 * vk_fence_get_active_sync() (vk_fence.c:239, vk_fence.c:273).  A fence carrying
 * imported acquire state therefore reported unsignalled forever, because the
 * permanent syncobj it was actually waiting on was never the one being polled.
 *
 * vk_common_ResetFences additionally restores the prior permanent payload
 * before resetting, which the spec requires (vk_fence.c:212-219).
 *
 * A binary sync still has no readable counter, so both waits remain polls on a
 * syncobj rather than counter reads; that part was right.  VkFence::timeout is
 * in nanoseconds, matching os_time_get_absolute_timeout() -- scaling it here
 * would wait 1000x too long, and collapsing 0 into UINT64_MAX turns the usual
 * zero-timeout poll into a hang.
 */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_ResetFences(VkDevice _device, uint32_t fenceCount, const VkFence *pFences)
{
   return vk_common_ResetFences(_device, fenceCount, pFences);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetFenceStatus(VkDevice _device, VkFence _fence)
{
   return vk_common_GetFenceStatus(_device, _fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_WaitForFences(VkDevice _device, uint32_t fenceCount, const VkFence *pFences,
                  VkBool32 waitAll, uint64_t timeout)
{
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
   /* vkDestroyEvent(VK_NULL_HANDLE) is a no-op.
    *
    * libvulkan_wsi tears a swapchain down by calling the destroy entry points
    * unconditionally, including on handles that were never created -- see
    * wsi_destroy_image(), which passes image->blit.image and
    * image->blit.buffer straight through (wsi_common.c:1067-1070).  The spec
    * requires every destroy and free entry point to accept a null handle
    * and do nothing, and without the guard teardown faults on a null image.
    */
   if (_event == NULL)
      return;


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

/*
 * These delegate to the runtime, and that fixes a NULL entry point as well as
 * wrong semantics.
 *
 * wsi_device_init() pulls vkWaitSemaphores out of the device proc addr with
 * WSI_GET_CB() (wsi_common.c:188-191), which is a bare cast with no NULL
 * check, so a missing entry point silently becomes a call through address 0.
 * Sweeping the whole struct at swapchain-creation time found it the only one
 * of 37 still missing:
 *
 *     CHECKED 37 entry points: 36 present, 1 NULL
 *       NULL vkWaitSemaphores
 *
 * The hand-written versions were also wrong in ways that only a missing
 * feature test would have caught:
 *
 *  - ta5_SignalSemaphore added to the counter instead of setting it, so
 *    signalling a timeline semaphore twice did not reach the requested value.
 *  - Creation ignored VkSemaphoreTypeCreateInfo entirely and always started at
 *    zero, so timeline semaphores could not be given an initial value and
 *    binary semaphores were indistinguishable from timelines.
 *  - ta5_WaitSemaphores consumed the counter on a host wait and ignored
 *    pWaitAll/any and the timeout entirely -- it returned VK_TIMEOUT for any
 *    unsignalled semaphore rather than blocking.
 *
 * vk_common_* gets all of this from the same sync abstraction the fences use,
 * which is wired up because the physical device publishes
 * supported_sync_types before wsi_device_init() runs.
 */
VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateSemaphore(VkDevice _device, const VkSemaphoreCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkSemaphore *pSemaphore)
{
   return vk_common_CreateSemaphore(_device, pCreateInfo, pAllocator,
                                    pSemaphore);
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroySemaphore(VkDevice _device, VkSemaphore _semaphore,
                     const VkAllocationCallbacks *pAllocator)
{
   vk_common_DestroySemaphore(_device, _semaphore, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetSemaphoreCounterValue(VkDevice _device, VkSemaphore _semaphore,
                             uint64_t *pValue)
{
   return vk_common_GetSemaphoreCounterValue(_device, _semaphore, pValue);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_SignalSemaphore(VkDevice _device, const VkSemaphoreSignalInfo *pSignalInfo)
{
   return vk_common_SignalSemaphore(_device, pSignalInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_WaitSemaphores(VkDevice _device, const VkSemaphoreWaitInfo *pWaitInfo,
                   uint64_t timeout)
{
   return vk_common_WaitSemaphores(_device, pWaitInfo, timeout);
}