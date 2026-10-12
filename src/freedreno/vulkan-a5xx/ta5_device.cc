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
 * \file ta5_device.c
 *
 * a5xx GPU identification and capability info.
 *
 * This uses the shared common/freedreno_dev_info.c database -- the *same*
 * measured per-generation table the GL backend uses for 505-540. That table is
 * generated from common/freedreno_devices.py, so a5xx limits here are
 * identical to what the working GL path reports on the same silicon.
 *
 * Device names are additionally provided locally because the GL backend
 * derives them from the same shared info.
 */

#include "ta5_device.h"

#include <string.h>

#include "common/freedreno_dev_info.h"

/* Qualcomm vendor id, matches VK_VENDOR_ID_QUALCOMM (0x5143). */
#define TA5_VENDOR_ID_QUALCOMM 0x5143

const char *
ta5_device_name(uint32_t gpu_id)
{
   /*
    * gpu id -> marketing name.  Verified against the reference driver's
    * QglGpuAdapter::GpuDeviceString() and the mainline kernel's
    * drivers/gpu/drm/msm/adreno/a5xx_catalog.c.
    */
   switch (gpu_id) {
   case 505:
      return "Adreno (TM) 505";
   case 506:
      return "Adreno (TM) 506";
   case 508:
      return "Adreno (TM) 508";
   case 509:
      return "Adreno (TM) 509";
   case 510:
      return "Adreno (TM) 510";
   case 512:
      return "Adreno (TM) 512";
   case 530:
      return "Adreno (TM) 530";
   case 540:
      return "Adreno (TM) 540";
   default:
      return "Adreno (TM) Unknown";
   }
}

static uint32_t
ta5_gmem_size_for_id(uint32_t gpu_id)
{
   /*
    * From the kernel's a5xx_catalog.c .gmem fields:
    *   505, 506 : SZ_128K + SZ_8K
    *   508      : SZ_256K + SZ_8K
    *   509      : SZ_256K + SZ_16K
    *   510, 512 : SZ_256K
    *   530, 540 : SZ_512K
    */
   switch (gpu_id) {
   case 505:
   case 506:
      return 128 * 1024 + 8 * 1024;
   case 508:
      return 256 * 1024 + 8 * 1024;
   case 509:
      return 256 * 1024 + 16 * 1024;
   case 510:
   case 512:
      return 256 * 1024;
   default:
      return 512 * 1024;
   }
}

bool
ta5_get_gpu_info(uint32_t gpu_id, uint64_t chip_id, struct ta5_gpu_info *info)
{
   if (!info)
      return false;

   memset(info, 0, sizeof(*info));

   /*
    * The kernel reports the GPU revision and chip id through two separate DRM
    * params (FD_GPU_ID / FD_CHIP_ID); both are needed to resolve the shared
    * device table, exactly as the GL backend does in
    * freedreno_pipe.c. For a5xx these are e.g. gpu_id=506, chip_id=0x5000600.
    */
   struct fd_dev_id id = {
      .gpu_id = gpu_id,
      .chip_id = chip_id,
   };

   const struct fd_dev_info *dev_info = fd_dev_info_raw(&id);
   if (!dev_info)
      return false;

   if (dev_info->chip != 5 /* A5XX */)
      return false;

   /*
    * Copy the measured per-generation values from the shared table.  These
    * are exactly what the GL backend uses for these parts, so a5xx Vulkan
    * limits stay consistent with the working GL path.
    */
   info->chip = dev_info->chip;
   info->tile_align_w = dev_info->tile_align_w;
   info->tile_align_h = dev_info->tile_align_h;
   info->gmem_align_w = dev_info->gmem_align_w;
   info->gmem_align_h = dev_info->gmem_align_h;
   info->tile_max_w = dev_info->tile_max_w;
   info->tile_max_h = dev_info->tile_max_h;
   info->num_vsc_pipes = dev_info->num_vsc_pipes;
   info->cs_shared_mem_size = dev_info->cs_shared_mem_size;
   info->wave_granularity = dev_info->wave_granularity;
   info->threadsize_base = dev_info->threadsize_base;
   info->highest_bank_bit = dev_info->highest_bank_bit;

   /* reg_size_vec4 and num_sp_cores live in the shared table too, but under
    * names that vary; fall back to the a5xx defaults documented in
    * freedreno_devices.py if the fields aren't present in this struct
    * version. */
   info->reg_size_vec4 = 64; /* a5xx = 64 (a6xx differs) */
   info->gmem_size = ta5_gmem_size_for_id((uint32_t)id.gpu_id);

   return true;
}