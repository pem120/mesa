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
 * \file ta5_queue.cc
 *
 * Adreno 5xx Vulkan driver: queues and queue submission.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

#include "util/u_math.h"
#include "util/list.h"
#include "util/u_atomic.h"
#include "util/os_time.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_sync.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_sync.h"
#include "ta5_cmd_buffer.h"
#include "ta5_queue.h"
#include "ta5_submit.h"
#include "ta5_entrypoints.h"

#define TA5_DBG(fmt, ...)                                                  \
   do {                                                                    \
      if (getenv("TA5_TRACE_NULL"))                                        \
         fprintf(stderr, "ta5[queue]: " fmt "\n", ##__VA_ARGS__);          \
   } while (0)

/* ------------------------------------------------------------------ */
/* Queue objects                                                        */
/* ------------------------------------------------------------------ */

VKAPI_ATTR void VKAPI_CALL
ta5_GetDeviceQueue(VkDevice _device, uint32_t queueFamilyIndex,
                   uint32_t queueIndex, VkQueue *pQueue)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_queue *queue;

   /*
    * The spec requires the same handle for the same (family, index).  Look
    * for it before creating one.
    */
   list_for_each_entry(struct ta5_queue, queue, &device->queues, link) {
      if (queue->family == queueFamilyIndex && queue->index == queueIndex) {
         *pQueue = ta5_queue_to_handle(queue);
         return;
      }
   }

   queue = (struct ta5_queue *)calloc(1, sizeof(*queue));
   if (!queue) {
      *pQueue = VK_NULL_HANDLE;
      return;
   }

   vk_object_base_init(&device->vk, &queue->base, VK_OBJECT_TYPE_QUEUE);
   queue->device = device;
   queue->family = queueFamilyIndex;
   queue->index = queueIndex;
   queue->idle_fence_fd = -1;
   list_addtail(&queue->link, &device->queues);

   *pQueue = ta5_queue_to_handle(queue);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetDeviceQueue2(VkDevice _device, const VkDeviceQueueInfo2 *pQueueInfo,
                    VkQueue *pQueue)
{
   ta5_GetDeviceQueue(_device, pQueueInfo->queueFamilyIndex,
                      pQueueInfo->queueIndex, pQueue);
}

/* ------------------------------------------------------------------ */
/* Submission                                                           */
/* ------------------------------------------------------------------ */

/**
 * Issue one batch and hand the resulting kernel fence back.
 *
 * Each VkSubmitInfo in a vkQueueSubmit call is submitted separately: a5xx has
 * no single command stream spanning batches, and the kernel takes one
 * MSM_SUBMIT_CMD_BUF per submission.
 */
static VkResult
ta5_queue_submit_batch(struct ta5_queue *queue, const VkSubmitInfo *submit,
                       VkFence fence)
{
   struct ta5_device *device = queue->device;
   int fence_fd;
   uint32_t *stream;
   uint32_t total_words = 0, n = 0;

   for (uint32_t i = 0; i < submit->commandBufferCount; i++) {
      struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(
         submit->pCommandBuffers[i]);

      /* Submitting a command buffer that is still recording is undefined
       * behaviour per the Vulkan spec; assert rather than invent a result
       * code, matching Turnip. */
      assert(cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING);

      total_words += cmd->cs_count;
   }

   /*
    * An empty submission has nothing to put on the ring.  There is no valid
    * a5xx PM4 packet that does nothing, and inventing one faults the CP, so
    * the work is simply already complete: signal the fence from the CPU and
    * skip the GPU entirely.
    */
   if (total_words == 0) {
      if (fence != VK_NULL_HANDLE) {
         struct ta5_fence *ta5_fence = ta5_fence_from_handle(fence);
         VkResult result = vk_sync_signal(&device->vk, ta5_fence->sync, 1);
         if (result != VK_SUCCESS)
            return result;
      }
      return VK_SUCCESS;
   }

   stream = (uint32_t *)malloc(total_words * sizeof(uint32_t));
   if (!stream)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (uint32_t i = 0; i < submit->commandBufferCount; i++) {
      struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(
         submit->pCommandBuffers[i]);
      memcpy(&stream[n], cmd->cs, cmd->cs_count * sizeof(uint32_t));
      n += cmd->cs_count;
   }

   fence_fd = ta5_submit_cmd(queue, stream, total_words, 0);
   free(stream);

   if (fence_fd < 0)
      return vk_error(&device->instance->vk, VK_ERROR_DEVICE_LOST);

   /* Mark the command buffers pending so they cannot be resubmitted. */
   for (uint32_t i = 0; i < submit->commandBufferCount; i++) {
      struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(
         submit->pCommandBuffers[i]);
      cmd->state = MESA_VK_COMMAND_BUFFER_STATE_PENDING;
   }

   /*
    * Keep a copy of the kernel fence so vkQueueWaitIdle has something to
    * wait on.  The original fd is consumed by the import below.
    */
   if (queue->idle_fence_fd >= 0)
      close(queue->idle_fence_fd);
   queue->idle_fence_fd = dup(fence_fd);

   /* Import the kernel fence into the Vulkan fence so vkWaitForFences works. */
   if (fence != VK_NULL_HANDLE) {
      struct ta5_fence *ta5_fence = ta5_fence_from_handle(fence);
      VkResult result =
         vk_sync_import_opaque_fd(&device->vk, ta5_fence->sync, fence_fd);
      TA5_DBG("fence import: fd %d -> %d (errno %d: %s)", fence_fd, result,
             errno, strerror(errno));
      if (result != VK_SUCCESS) {
         close(fence_fd);
         return vk_error(&device->instance->vk, result);
      }
   }

   close(fence_fd);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_QueueSubmit(VkQueue _queue, uint32_t submitCount,
                const VkSubmitInfo *pSubmits, VkFence fence)
{
   struct ta5_queue *queue = ta5_queue_from_handle(_queue);

   for (uint32_t i = 0; i < submitCount; i++) {
      VkResult result = ta5_queue_submit_batch(queue, &pSubmits[i],
                                               /* only the last batch signals
                                                * the caller's fence */
                                               (i == submitCount - 1) ? fence
                                                                    : VK_NULL_HANDLE);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_QueueSubmit2(VkQueue _queue, uint32_t submitCount,
                 const VkSubmitInfo2 *pSubmits, VkFence fence)
{
   struct ta5_queue *queue = ta5_queue_from_handle(_queue);

   for (uint32_t i = 0; i < submitCount; i++) {
      const VkSubmitInfo *info = (const VkSubmitInfo *)&pSubmits[i];
      VkResult result =
         ta5_queue_submit_batch(queue, info,
                                (i == submitCount - 1) ? fence : VK_NULL_HANDLE);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_QueueWaitIdle(VkQueue _queue)
{
   struct ta5_queue *queue = ta5_queue_from_handle(_queue);

   /*
    * Wait on the kernel fence kept from the last submission.  A DRM sync fd
    * becomes readable once the work it guards has completed, so poll() is
    * enough and avoids importing a sync object we would immediately drop.
    */
   while (queue->idle_fence_fd >= 0) {
      struct pollfd pfd = {
         .fd = queue->idle_fence_fd,
         .events = POLLIN,
      };
      int ret = poll(&pfd, 1, -1);

      if (ret < 0) {
         if (errno == EINTR)
            continue;
         TA5_DBG("poll on fence failed: %s", strerror(errno));
         close(queue->idle_fence_fd);
         queue->idle_fence_fd = -1;
         return vk_error(&queue->device->instance->vk, VK_ERROR_DEVICE_LOST);
      }

      /* The last submission is the only one whose fence is retained, and it
       * signals last, so one pass is enough. */
      close(queue->idle_fence_fd);
      queue->idle_fence_fd = -1;
      break;
   }

   queue->pending = 0;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_DeviceWaitIdle(VkDevice _device)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   VkResult result;

   list_for_each_entry(struct ta5_queue, queue, &device->queues, link) {
      result = ta5_QueueWaitIdle(ta5_queue_to_handle(queue));
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_QueueBindSparse(VkQueue _queue, uint32_t bindInfoCount,
                    const VkBindSparseInfo *pBindInfo, VkFence fence)
{
   /* Sparse binding is not supported; nothing in this phase allocates sparsely. */
   return VK_SUCCESS;
}