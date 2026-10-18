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
 * \file ta5_queue.h
 *
 * Adreno 5xx Vulkan driver: queue objects.
 */

#ifndef TA5_QUEUE_H
#define TA5_QUEUE_H

#include <stdint.h>

#include "util/u_atomic.h"
#include "vk_object.h"
#include "vk_device.h"
#include "vk_queue.h"

#include "ta5_instance.h"

struct ta5_bo;

/**
 * Queue.
 *
 * a5xx exposes a single universal family, so a queue is identified by
 * (family, index) and all of them submit to the same msm pipe.  Each queue
 * owns a ring buffer, because command streams must not be overwritten while
 * the GPU is still reading them.
 *
 * The runtime's struct vk_queue is embedded first, and libvulkan_wsi requires
 * that.  wsi_QueuePresentKHR casts the handle straight to struct vk_queue
 * (wsi_common.c:2930) and then indexes per-queue arrays with it:
 *
 *     image->blit.cmd_buffers[queue->queue_family_index]   wsi_common.c:2757
 *     image->timestamp_cmd_buffers[queue->queue_family_index]
 *
 * With a private layout that read landed on unrelated fields, produced an
 * out-of-range index, and vkcube segfaulted inside wsi_common_queue_present on
 * the first present.  vk_queue_init() also links the queue into
 * vk_device::queues and records the family index, so embedding it is both the
 * layout fix and the bookkeeping.
 */
struct ta5_queue
{
   struct vk_queue vk;

   struct ta5_device *device;
   uint32_t family;
   uint32_t index;

   /** Ring buffer the queue submits command streams through. */
   struct ta5_bo *ring;
   void *ring_map;
   uint32_t ring_offset;

   /** Submissions issued but not yet waited on. */
   uint32_t pending;

   /**
    * The kernel fence from the most recent submission, kept so that
    * vkQueueWaitIdle has something real to wait on.  -1 when idle.
    */
   int idle_fence_fd;

   /** Link in ta5_device::queues.
    *
    * Distinct from vk_queue::link, which the runtime uses.  This one is ours,
    * so that queue teardown can reach the ring and the idle fence without
    * walking runtime bookkeeping.
    */
   struct list_head link;
};

VK_DEFINE_HANDLE_CASTS(ta5_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

#endif /* TA5_QUEUE_H */