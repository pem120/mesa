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
 * \file ta5_formats.h
 *
 * Adreno 5xx Vulkan driver: format capabilities.
 */

#ifndef TA5_FORMATS_H
#define TA5_FORMATS_H

#include "vulkan/vulkan_core.h"

struct ta5_physical_device;

/**
 * Per-format capability entry, mirroring what the Vulkan spec's
 * VkFormatProperties wants: what the hardware can do with the format in
 * linear-tiled, optimally-tiled, and buffer (vertex/SSBO) form.
 */
struct ta5_format_caps
{
   VkFormatFeatureFlags linear;
   VkFormatFeatureFlags optimal;
   VkFormatFeatureFlags buffer;
};

/**
 * Look up the capability entry for a format.
 *
 * Formats absent from the table are genuinely unsupported and get all-zero
 * feature flags, which is the spec-correct answer: an application sees no
 * features and must not use them.
 */
const struct ta5_format_caps *ta5_format_caps_get(VkFormat format);

/**
 * Block size in bytes, or 0 for unsupported/compressed formats.
 *
 * Used to derive maxResourceSize.  Compressed formats report 0 and are
 * rejected by the resource-size check.
 */
uint32_t ta5_format_block_size(VkFormat format);

#endif /* TA5_FORMATS_H */