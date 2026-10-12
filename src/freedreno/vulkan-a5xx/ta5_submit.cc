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

struct ta5_bo *
ta5_bo_alloc(struct ta5_device *device, uint32_t size, uint32_t flags)
{
   struct ta5_bo *bo;

   bo = (struct ta5_bo *)calloc(1, sizeof(*bo));
   if (!bo)
      return NULL;

   bo->device = device;
   bo->size = size;
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
   struct ta5_device *device = queue->device;
   struct ta5_bo *ring;
   struct drm_msm_gem_submit_bo bos[1];
   struct drm_msm_gem_submit_cmd cmds[1];
   struct drm_msm_gem_submit req;
   uint32_t size = words * sizeof(uint32_t);
   int ret;

   ring = ta5_queue_ring(queue);
   if (!ring)
      return -1;

   /*
    * Wrap to the start of the ring when a submission does not fit.  In this
    * phase the queue is always idle before the next submit (we wait below),
    * so overwriting is safe.
    */
   if (size > ta5_ring_space(queue))
      queue->ring_offset = 0;

   memcpy((char *)queue->ring_map + queue->ring_offset, cs, size);

   bos[0].flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE;
   bos[0].handle = ring->handle;
   bos[0].presumed = 0;

   cmds[0].type = MSM_SUBMIT_CMD_BUF;
   cmds[0].submit_idx = 0;
   cmds[0].submit_offset = queue->ring_offset;
   cmds[0].size = size;
   cmds[0].pad = 0;
   cmds[0].nr_relocs = reloc_count;
   cmds[0].relocs = 0;

   memset(&req, 0, sizeof(req));
   req.flags = MSM_PIPE_3D0 | MSM_SUBMIT_FENCE_FD_OUT;
   req.queueid = 0;
   req.bos = (uint64_t)(uintptr_t)bos;
   req.nr_bos = ARRAY_SIZE(bos);
   req.cmds = (uint64_t)(uintptr_t)cmds;
   req.nr_cmds = ARRAY_SIZE(cmds);

   ret = drmCommandWriteRead(device->fd, DRM_MSM_GEM_SUBMIT, &req, sizeof(req));
   if (ret) {
      TA5_DBG("submit failed: %d (%s)", ret, strerror(errno));
      return -1;
   }

   queue->ring_offset += size;
   queue->pending++;

   TA5_DBG("submitted %u bytes at ring offset %u, fence fd %d", size,
           cmds[0].submit_offset, req.fence_fd);

   /* Ownership of the fence fd transfers to the caller. */
   return req.fence_fd;
}