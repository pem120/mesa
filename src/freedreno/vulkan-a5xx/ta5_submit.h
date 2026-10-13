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
 * \file ta5_submit.h
 *
 * Adreno 5xx Vulkan driver: DRM buffer objects and command submission.
 */

#ifndef TA5_SUBMIT_H
#define TA5_SUBMIT_H

#include <stdbool.h>
#include <stdint.h>

#include "vk_object.h"
#include "vk_device.h"

#include "drm/msm_drm.h"

#include "ta5_instance.h"

/**
 * A DRM GEM buffer object.
 *
 * a5xx on mainline msm does not use userspace-managed VM binding (freedreno
 * never sets MSM_PARAM_EN_VM_BIND), so GPU addresses are not known to
 * userspace.  Addresses are patched into the command stream at submit time by
 * the kernel using drm_msm_gem_submit_reloc entries instead -- see
 * ta5_submit_cmd().
 */
struct ta5_bo
{
   struct ta5_device *device;
   uint32_t handle;
   uint32_t size;
   uint32_t flags; /**< MSM_BO_* cache flags given to GEM_NEW */
   void *map;      /**< lazily created host mapping, NULL until mapped */
   uint32_t refcount;
};

/**
 * Allocate a GEM object of the given size.
 *
 * flags are MSM_BO_* values from drm/msm_drm.h.
 */
/**
 * True when `bo` was allocated with a CPU-cacheable mapping.
 *
 * Such a mapping is the only one the CPU and the GPU can both touch, and a5xx
 * memory is not coherent, so these need explicit write-out and invalidation
 * around a host mapping -- see ta5_MapMemory() and ta5_UnmapMemory().
 */
static inline bool
ta5_bo_is_cached(const struct ta5_bo *bo)
{
   return (bo->flags & MSM_BO_CACHE_MASK) == MSM_BO_CACHED;
}

/**
 * Block until the GPU is done with `bo`.
 *
 * msm_drv.c is explicit that this is "the normal way to synchronize with the
 * GPU": the submit fence fd only tells you the submission was queued, and on
 * this kernel a render is still in flight after it becomes readable.  CPU_PREP
 * waits on the buffer's dma-reservation instead, which is released when the
 * GPU actually retires the work.
 *
 * `op` is a mask of MSM_PREP_READ / MSM_PREP_WRITE.
 */
VkResult ta5_bo_cpu_prep(struct ta5_bo *bo, uint32_t op);

struct ta5_bo *ta5_bo_alloc(struct ta5_device *device, uint32_t size,
                             uint32_t flags);
struct ta5_bo *ta5_bo_from_handle(struct ta5_device *device, uint32_t handle,
                                  uint32_t size);
void ta5_bo_ref(struct ta5_bo *bo);
void ta5_bo_unref(struct ta5_bo *bo);

/** Host mapping for CPU access; NULL on failure. */
void *ta5_bo_map(struct ta5_bo *bo);

/**
 * Submit one command stream.
 *
 * Copies the stream into a ring buffer owned by the queue and issues
 * DRM_MSM_GEM_SUBMIT.  Returns the DRM fence fd on success, or -1.  The
 * caller owns the returned fd and must close it.
 */
struct ta5_reloc;
struct ta5_ib;

/** Submit a first-level stream plus its relocatable CP_INDIRECT_BUFFER targets. */
int ta5_submit_cmd_relocs(struct ta5_queue *queue, const uint32_t *cs,
                          uint32_t words, struct ta5_reloc *relocs,
                          struct ta5_bo **reloc_bo, uint32_t reloc_bo_count,
                          uint32_t reloc_count, struct ta5_ib **ibs,
                          uint32_t ib_count);

int ta5_submit_cmd(struct ta5_queue *queue, const uint32_t *cs, uint32_t words,
                   uint32_t reloc_count);

#endif /* TA5_SUBMIT_H */
