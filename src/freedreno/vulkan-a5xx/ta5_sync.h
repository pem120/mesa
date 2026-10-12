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
struct ta5_fence
{
   struct vk_object_base base;
   struct ta5_device *device;
   struct vk_sync *sync;
   bool signaled; /**< fences start signaled when created with that flag */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_fence, base, VkFence, VK_OBJECT_TYPE_FENCE)

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

/**
 * Binary semaphore.
 *
 * Counted on the host for now.  When command buffers gain the ability to
 * signal and wait on a semaphore in PM4, the counter moves to GPU-visible
 * memory; the interface does not change.
 */
struct ta5_semaphore
{
   struct vk_object_base base;
   struct ta5_device *device;
   uint64_t counter;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_semaphore, base, VkSemaphore,
                               VK_OBJECT_TYPE_SEMAPHORE)

#endif /* TA5_SYNC_H */