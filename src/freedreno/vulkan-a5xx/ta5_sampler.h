/*
 * Samplers.
 *
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 */

#ifndef TA5_SAMPLER_H
#define TA5_SAMPLER_H

#include "vk_object.h"

struct ta5_device;

/**
 * A sampler, as the four descriptor dwords the fetch unit consumes.
 *
 * The object exists only to own those four dwords: the sampling state is not
 * register state that outlives a submission, it travels with the texture
 * descriptor block (see ta5_cmd_buffer.cc).  One dword per TEX_SAMPn.
 */
struct ta5_sampler
{
   struct vk_object_base base;
   struct ta5_device *device;

   /** TEX_SAMP0..3, in that order. */
   uint32_t desc[4];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_sampler, base, VkSampler, VK_OBJECT_TYPE_SAMPLER)

#endif /* TA5_SAMPLER_H */