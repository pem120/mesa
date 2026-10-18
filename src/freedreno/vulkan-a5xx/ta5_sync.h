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
 * \file ta5_sync.h
 *
 * Adreno 5xx Vulkan driver: fences, events and semaphores.
 */

#ifndef TA5_SYNC_H
#define TA5_SYNC_H

#include <stdint.h>

#include "util/u_atomic.h"

#include "vk_object.h"
#include "vk_device.h"
#include "vk_sync.h"
#include "vk_fence.h"

#include "ta5_instance.h"

/**
 * Fence.
 *
 * Backed by a DRM syncobj via the Vulkan runtime's generic sync abstraction.
 * That matters because the kernel fence created by a submission has to be
 * waited on from userspace, and syncobj is the only mechanism the msm driver
 * supports for that.  A userspace-only atomic would work for an empty
 * submission but would break the moment a real fence FD comes back from
 * MSM_GEM_NEW.
 */
/**
 * A fence.
 *
 * The runtime's struct vk_fence is embedded rather than copied field by field,
 * and it has to be: libvulkan_wsi hands our VkFence handle straight back to
 * vk_fence_reset_temporary(), which reads fence->temporary (vk_fence.c:170)
 * and destroys it if non-NULL.
 *
 * With a private layout that read landed on our `device` pointer, which is
 * always non-NULL.  wsi_common therefore believed every fence carried
 * temporary state, treated the *device* pointer as a struct vk_sync *, and
 * called through its garbage `type->finish`.  Headless vkmark segfaulted on
 * the first frame inside ta5_GetDeviceQueue, having jumped through a pointer
 * that happened to resolve into this driver.
 *
 * There is deliberately nothing of our own in this struct.  Both edges of the
 * runtime's layout leave no room:
 *
 *  - VK_DEFINE_NONDISP_HANDLE_CASTS asserts that the object base sits at
 *    offset 0, so nothing can precede it.
 *  - vk_fence::permanent must be the final member (vk_fence.h:52) because the
 *    sync implementation appends private state after it: vk_drm_syncobj is a
 *    struct vk_sync plus a trailing syncobj (vk_drm_syncobj.h:34), and
 *    vk_sync_init writes sync_type->size bytes rather than
 *    sizeof(struct vk_sync) (vk_sync.c:99).  Fields placed after the embedded
 *    vk_fence were therefore being overwritten by fence creation -- which is
 *    how `device` and `signaled` were clobbered, after which
 *    ta5_GetFenceStatus() dereferenced fence->device through the mangled value.
 *
 * ta5_CreateFence allocates offsetof(struct ta5_fence, base.permanent) +
 * sync_type.size, matching vk_fence_create() (vk_fence.c:126), so that
 * variable-size tail has room.  Any driver state a fence needs has to live
 * elsewhere -- the device is reachable through the runtime's device pointer.
 */
struct ta5_fence
{
   struct vk_fence base;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_fence, base.base, VkFence,
                               VK_OBJECT_TYPE_FENCE)

/**
 * Host event.
 *
 * A5xx has no host-visible event state; the GL backend implements host
 * events with an ordinary atomic in a suballocated buffer.  Until command
 * buffers can write to memory, nothing signalled from the GPU can reach this,
 * so the CPU-side state is the whole object.
 */
struct ta5_event
{
   struct vk_object_base base;
   struct ta5_device *device;
   bool signaled;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_event, base, VkEvent, VK_OBJECT_TYPE_EVENT)

/*
 * Semaphores have no struct here on purpose.  They used to be a private
 * {vk_object_base, device, counter} object, but wsi_signal_semaphore_for_image()
 * casts the handle to the runtime's struct vk_semaphore and writes through its
 * temporary payload -- which in the private layout was our `device` and
 * `counter` fields.  The same class of bug as ta5_fence, one object along.
 * ta5_CreateSemaphore and friends now forward to vk_common_*.
 */

#endif /* TA5_SYNC_H */