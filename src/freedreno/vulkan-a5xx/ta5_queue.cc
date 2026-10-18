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
#include <alloca.h>
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
#include "vk_semaphore.h"
#include "vk_fence.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_sync.h"
#include "ta5_cmd_buffer.h"
#include <time.h>

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
ta5_queue_submit_batch(struct ta5_queue *queue, const VkCommandBuffer *cmds,
                       uint32_t cmd_count, int *out_fence_fd)
{
   struct ta5_device *device = queue->device;
   int fence_fd;
   uint32_t *stream;

   if (out_fence_fd)
      *out_fence_fd = -1;
   uint32_t total_words = 0, n = 0;

   for (uint32_t i = 0; i < cmd_count; i++) {
      struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(
         cmds[i]);

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
      /* Nothing goes to the GPU, so *out_fence_fd stays -1 and the caller
       * signals this batch's payloads straight away: there is no work in
       * flight to wait for. */
      return VK_SUCCESS;
   }

   stream = (uint32_t *)malloc(total_words * sizeof(uint32_t));
   if (!stream)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   /*
    * Concatenating the buffers shifts every relocation target, because
    * dst_off is a byte offset into the stream that now starts earlier in the
    * merged copy.  Each command buffer also indexes its own BO list, so those
    * have to be merged and the indices remapped into one table.
    */
    uint32_t total_relocs = 0, total_bo = 0, total_ibs = 0;
    for (uint32_t i = 0; i < cmd_count; i++) {
       struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(
          cmds[i]);
       total_relocs += cmd->reloc_count;
       total_bo += cmd->reloc_bo_count;
       total_ibs += cmd->ib_count;
    }

    struct ta5_reloc *merged_relocs = NULL;
    struct ta5_bo **merged_bos = NULL;
    struct ta5_ib **merged_ibs = NULL;
    uint32_t merged_bo_count = 0;

   if (total_bo) {
      merged_bos = (struct ta5_bo **)calloc(total_bo, sizeof(*merged_bos));
      merged_relocs = (struct ta5_reloc *)calloc(
         total_relocs ? total_relocs : 1, sizeof(*merged_relocs));
      if (!merged_bos || !merged_relocs) {
         free(merged_bos);
         free(merged_relocs);
         free(stream);
         return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
       }
    }

    if (total_ibs) {
       merged_ibs = (struct ta5_ib **)malloc(total_ibs * sizeof(*merged_ibs));
       if (!merged_ibs) {
          free(merged_bos);
          free(merged_relocs);
          free(stream);
          return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
       }
    }

    uint32_t reloc_n = 0;
    uint32_t ib_n = 0;
    for (uint32_t i = 0; i < cmd_count; i++) {
      struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(
         cmds[i]);

      /* Remap this buffer's BO indices into the merged table. */
      uint32_t *remap =
         (uint32_t *)alloca(cmd->reloc_bo_count * sizeof(uint32_t));
      for (uint32_t j = 0; j < cmd->reloc_bo_count; j++) {
         struct ta5_bo *bo = cmd->reloc_bos[j];
         uint32_t k = 0;
         while (k < merged_bo_count && merged_bos[k] != bo)
            k++;
         if (k == merged_bo_count)
            merged_bos[merged_bo_count++] = bo;
         remap[j] = k;
      }

      for (uint32_t j = 0; j < cmd->reloc_count; j++) {
         merged_relocs[reloc_n] = cmd->relocs[j];
         merged_relocs[reloc_n].bo_index = remap[cmd->relocs[j].bo_index];
         merged_relocs[reloc_n].dst_off += n * 4;
         reloc_n++;
      }

      memcpy(&stream[n], cmd->cs, cmd->cs_count * sizeof(uint32_t));
      n += cmd->cs_count;

      /* TA5_TRACE_CB dumps the whole command-buffer stream of every buffer in
       * the batch, before relocations.  The draw's own IB is dumped
       * separately by TA5_TRACE_DRAW; this is everything around it. */
      if (getenv("TA5_TRACE_CB")) {
         fprintf(stderr, "=== cmd buffer %u: %u dwords ===\n", i,
                 cmd->cs_count);
         for (uint32_t w = 0; w < cmd->cs_count; w++)
            fprintf(stderr, "  %04x: %08x\n", w * 4, cmd->cs[w]);
      }

      for (uint32_t j = 0; j < cmd->ib_count; j++)
         merged_ibs[ib_n++] = &cmd->ibs[j];
    }

    if (getenv("TA5_TRACE_CB")) {
      fprintf(stderr, "=== relocs: %d over %u bos ===\n", reloc_n,
              merged_bo_count);
      for (uint32_t k = 0; k < merged_bo_count; k++)
         fprintf(stderr, "  bo[%u] %p size=%u\n", k, (void *)merged_bos[k],
                 merged_bos[k]->size);
      for (uint32_t k = 0; k < reloc_n; k++)
         fprintf(stderr, "  reloc dst=%u bo=%u or=%x\n",
                 merged_relocs[k].dst_off, merged_relocs[k].bo_index,
                 merged_relocs[k].or_mask);
      fprintf(stderr, "  ibs: %u\n", ib_n);
    }

    /* The stream buffer is CPU memory the kernel reads with get_user_pages.
     * Printing it says which half of the address space a fault address in
     * dmesg belongs to: a GPU IOVA (near a buffer's) or a CPU address that
     * leaked into the stream. */
    if (getenv("TA5_TRACE_CB"))
      fprintf(stderr, "  stream=%p (%u words)\n", (void *)stream, total_words);

    fence_fd = ta5_submit_cmd_relocs(queue, stream, total_words, merged_relocs,
                                     merged_bos, merged_bo_count, reloc_n,
                                     merged_ibs, ib_n);
    /*
     * The kernel patches relocations into the stream in place, so after the
     * submit the words the relocations covered hold the IOVAs the hardware was
     * actually told to use.  Printing them is how a fault address in dmesg gets
     * matched to the buffer it came from.
     */
    if (getenv("TA5_TRACE_CB")) {
      for (uint32_t k = 0; k < reloc_n; k++) {
         uint32_t off = merged_relocs[k].dst_off / 4;
         fprintf(stderr, "  patched[%u] bo=%u size=%u -> %08x%08x\n", k,
                 merged_relocs[k].bo_index,
                 merged_bos[merged_relocs[k].bo_index]->size,
                 off + 1 < total_words ? stream[off + 1] : 0,
                 off < total_words ? stream[off] : 0);
      }
    }

    free(merged_relocs);
    free(merged_bos);
    free(merged_ibs);
   free(stream);

   if (fence_fd < 0) {
      TA5_DBG("fence_fd < 0 (%d) -> DEVICE_LOST", fence_fd);
      return vk_error(&device->instance->vk, VK_ERROR_DEVICE_LOST);
   }

   /* Mark the command buffers pending so they cannot be resubmitted. */
   for (uint32_t i = 0; i < cmd_count; i++) {
      struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(
         cmds[i]);
      cmd->state = MESA_VK_COMMAND_BUFFER_STATE_PENDING;
   }

   /*
    * Keep a copy of the kernel fence so vkQueueWaitIdle has something to
    * wait on, and hand the original to the caller.
    */
   if (queue->idle_fence_fd >= 0)
      close(queue->idle_fence_fd);
   queue->idle_fence_fd = dup(fence_fd);

   /*
    * The Vulkan fence is no longer fed from the kernel here.  It used to be,
    * via vk_sync_import_sync_file(), because a sync file is not a syncobj fd
    * and the opaque-fd path fails it with EINVAL.  But importing it signalled
    * the fence as soon as the submit ioctl returned rather than when the work
    * finished, which is the ordering the spec asks for and the reason an
    * application could read an image the GPU was still writing.
    *
    * The caller now polls this fd and raises every payload -- the fence and
    * the batch's signal semaphores -- only once the GPU is done.  See
    * ta5_queue_submit_one().
    */
   if (out_fence_fd) {
      *out_fence_fd = fence_fd;
   } else {
      close(fence_fd);
   }

   return VK_SUCCESS;
}

/**
 * Wait for a kernel sync-file fd to report completion.
 *
 * MSM_SUBMIT_FENCE_FD_OUT returns a sync file, which becomes readable once the
 * work it guards has finished.  poll() is therefore enough and avoids importing
 * a sync object we would immediately drop.
 *
 * Returns the poll revents so a hung GPU (which on this part shows up as a
 * continuous fault loop rather than a lost context) is distinguishable from
 * normal completion.
 */
static int
ta5_poll_sync_fd(int fd)
{
   struct pollfd pfd = {
      .fd = fd,
      .events = POLLIN,
   };

   for (;;) {
      int ret = poll(&pfd, 1, -1);
      if (ret < 0 && errno == EINTR)
         continue;
      if (ret < 0)
         return -errno;
      if (ret == 0)
         continue; /* timed out, but timeout was infinite */

      return pfd.revents;
   }
}

/**
 * Block on the semaphores this batch waits for.
 *
 * The waits are satisfied on the host rather than encoded into the command
 * stream, because nothing in PM4 implements them yet.  Waiting on the active
 * sync is what makes WSI work at all: wsi's present path hands the driver a
 * binary semaphore and blocks on it, and since ta5_QueueSubmit() used to
 * ignore pSignalSemaphores entirely, that semaphore was never signalled and
 * both headless and Wayland presented nothing and then hung.
 *
 * vk_semaphore_reset_temporary() restores the previous permanent payload after
 * an imported one has been waited on, and the restored payload is reset so the
 * semaphore is unsignalled for its next use -- the binary-semaphore part of the
 * contract that vk_queue.c:300-325 arranges when it builds a real submit.
 */
static VkResult
ta5_queue_wait_semaphores(struct ta5_device *device, uint32_t count,
                          const VkSemaphore *semaphores,
                          const VkPipelineStageFlags2 *stage_masks,
                          const uint64_t *values)
{
   if (count == 0)
      return VK_SUCCESS;

   struct vk_sync_wait *waits =
      (struct vk_sync_wait *)calloc(count, sizeof(*waits));
   if (!waits)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < count; i++) {
      struct vk_semaphore *sem = vk_semaphore_from_handle(semaphores[i]);

      waits[i] = (struct vk_sync_wait) {
         .sync = vk_semaphore_get_active_sync(sem),
         .stage_mask = stage_masks ? stage_masks[i]
                                   : (VkPipelineStageFlags2)~0ull,
         .wait_value = sem->type == VK_SEMAPHORE_TYPE_TIMELINE
                          ? (values ? values[i] : 0)
                          : 0,
      };
   }

   if (getenv("TA5_TRACE_SEM")) {
      for (uint32_t i = 0; i < count; i++) {
         struct vk_semaphore *sem = vk_semaphore_from_handle(semaphores[i]);
         fprintf(stderr,
                 "ta5[sem]: wait %p type=%s temp=%p want=%llu\n",
                 (void *)sem,
                 sem->type == VK_SEMAPHORE_TYPE_TIMELINE ? "timeline" : "binary",
                 (void *)sem->temporary,
                 (unsigned long long)waits[i].wait_value);
      }
   }

   result = vk_sync_wait_many(&device->vk, count, waits, VK_SYNC_WAIT_COMPLETE,
                              UINT64_MAX);
   if (getenv("TA5_TRACE_SEM"))
      fprintf(stderr, "ta5[sem]: wait_many -> %d\n", result);

   /*
    * Consume what was waited on, but only once the wait actually succeeded --
    * resetting a semaphore whose wait timed out would silently un-signal it
    * and turn a recoverable poll into a permanent hang.
    */
   if (result == VK_SUCCESS) {
      for (uint32_t i = 0; i < count; i++) {
         struct vk_semaphore *sem = vk_semaphore_from_handle(semaphores[i]);

         vk_semaphore_reset_temporary(&device->vk, sem);
         if (sem->type == VK_SEMAPHORE_TYPE_BINARY) {
            VkResult r = vk_sync_reset(&device->vk, &sem->permanent);
            if (r != VK_SUCCESS)
               result = r;
         }
      }
   }

   free(waits);
   return result;
}

/**
 * Signal the semaphores and the fence once the GPU has actually finished.
 *
 * Signalling before completion would let an application read a swapchain image
 * that the GPU is still writing.  The kernel fence is what tells us when that
 * has stopped, so it is waited on here and only then are the payloads raised.
 * This serialises submissions -- there is no overlap -- which costs throughput
 * but is the only ordering this driver can currently guarantee.
 */
static VkResult
ta5_queue_signal_syncs(struct ta5_device *device, uint32_t count,
                       const VkSemaphore *semaphores, const uint64_t *values,
                       VkFence fence)
{
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < count; i++) {
      struct vk_semaphore *sem = vk_semaphore_from_handle(semaphores[i]);
      struct vk_sync *sync = vk_semaphore_get_active_sync(sem);
      uint64_t value = sem->type == VK_SEMAPHORE_TYPE_TIMELINE
                          ? (values ? values[i] : 0)
                          : 0;
      /*
       * Whether we are raising an imported payload or the semaphore's own one
       * decides what happens next, and getting it backwards loses the signal.
       */
      bool had_temporary = (sem->temporary != NULL);

      if (getenv("TA5_TRACE_SEM"))
         fprintf(stderr, "ta5[sem]: signal %p type=%s temp=%p value=%llu\n",
                 (void *)sem,
                 sem->type == VK_SEMAPHORE_TYPE_TIMELINE ? "timeline" : "binary",
                 (void *)sem->temporary, (unsigned long long)(value ? value : 1));
      VkResult r = vk_sync_signal(&device->vk, sync, value ? value : 1);
      if (r != VK_SUCCESS && result == VK_SUCCESS)
         result = r;

      /*
       * An imported (temporary) payload is single-use: once it has been raised
       * it is dropped and the semaphore is left unsignalled, which is what
       * vk_queue.c does for a signalled import.
       *
       * But when there was no temporary, the signal landed on the permanent
       * payload and that payload has to STAY raised until some consumer waits
       * on it.  Resetting it here un-signalled the semaphore in the same breath
       * that signalled it, and the wait on the next frame then blocked
       * forever on a semaphore nobody would ever raise again:
       *
       *     ta5[sem]: signal 0xffff8fad35d0 type=binary temp=0 value=1
       *     ta5[sem]: wait   0xffff8fad35d0 type=binary temp=0   <- hangs
       */
      if (had_temporary) {
         vk_semaphore_reset_temporary(&device->vk, sem);
         if (sem->type == VK_SEMAPHORE_TYPE_BINARY) {
            VkResult r = vk_sync_reset(&device->vk, &sem->permanent);
            if (r != VK_SUCCESS && result == VK_SUCCESS)
               result = r;
         }
      }
   }

   if (fence != VK_NULL_HANDLE) {
      struct vk_fence *vfence = vk_fence_from_handle(fence);

      /* Signal the active payload, matching what a WSI acquire imports. */
      struct vk_sync *sync = vk_fence_get_active_sync(vfence);
      VkResult r = vk_sync_signal(&device->vk, sync, 1);
      if (r != VK_SUCCESS && result == VK_SUCCESS)
         result = r;

      vk_fence_reset_temporary(&device->vk, vfence);
   }

   return result;
}

/**
 * Run one batch: wait its dependencies, submit, wait for the GPU, then signal.
 */
static VkResult
ta5_queue_submit_one(struct ta5_queue *queue, const VkCommandBuffer *cmds,
                     uint32_t cmd_count, uint32_t wait_count,
                     const VkSemaphore *wait_sems,
                     const VkPipelineStageFlags2 *wait_stages,
                     const uint64_t *wait_values, uint32_t signal_count,
                     const VkSemaphore *signal_sems, const uint64_t *signal_values,
                     VkFence fence)
{
   struct ta5_device *device = queue->device;
   VkResult result;

   result = ta5_queue_wait_semaphores(device, wait_count, wait_sems,
                                      wait_stages, wait_values);
   if (result != VK_SUCCESS)
      return result;

   /*
    * The batch runs, and its signal payloads are raised only after the kernel
    * says it is done, so the fence cannot be signalled for work still in
    * flight.  ta5_queue_submit_batch() keeps ownership of the returned kernel
    * fence through *out_fence_fd for that reason.
    */
   int fence_fd = -1;
   result = ta5_queue_submit_batch(queue, cmds, cmd_count, &fence_fd);
   if (result != VK_SUCCESS)
      return result;

   /* An empty batch has no kernel fence and is already complete. */
   if (fence_fd >= 0) {
      int revents = ta5_poll_sync_fd(fence_fd);

      if (revents < 0) {
         close(fence_fd);
         return vk_error(&device->instance->vk, VK_ERROR_DEVICE_LOST);
      }
      if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
         /*
          * The GPU is faulting rather than the context being lost, so the
          * submission will never complete.  Reporting success here would hang
          * the application on the signal below.
          */
         close(fence_fd);
         return vk_error(&device->instance->vk, VK_ERROR_DEVICE_LOST);
      }
      close(fence_fd);
   }

   return ta5_queue_signal_syncs(device, signal_count, signal_sems,
                                 signal_values, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_QueueSubmit(VkQueue _queue, uint32_t submitCount,
                const VkSubmitInfo *pSubmits, VkFence fence)
{
   struct ta5_queue *queue = ta5_queue_from_handle(_queue);

   /* A submit with no batches still has to signal its fence, or an
    * application that queues nothing and waits deadlocks. */
   if (submitCount == 0)
      return ta5_queue_signal_syncs(queue->device, 0, NULL, NULL, fence);

   for (uint32_t i = 0; i < submitCount; i++) {
      const VkSubmitInfo *info = &pSubmits[i];
      bool last = (i == submitCount - 1);

      /*
       * VkSubmitInfo carries 32-bit stage masks; the sync wait list wants
       * VkPipelineStageFlags2.  The bits have to be shifted because the two
       * enumerations do not share values -- the sync-bit that means
       * "everything" only exists in the 64-bit form.
       */
      VkPipelineStageFlags2 *wait_stages = NULL;
      if (info->waitSemaphoreCount) {
         wait_stages = (VkPipelineStageFlags2 *)calloc(info->waitSemaphoreCount,
                                                       sizeof(*wait_stages));
         if (!wait_stages)
            return vk_error(&queue->device->instance->vk,
                            VK_ERROR_OUT_OF_HOST_MEMORY);
         for (uint32_t j = 0; j < info->waitSemaphoreCount; j++)
            wait_stages[j] = (VkPipelineStageFlags2)info->pWaitDstStageMask[j];
      }

      VkResult result = ta5_queue_submit_one(
         queue, info->pCommandBuffers, info->commandBufferCount,
         info->waitSemaphoreCount, info->pWaitSemaphores, wait_stages, NULL,
         last ? info->signalSemaphoreCount : 0, info->pSignalSemaphores, NULL,
         last ? fence : VK_NULL_HANDLE);
      free(wait_stages);
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

   if (submitCount == 0)
      return ta5_queue_signal_syncs(queue->device, 0, NULL, NULL, fence);

   for (uint32_t i = 0; i < submitCount; i++) {
      const VkSubmitInfo2 *info = &pSubmits[i];
      bool last = (i == submitCount - 1);

      /*
       * VkSubmitInfo2 is not layout-compatible with VkSubmitInfo: casting one
       * to the other made commandBufferCount read a truncated semaphore
       * pointer and pCommandBuffers read a pointer to pipeline stage masks,
       * which then got walked as an array of VkCommandBuffer handles.  Read
       * the handles out of the per-batch VkCommandBufferInfo instead.
       */
      const VkCommandBufferSubmitInfo *cb_info = info->pCommandBufferInfos;
      uint32_t cb_count = info->commandBufferInfoCount;
      VkCommandBuffer *cmds = NULL;

      if (cb_count) {
         cmds = (VkCommandBuffer *)malloc(cb_count * sizeof(*cmds));
         if (!cmds)
            return vk_error(&queue->device->instance->vk,
                            VK_ERROR_OUT_OF_HOST_MEMORY);
         for (uint32_t j = 0; j < cb_count; j++)
            cmds[j] = cb_info[j].commandBuffer;
      }

      /* Semaphore values live in the per-semaphore info arrays. */
      uint64_t *wait_values = NULL, *signal_values = NULL;

      if (info->waitSemaphoreInfoCount) {
         wait_values = (uint64_t *)calloc(info->waitSemaphoreInfoCount,
                                          sizeof(*wait_values));
         if (!wait_values) {
            free(cmds);
            return vk_error(&queue->device->instance->vk,
                            VK_ERROR_OUT_OF_HOST_MEMORY);
         }
         for (uint32_t j = 0; j < info->waitSemaphoreInfoCount; j++)
            wait_values[j] = info->pWaitSemaphoreInfos[j].value;
      }
      if (info->signalSemaphoreInfoCount) {
         signal_values = (uint64_t *)calloc(info->signalSemaphoreInfoCount,
                                            sizeof(*signal_values));
         if (!signal_values) {
            free(wait_values);
            free(cmds);
            return vk_error(&queue->device->instance->vk,
                            VK_ERROR_OUT_OF_HOST_MEMORY);
         }
         for (uint32_t j = 0; j < info->signalSemaphoreInfoCount; j++)
            signal_values[j] = info->pSignalSemaphoreInfos[j].value;
      }

      uint32_t wait_count = info->waitSemaphoreInfoCount;
      uint32_t signal_count =
         last ? info->signalSemaphoreInfoCount : 0;

      /* Collect the semaphore handles from the info arrays. */
      VkSemaphore *wait_sems = NULL, *signal_sems = NULL;
      VkPipelineStageFlags2 *wait_stages = NULL;

      if (wait_count) {
         wait_sems = (VkSemaphore *)calloc(wait_count, sizeof(*wait_sems));
         wait_stages = (VkPipelineStageFlags2 *)calloc(
            wait_count, sizeof(*wait_stages));
         if (!wait_sems || !wait_stages) {
            free(wait_sems); free(wait_stages); free(wait_values);
            free(signal_values); free(cmds);
            return vk_error(&queue->device->instance->vk,
                            VK_ERROR_OUT_OF_HOST_MEMORY);
         }
         for (uint32_t j = 0; j < wait_count; j++) {
            wait_sems[j] = info->pWaitSemaphoreInfos[j].semaphore;
            wait_stages[j] = info->pWaitSemaphoreInfos[j].stageMask;
         }
      }
      if (signal_count) {
         signal_sems = (VkSemaphore *)calloc(signal_count,
                                             sizeof(*signal_sems));
         if (!signal_sems) {
            free(wait_sems); free(wait_stages); free(wait_values);
            free(signal_values); free(cmds);
            return vk_error(&queue->device->instance->vk,
                            VK_ERROR_OUT_OF_HOST_MEMORY);
         }
         for (uint32_t j = 0; j < signal_count; j++)
            signal_sems[j] = info->pSignalSemaphoreInfos[j].semaphore;
      }

      VkResult result = ta5_queue_submit_one(
         queue, cmds, cb_count, wait_count, wait_sems, wait_stages, wait_values,
         signal_count, signal_sems, signal_values, last ? fence : VK_NULL_HANDLE);

      free(wait_sems); free(wait_stages); free(wait_values);
      free(signal_sems); free(signal_values);
      free(cmds);
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
      struct timespec t0, t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      int ret = poll(&pfd, 1, -1);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (getenv("TA5_TRACE_NULL"))
         fprintf(stderr, "ta5[wait]: poll fd %d -> %d in %ld us\n", pfd.fd,
                 ret, (t1.tv_sec - t0.tv_sec) * 1000000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000);

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
