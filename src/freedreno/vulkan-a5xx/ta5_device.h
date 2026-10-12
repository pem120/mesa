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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file ta5_device.h
 *
 * Adreno 5xx Vulkan driver.  This driver targets pre-a6xx Adreno GPUs
 * (Adreno 505/506/508/509/510/512/530/540) which use the *bindful* descriptor
 * model: descriptors are bound per-shader-stage into dedicated hardware
 * arrays rather than a single bindless heap.  This is the fundamental
 * architectural difference from Turnip (a6xx+), and is why a separate driver
 * is required.
 *
 * Target API level is Vulkan 1.1.  a5xx hardware cannot support Vulkan 1.2 /
 * VK_EXT_descriptor_indexing (both require bindless descriptors).
 */

#ifndef TA5_DEVICE_H
#define TA5_DEVICE_H

#include <stdint.h>

#include "common/fd5_hw.h"
#include "common/freedreno_common.h"

/* forward declarations of core vk object wrappers */
struct ta5_instance;
struct ta5_physical_device;
struct ta5_device;

/**
 * Per-physical-device a5xx generation info.
 *
 * Mirrors the subset of fd5 device info that the Vulkan driver needs. Values
 * are seeded from the shared common/freedreno_devices.py CHIP.A5XX table, which
 * the GL backend already uses and which encodes the *measured* limits for each
 * generation. See A5XX_VULKAN_SPEC.md section 6.5.
 */
struct ta5_gpu_info
{
   uint32_t chip;             /**< A5XX (== 5) for all supported parts */
   uint32_t num_gmem_pipes;
   uint32_t gmem_size;        /**< bytes of on-chip gmem */
   uint32_t tile_max_w;
   uint32_t tile_max_h;
   uint32_t gmem_align_w;
   uint32_t gmem_align_h;
   uint32_t tile_align_w;
   uint32_t tile_align_h;
   uint32_t num_vsc_pipes;
   uint32_t num_sp_cores;
   uint32_t highest_bank_bit;
   uint32_t reg_size_vec4;
   uint32_t cs_shared_mem_size;
   uint32_t wave_granularity;
   uint32_t threadsize_base;
};

/* gmem size in bytes, per Adreno a5xx SKU (matches kernel a5xx_catalog.c):
 *   505/506: SZ_128K + SZ_8K
 *   508/509/510/512: SZ_256K (+ SZ_16K for 509)
 *   530/540: SZ_512K + ...
 */
#define TA5_GMEM_SIZE_506 (128 * 1024 + 8 * 1024)
#define TA5_GMEM_SIZE_509 (256 * 1024 + 16 * 1024)
#define TA5_GMEM_SIZE_512 (256 * 1024)
#define TA5_GMEM_SIZE_540 (512 * 1024)

/**
 * Physical device (a "GPU").  Corresponds to one msm DRM node.
 */
struct ta5_physical_device
{
   struct ta5_instance *instance;

   int dev_fd;                 /**< DRM fd for /dev/dri/renderD* */
   uint32_t device_id;         /**< Adreno chip id, e.g. 0x05000600 (506) */
   struct ta5_gpu_info gpu_info;

   char name[64];              /**< e.g. "Adreno (TM) 506" */
   uint32_t vendor_id;         /**< VK_VENDOR_ID_QUALCOMM (0x5143) */
   uint32_t driver_version;

   /* Vulkan-visible limits, filled from ta5_gpu_info */
   uint32_t timestamp_period;
   uint32_t gmem_size;

   void *sysmem;               /**< msm / freedreno device handle (fd_device) */
   bool has_a5xx_hw;
};

/**
 * Map a GPU to the a5xx device info, using the two ids the kernel reports via
 * DRM params (FD_GPU_ID / FD_CHIP_ID). Resolves through the shared
 * common/freedreno_dev_info.c table, so the limits match the GL backend.
 *
 * Returns false if the GPU is not a supported a5xx part.
 */
bool ta5_get_gpu_info(uint32_t gpu_id, uint64_t chip_id,
                      struct ta5_gpu_info *info);

/**
 * Human-readable device name for an a5xx GPU id, e.g. "Adreno (TM) 506".
 * Falls back to "Adreno (TM) Unknown" for unrecognized ids.
 */
const char *ta5_device_name(uint32_t gpu_id);

#endif /* TA5_DEVICE_H */