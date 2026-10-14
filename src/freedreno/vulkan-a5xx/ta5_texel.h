/*
 * Texel descriptors.
 *
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 */

/**
 * \file ta5_texel.h
 *
 * The a5xx texel descriptor: twelve dwords that describe one sampled image to
 * the texture fetch unit.
 *
 * Transcribed from fd5_sampler_view_create() (fd5_texture.c:109) and the
 * fd5_emit_textures() loop (fd5_emit.c:363) that ships them, which is what
 * Turnip does for the same hardware.  The twelve words are the same twelve
 * words the GL backend calls texconst0..3 and texconst5..11: words 0..3 are
 * the descriptor proper and words 4..5 are the image address, which the kernel
 * relocates rather than the driver writing it.
 */

#ifndef TA5_TEXEL_H
#define TA5_TEXEL_H

#include <stdint.h>

#include <stdint.h>

#include "vulkan/vulkan.h"

struct ta5_image_view;

/** TEX_CONST words in one texture descriptor. */
#define TA5_TEXEL_DESC_DWORDS 12

/**
 * Build the twelve descriptor dwords for a view.
 *
 * `address` receives the image's IOVA plus the byte offset of the view's base
 * mip level and base array layer; the caller emits that as the relocation so
 * the kernel can patch it, and the descriptor itself carries no address.
 */
void ta5_texel_descriptor(const struct ta5_image_view *view, uint32_t *desc,
                          uint64_t *address);

/**
 * Number of textures a view occupies: a cube or cube array view spans several
 * layers but is still one fetch unit and one descriptor.
 */
unsigned ta5_texel_descriptor_count(const struct ta5_image_view *view);

/**
 * TFMT5 value for a VkFormat, or TFMT5_NONE if it cannot be sampled.
 *
 * uint32_t rather than the enum so this header does not have to pull in the
 * generated register header; the values are small integers.
 */
uint32_t ta5_texel_format(VkFormat format);

#endif /* TA5_TEXEL_H */