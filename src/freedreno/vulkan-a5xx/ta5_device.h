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
 * Adreno 5xx GPU identification and capability info.
 *
 * a5xx uses the bindful descriptor model: descriptors are bound per shader
 * stage into dedicated hardware arrays rather than a single bindless heap.
 * That is the fundamental architectural difference from Turnip (a6xx+) and
 * the reason this is a separate driver.  Target API level is Vulkan 1.1.
 */

#ifndef TA5_DEVICE_H
#define TA5_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "common/freedreno_dev_info.h"

/**
 * Per-physical-device a5xx generation info.
 *
 * Mirrors the subset of the shared fd_dev_info that the Vulkan driver needs.
 * Seeded from common/freedreno_devices.py CHIP.A5XX, which is the *measured*
 * table the GL backend already uses for these parts, so limits stay
 * consistent with the working GL path on the same silicon.
 */
struct ta5_gpu_info
{
   uint32_t chip;             /**< A5XX (== 5) for all supported parts */
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

/**
 * Resolve a GPU to the a5xx device info using the two ids the kernel reports
 * through DRM params (FD_GPU_ID / FD_CHIP_ID), exactly as the GL backend does
 * in freedreno_pipe.c.  Returns false for non-a5xx parts.
 */
bool ta5_get_gpu_info(uint32_t gpu_id, uint64_t chip_id,
                      struct ta5_gpu_info *info);

/**
 * Human-readable device name for an a5xx GPU id, e.g. "Adreno (TM) 506".
 */
const char *ta5_device_name(uint32_t gpu_id);

#endif /* TA5_DEVICE_H */