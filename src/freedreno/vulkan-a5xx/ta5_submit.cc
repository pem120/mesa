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
 * \file ta5_submit.cc
 *
 * Adreno 5xx Vulkan driver: DRM buffer objects and command submission.
 *
 * Mirrors what the GL backend does in freedreno/drm/msm/msm_bo.c and
 * msm_ringbuffer.c, which is known to work on this part:
 *
 *   - buffers are DRM GEM objects (MSM_BO_WC for CPU-written command streams)
 *   - a submission is a drm_msm_gem_submit_cmd of type MSM_SUBMIT_CMD_BUF
 *     pointing into a ring buffer, with a drm_msm_gem_submit_bo carrying the
 *     GEM handle
 *   - GPU addresses are patched by the kernel from drm_msm_gem_submit_reloc
 *     entries, because a5xx does not use userspace-managed VM binding
 *
 * Turnip's vm_bind branch is deliberately not used; it does not apply here.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <drm/msm_drm.h>
#include <xf86drm.h>

#include "util/u_math.h"
#include "util/u_atomic.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_alloc.h"
#include "vk_device.h"
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
         fprintf(stderr, "ta5[submit]: " fmt "\n", ##__VA_ARGS__);         \
   } while (0)

/** Per-queue ring buffer size, in bytes. */
#define TA5_RING_SIZE (64 * 1024)

/* ------------------------------------------------------------------ */
/* Buffer objects                                                       */
/* ------------------------------------------------------------------ */

static int
ta5_gem_new(struct ta5_device *device, uint64_t size, uint32_t msm_flags,
            uint32_t *out_handle)
{
   struct drm_msm_gem_new req = {
      .size = size,
      .flags = msm_flags,
   };

   if (drmCommandWriteRead(device->fd, DRM_MSM_GEM_NEW, &req, sizeof(req)))
      return -1;

   *out_handle = req.handle;
   return 0;
}

static int
ta5_gem_get_offset(struct ta5_device *device, uint32_t handle, uint32_t info,
                   uint64_t *out_value)
{
   struct drm_msm_gem_info req = {
      .handle = handle,
      .info = info,
   };

   if (drmCommandWriteRead(device->fd, DRM_MSM_GEM_INFO, &req, sizeof(req)))
      return -1;

   *out_value = req.value;
   return 0;
}

VkResult
ta5_bo_cpu_prep(struct ta5_bo *bo, uint32_t op)
{
   struct drm_msm_gem_cpu_prep args = {
      .handle = bo->handle,
      .op = op,
      /* Long enough that a busy GPU is not reported as a hang. */
      .timeout = { .tv_sec = 2, .tv_nsec = 0 },
   };

   /*
    * Not every kernel here implements GEM_CPU_PREP -- the running one on the
    * msm8953 device answers EINVAL, which is also what an unrecognised DRM
    * command returns.  Treat it as advisory: the submit fence is the fallback.
    */
   if (drmCommandWriteRead(bo->device->fd, DRM_IOCTL_MSM_GEM_CPU_PREP, &args,
                           sizeof(args)) &&
       getenv("TA5_TRACE_NULL"))
      fprintf(stderr, "ta5[map]: gem cpu_prep(%u) unsupported: %s\n",
              bo->handle, strerror(errno));

   return VK_SUCCESS;
}

struct ta5_bo *
ta5_bo_alloc(struct ta5_device *device, uint32_t size, uint32_t flags)
{
   struct ta5_bo *bo;

   bo = (struct ta5_bo *)calloc(1, sizeof(*bo));
   if (!bo)
      return NULL;

   bo->device = device;
   bo->size = size;
   bo->flags = flags;
   bo->refcount = 1;

   if (ta5_gem_new(device, size, flags, &bo->handle)) {
      TA5_DBG("gem_new(%u) failed: %s", size, strerror(errno));
      free(bo);
      return NULL;
   }

   return bo;
}

struct ta5_bo *
ta5_bo_from_handle(struct ta5_device *device, uint32_t handle, uint32_t size)
{
   struct ta5_bo *bo;

   bo = (struct ta5_bo *)calloc(1, sizeof(*bo));
   if (!bo)
      return NULL;

   bo->device = device;
   bo->handle = handle;
   bo->size = size;
   bo->refcount = 1;

   return bo;
}

void
ta5_bo_ref(struct ta5_bo *bo)
{
   p_atomic_inc(&bo->refcount);
}

void
ta5_bo_unref(struct ta5_bo *bo)
{
   if (!p_atomic_dec_return(&bo->refcount))
      return;

   if (bo->map)
      munmap(bo->map, bo->size);

   if (bo->handle) {
      struct drm_gem_close req = {.handle = bo->handle};
      drmIoctl(bo->device->fd, DRM_IOCTL_GEM_CLOSE, &req);
   }

   free(bo);
}

void *
ta5_bo_map(struct ta5_bo *bo)
{
   uint64_t offset;

   if (bo->map)
      return bo->map;

   /*
    * Ask the kernel for the object's mmap offset, then map it directly.
    * This is the same sequence fd_bo_map_os_mmap() uses in the GL backend.
    */
   if (ta5_gem_get_offset(bo->device, bo->handle, MSM_INFO_GET_OFFSET, &offset)) {
      TA5_DBG("gem_info_get(offset) failed: %s", strerror(errno));
      return NULL;
   }

   bo->map = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  bo->device->fd, offset);
   if (bo->map == MAP_FAILED) {
      TA5_DBG("mmap failed: %s", strerror(errno));
      bo->map = NULL;
      return NULL;
   }

   return bo->map;
}

/* ------------------------------------------------------------------ */
/* Submission                                                           */
/* ------------------------------------------------------------------ */

static struct ta5_bo *
ta5_queue_ring(struct ta5_queue *queue)
{
   struct ta5_bo *bo;
   void *map;

   if (queue->ring)
      return queue->ring;

   /* MSM_BO_WC matches freedreno's default for CPU-written buffers. */
   bo = ta5_bo_alloc(queue->device, TA5_RING_SIZE, MSM_BO_WC);
   if (!bo)
      return NULL;

   map = ta5_bo_map(bo);
   if (!map) {
      ta5_bo_unref(bo);
      return NULL;
   }

   /* Start clean so padding is a known value. */
   memset(map, 0, TA5_RING_SIZE);

   queue->ring = bo;
   queue->ring_map = map;
   queue->ring_offset = 0;

   TA5_DBG("allocated %d byte ring buffer", TA5_RING_SIZE);
   return bo;
}

/** Bytes of headroom left in the ring. */
static uint32_t
ta5_ring_space(struct ta5_queue *queue)
{
   return TA5_RING_SIZE - queue->ring_offset;
}

int
ta5_submit_cmd(struct ta5_queue *queue, const uint32_t *cs, uint32_t words,
               uint32_t reloc_count)
{
   return ta5_submit_cmd_relocs(queue, cs, words, NULL, NULL, 0,
                                reloc_count, NULL, 0);
}

static uint32_t
ta5_submit_bo_index(struct ta5_bo **bos, uint32_t bo_count,
                    struct ta5_bo *bo)
{
   for (uint32_t i = 0; i < bo_count; i++) {
      if (bos[i] == bo)
         return i;
   }
   return UINT32_MAX;
}

/**
 * Submit a command stream, optionally referring to other buffers.
 *
 * relocs[] names buffers whose GPU addresses must be patched into the stream.
 * Each referenced buffer needs its own entry in bos[] (after the command
 * stream itself) or the kernel rejects the submit.
 */
int
ta5_submit_cmd_relocs(struct ta5_queue *queue, const uint32_t *cs,
                      uint32_t words, struct ta5_reloc *relocs,
                      struct ta5_bo **reloc_bo, uint32_t reloc_bo_count,
                      uint32_t reloc_count, struct ta5_ib **ibs,
                      uint32_t ib_count)
{
   struct ta5_device *device = queue->device;
   struct ta5_bo *ring;
   struct ta5_bo **all_bos = NULL;
   struct drm_msm_gem_submit_bo *bos = NULL;
   struct drm_msm_gem_submit_reloc *krelocs = NULL;
   struct drm_msm_gem_submit_cmd *cmds = NULL;
   struct drm_msm_gem_submit req = {};
   uint32_t size = words * sizeof(uint32_t);
   uint64_t bo_capacity64 = 1 + reloc_bo_count;
   uint64_t reloc_count64 = reloc_count;
   uint32_t bo_capacity, bo_count = 1, reloc_n = 0;
   uint32_t i;
   int ret;

   ring = ta5_queue_ring(queue);
   if (!ring)
      return -1;

   /*
    * A submission that is larger than the ring can never be placed: wrapping
    * only helps while it still fits in TA5_RING_SIZE.  cmd->cs grows by 8192
    * words per draw, so a frame with a few dozen draws crosses 64 KB and
    * would otherwise run off the end of the mapping.
    */
   if (size > TA5_RING_SIZE)
      return -1;

   for (uint32_t j = 0; j < ib_count; j++) {
      bo_capacity64 += 1 + ibs[j]->reloc_bo_count;
      reloc_count64 += ibs[j]->reloc_count;
   }
   if (bo_capacity64 > UINT32_MAX || reloc_count64 > UINT32_MAX ||
       ib_count == UINT32_MAX)
      return -1;
   bo_capacity = bo_capacity64;

   all_bos = (struct ta5_bo **)calloc(bo_capacity, sizeof(*all_bos));
   bos = (struct drm_msm_gem_submit_bo *)calloc(bo_capacity, sizeof(*bos));
   cmds = (struct drm_msm_gem_submit_cmd *)calloc(1 + ib_count, sizeof(*cmds));
   if (reloc_count64)
      krelocs = (struct drm_msm_gem_submit_reloc *)calloc(
         reloc_count64, sizeof(*krelocs));
   if (!all_bos || !bos || !cmds || (reloc_count64 && !krelocs)) {
      ret = -1;
      goto cleanup;
   }

   /* BO 0 is the first-level ring; every other BO is deduplicated by handle. */
   all_bos[0] = ring;
   for (i = 0; i < reloc_bo_count; i++) {
      if (ta5_submit_bo_index(all_bos, bo_count, reloc_bo[i]) == UINT32_MAX)
         all_bos[bo_count++] = reloc_bo[i];
   }
   for (uint32_t j = 0; j < ib_count; j++) {
      if (ta5_submit_bo_index(all_bos, bo_count, ibs[j]->bo) == UINT32_MAX)
         all_bos[bo_count++] = ibs[j]->bo;
      for (uint32_t k = 0; k < ibs[j]->reloc_bo_count; k++) {
         if (ta5_submit_bo_index(all_bos, bo_count,
                                 ibs[j]->reloc_bos[k]) == UINT32_MAX)
            all_bos[bo_count++] = ibs[j]->reloc_bos[k];
      }
   }
   assert(bo_count <= bo_capacity);

   for (i = 0; i < bo_count; i++) {
      bos[i].flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE;
      bos[i].handle = all_bos[i]->handle;
      bos[i].presumed = 0;
   }

   /*
    * Wrap to the start of the ring when a submission does not fit.  In this
    * phase the queue is always idle before the next submit (we wait below),
    * so overwriting is safe.
    */
   if (size > ta5_ring_space(queue))
      queue->ring_offset = 0;

   memcpy((char *)queue->ring_map + queue->ring_offset, cs, size);

   /* Main ring stream. */
   cmds[0].type = MSM_SUBMIT_CMD_BUF;
   cmds[0].submit_idx = 0;
   /*
    * The two fields use different units, which is easy to get wrong:
    *
    *   submit_offset  dwords -- the kernel computes the command stream's GPU
    *                             address as iova + submit_offset * 4.
    *   size           bytes  -- the kernel rejects a value that is not a
    *                             multiple of 4, and hands it to the CP
    *                             verbatim as the IB1 length, which the CP
    *                             also reads as bytes.
    *
    * drm_msm_gem_submit_reloc.submit_offset is bytes as well (the kernel
    * divides that one by four itself when indexing the stream).
    */
   cmds[0].submit_offset = queue->ring_offset / 4;
   cmds[0].size = size;
   cmds[0].pad = 0;
   cmds[0].nr_relocs = reloc_count;
   cmds[0].relocs = (uint64_t)(uintptr_t)krelocs;

   for (i = 0; i < reloc_count; i++) {
      struct ta5_bo *bo = reloc_bo[relocs[i].bo_index];
      uint32_t bo_idx = ta5_submit_bo_index(all_bos, bo_count, bo);

      if (getenv("TA5_TRACE_SUBMIT"))
         fprintf(stderr,
                 "ta5[submit] reloc %u/%u bo=%p idx=%u -> all_bos idx=%u "
                 "bo_count=%u size=%u bo_offset=%llu dst_off=%u\n",
                 i, reloc_count, (void *)bo, relocs[i].bo_index, bo_idx,
                 bo_count, bo->size,
                 (unsigned long long)relocs[i].bo_offset, relocs[i].dst_off);

      assert(bo_idx != UINT32_MAX);
      krelocs[reloc_n].submit_offset =
         relocs[i].dst_off + queue->ring_offset;
      krelocs[reloc_n]._or = relocs[i].or_mask;
      krelocs[reloc_n].shift = relocs[i].shift;
      krelocs[reloc_n].reloc_idx = bo_idx;
      krelocs[reloc_n].reloc_offset = relocs[i].bo_offset;
      reloc_n++;
   }

   /* IB targets are relocated by the kernel, but only entered via CP_IB. */
   for (uint32_t j = 0; j < ib_count; j++) {
      struct ta5_ib *ib = ibs[j];
      struct drm_msm_gem_submit_cmd *ib_cmd = &cmds[j + 1];
      uint32_t bo_idx = ta5_submit_bo_index(all_bos, bo_count, ib->bo);

      assert(bo_idx != UINT32_MAX);
      ib_cmd->type = MSM_SUBMIT_CMD_IB_TARGET_BUF;
      ib_cmd->submit_idx = bo_idx;
      ib_cmd->submit_offset = 0;
      ib_cmd->size = ib->size * sizeof(uint32_t);
      ib_cmd->pad = 0;
      ib_cmd->nr_relocs = ib->reloc_count;
      ib_cmd->relocs = ib->reloc_count
                          ? (uint64_t)(uintptr_t)&krelocs[reloc_n]
                          : 0;

      for (uint32_t k = 0; k < ib->reloc_count; k++) {
         struct ta5_reloc *r = &ib->relocs[k];
         struct ta5_bo *src = ib->reloc_bos[r->bo_index];
         uint32_t src_idx = ta5_submit_bo_index(all_bos, bo_count, src);

         assert(src_idx != UINT32_MAX);
         krelocs[reloc_n].submit_offset = r->dst_off;
         krelocs[reloc_n]._or = r->or_mask;
         krelocs[reloc_n].shift = r->shift;
         krelocs[reloc_n].reloc_idx = src_idx;
         krelocs[reloc_n].reloc_offset = r->bo_offset;
         reloc_n++;
      }
   }
   assert(reloc_n == reloc_count64);

   memset(&req, 0, sizeof(req));
   req.flags = MSM_PIPE_3D0 | MSM_SUBMIT_FENCE_FD_OUT;
   req.queueid = 0;
   req.bos = (uint64_t)(uintptr_t)bos;
   req.nr_bos = bo_count;
   req.cmds = (uint64_t)(uintptr_t)cmds;
   req.nr_cmds = 1 + ib_count;

   ret = drmCommandWriteRead(device->fd, DRM_MSM_GEM_SUBMIT, &req, sizeof(req));
   if (ret) {
      TA5_DBG("submit failed: %d (%s)", ret, strerror(errno));
      ret = -1;
      goto cleanup;
   }

   if (getenv("TA5_TRACE_NULL") && reloc_count) {
      for (i = 0; i < reloc_count; i++) {
         uint32_t off = relocs[i].dst_off + queue->ring_offset;
         const uint32_t *rp = (const uint32_t *)queue->ring_map;
         fprintf(stderr,
                 "ta5[submit]: reloc %u -> ring[%u] = %08x (bo %p off %llu or %x)\n",
                 i, off / 4, rp[off / 4], (void *)reloc_bo[relocs[i].bo_index],
                  (unsigned long long)relocs[i].bo_offset, relocs[i].or_mask);
      }
   }

   if (getenv("TA5_TRACE_NULL") && ib_count) {
      for (uint32_t j = 0; j < ib_count; j++) {
         struct ta5_ib *ib = ibs[j];
         const uint32_t *p = (const uint32_t *)ib->bo->map;
         uint64_t iova = 0;

         ta5_gem_get_offset(device, ib->bo->handle, MSM_INFO_GET_IOVA, &iova);
         fprintf(stderr,
                 "ta5[ib]: target %u handle=%u iova=%016llx size=%u words relocs=%u\n",
                 j, ib->bo->handle, (unsigned long long)iova, ib->size,
                 ib->reloc_count);
         for (uint32_t k = 0; k < ib->reloc_count; k++) {
            uint32_t off = ib->relocs[k].dst_off / 4;
            struct ta5_bo *src = ib->reloc_bos[ib->relocs[k].bo_index];
            uint64_t src_iova = 0;
            ta5_gem_get_offset(device, src->handle, MSM_INFO_GET_IOVA,
                               &src_iova);
            fprintf(stderr,
                    "ta5[ib]:   reloc %u dst[%u]=%08x src handle=%u iova=%016llx+%llx shift=%d or=%08x\n",
                    k, off, p[off], src->handle,
                    (unsigned long long)src_iova,
                    (unsigned long long)ib->relocs[k].bo_offset,
                    ib->relocs[k].shift, ib->relocs[k].or_mask);
         }
      }
   }

   queue->ring_offset += size;
   queue->pending++;

   TA5_DBG("submitted %u bytes at ring offset %u, fence fd %d", size,
           cmds[0].submit_offset, req.fence_fd);

   /* Ownership of the fence fd transfers to the caller. */
   ret = req.fence_fd;

cleanup:
   free(krelocs);
   free(cmds);
   free(bos);
   free(all_bos);
   return ret;
}
