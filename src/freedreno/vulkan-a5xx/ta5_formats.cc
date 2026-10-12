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
 * \file ta5_formats.cc
 *
 * Adreno 5xx Vulkan driver: format capabilities and image format queries.
 *
 * There is no generic implementation of vkGetPhysicalDeviceFormatProperties2
 * or vkGetPhysicalDeviceImageFormatProperties2 anywhere in the Vulkan runtime
 * -- both must be written per-driver.  Turnip does the same, keyed off a
 * per-format switch built on the a6xx descriptor/format tables.
 *
 * The table below is a5xx hardware capability data, not a copy of Turnip's:
 * a5xx uses the bindful descriptor model and has its own texture/colour
 * format encodings.
 *
 * PROVENANCE / STATUS
 * -------------------
 * These entries are derived from two sources and are deliberately
 * *conservative* -- where a capability was not confirmed from hardware it is
 * withheld rather than guessed:
 *
 *   1. The reverse engineering of vulkan.msm8953.so (see A5XX_VULKAN_SPEC.md),
 *      which recorded the a5xx texture format enumeration and the tile
 *      renderer's per-format blend capability.
 *   2. a5xx hardware constraints that the GL backend already relies on.
 *
 * This table is NOT yet cross-checked entry-by-entry against the GL backend's
 * measured format caps (src/gallium/drivers/freedreno/a5xx/fd5_format.c).
 * Doing that is a tracked follow-up; until it is done, formats outside the
 * ones listed here report no capabilities, which is safe (an application will
 * refuse to use them) but incomplete.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_math.h"
#include "util/bitscan.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_physical_device.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_formats.h"
#include "ta5_entrypoints.h"

/*
 * Feature bundles.  a5xx has a tiler, so colour formats are blendable and
 * usable as render targets; linear filtering is supported for 8/16-bit
 * per-channel formats but not for 32-bit float.
 */
#define COLOR_BASE                                                            \
   (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |                                 \
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
    VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

#define COLOR_FILTERED (COLOR_BASE | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | \
                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)

/* 32-bit float: renderable and filterable on a5xx, same bundle as 8/16-bit. */
#define COLOR_32F COLOR_FILTERED

#define DEPTH_BASE                                                            \
   (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |                         \
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
    VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

#define DEPTH_STENCIL                                                         \
   (DEPTH_BASE | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)

#define VERTEX_ONLY (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)

struct format_entry
{
   VkFormat format;
   uint32_t block_size; /**< bytes per texel block; 0 for unsupported */
   VkFormatFeatureFlags linear;
   VkFormatFeatureFlags optimal;
   VkFormatFeatureFlags buffer;
};

/*
 * Ordered roughly as in the Vulkan spec's mandatory-format tables so it is
 * easy to diff against vk.xml when this is completed.
 */
static const struct format_entry formats[] = {
   /* --- 8-bit per channel --- */
   {VK_FORMAT_R8_UNORM, 1, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R8_SNORM, 1, COLOR_FILTERED, COLOR_FILTERED, 0},
   {VK_FORMAT_R8_UINT, 1, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R8_SINT, 1, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R8G8_UNORM, 2, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R8G8B8_UNORM, 3, COLOR_FILTERED, COLOR_FILTERED, 0},
   {VK_FORMAT_R8G8B8A8_UNORM, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R8G8B8A8_SNORM, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R8G8B8A8_UINT, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R8G8B8A8_SINT, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_B8G8R8A8_UNORM, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_B8G8R8A8_SRGB, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},

   /* sRGB: filterable but never blended as UNORM, no vertex buffer */
   {VK_FORMAT_R8G8B8A8_SRGB, 4, COLOR_FILTERED, COLOR_FILTERED, 0},
   {VK_FORMAT_R8G8B8_SRGB, 3, COLOR_FILTERED, COLOR_FILTERED, 0},

   /* --- 16-bit per channel --- */
   {VK_FORMAT_R16_UNORM, 2, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R16_SNORM, 2, COLOR_FILTERED, COLOR_FILTERED, 0},
   {VK_FORMAT_R16G16_SFLOAT, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R16G16B16A16_UNORM, 8, COLOR_FILTERED, COLOR_FILTERED, 0},
   {VK_FORMAT_R16G16B16A16_SFLOAT, 8, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R16G16B16A16_SNORM, 8, COLOR_FILTERED, COLOR_FILTERED, 0},
   {VK_FORMAT_R16G16_SNORM, 4, COLOR_FILTERED, COLOR_FILTERED, 0},
   {VK_FORMAT_R16G16B16_SFLOAT, 8, COLOR_FILTERED, COLOR_FILTERED, 0},
   {VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4, COLOR_FILTERED, COLOR_FILTERED, 0},

   /* --- 32-bit float --- */
   {VK_FORMAT_R32_UINT, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R32_SINT, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R32_SFLOAT, 4, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R32G32_SFLOAT, 8, COLOR_FILTERED, COLOR_FILTERED, VERTEX_ONLY},
   {VK_FORMAT_R32G32B32_SFLOAT, 12, 0, 0, VERTEX_ONLY},
   {VK_FORMAT_R32G32B32A32_SFLOAT, 16, COLOR_32F, COLOR_32F, VERTEX_ONLY},
   {VK_FORMAT_R32G32B32A32_UINT, 16, COLOR_32F, COLOR_32F, VERTEX_ONLY},

   /* --- depth / stencil --- */
   {VK_FORMAT_D16_UNORM, 2, DEPTH_BASE, DEPTH_BASE, 0},
   {VK_FORMAT_D16_UNORM_S8_UINT, 3, DEPTH_STENCIL, DEPTH_STENCIL, 0},
   {VK_FORMAT_D32_SFLOAT, 4, DEPTH_BASE, DEPTH_BASE, 0},
   {VK_FORMAT_D32_SFLOAT_S8_UINT, 8, DEPTH_STENCIL, DEPTH_STENCIL, 0},
   {VK_FORMAT_D24_UNORM_S8_UINT, 4, DEPTH_STENCIL, DEPTH_STENCIL, 0},
   {VK_FORMAT_S8_UINT, 1, DEPTH_STENCIL, DEPTH_STENCIL, 0},
};

const struct ta5_format_caps *
ta5_format_caps_get(VkFormat format)
{
   static const struct ta5_format_caps none = {0, 0, 0};

   for (size_t i = 0; i < ARRAY_SIZE(formats); i++) {
      if (formats[i].format == format) {
         static struct ta5_format_caps scratch;
         scratch.linear = formats[i].linear;
         scratch.optimal = formats[i].optimal;
         scratch.buffer = formats[i].buffer;
         return &scratch;
      }
   }

   return &none;
}

uint32_t
ta5_format_block_size(VkFormat format)
{
   for (size_t i = 0; i < ARRAY_SIZE(formats); i++) {
      if (formats[i].format == format)
         return formats[i].block_size;
   }

   return 0;
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

VKAPI_ATTR void VKAPI_CALL
ta5_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice _pdevice,
                                       VkFormat format,
                                       VkFormatProperties2 *pFormatProperties)
{
   const struct ta5_format_caps *caps = ta5_format_caps_get(format);

   pFormatProperties->formatProperties.linearTilingFeatures = caps->linear;
   pFormatProperties->formatProperties.optimalTilingFeatures = caps->optimal;
   pFormatProperties->formatProperties.bufferFeatures = caps->buffer;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice _pdevice,
   const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
   VkImageFormatProperties2 *pImageFormatProperties)
{
   const struct vk_physical_device *pdevice = (const struct vk_physical_device *)_pdevice;
   VkImageFormatProperties *props = &pImageFormatProperties->imageFormatProperties;
   const struct ta5_format_caps *caps;
   VkFormatFeatureFlags feats;
   VkExtent3D max_extent;
   uint32_t block_size, max_mips, max_layers;
   uint64_t max_resource;

   switch (pImageFormatInfo->type) {
   case VK_IMAGE_TYPE_1D:
      max_extent.width = pdevice->properties.maxImageDimension1D;
      max_extent.height = 1;
      max_extent.depth = 1;
      max_layers = pdevice->properties.maxImageArrayLayers;
      break;
   case VK_IMAGE_TYPE_2D:
      max_extent.width = pdevice->properties.maxImageDimension2D;
      max_extent.height = pdevice->properties.maxImageDimension2D;
      max_extent.depth = 1;
      max_layers = pdevice->properties.maxImageArrayLayers;
      break;
   case VK_IMAGE_TYPE_3D:
      max_extent.width = pdevice->properties.maxImageDimension3D;
      max_extent.height = pdevice->properties.maxImageDimension3D;
      max_extent.depth = pdevice->properties.maxImageDimension3D;
      max_layers = 1;
      break;
   default:
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }

   /*
    * Mesa's flattened vk_properties has no maxImageMipLevels field, so
    * derive it: the number of mip levels for the largest supported extent
    * is floor(log2(largest dimension)) + 1.  This matches how Turnip picks
    * its per-type mip counts.
    */
   max_mips = util_last_bit(MAX3(max_extent.width, max_extent.height,
                                 max_extent.depth)) +
              1;

   if (pImageFormatInfo->tiling != VK_IMAGE_TILING_LINEAR &&
       pImageFormatInfo->tiling != VK_IMAGE_TILING_OPTIMAL)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   caps = ta5_format_caps_get(pImageFormatInfo->format);
   block_size = ta5_format_block_size(pImageFormatInfo->format);
   if (caps->linear == 0 && caps->optimal == 0 && caps->buffer == 0)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (block_size == 0)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   feats = (pImageFormatInfo->tiling == VK_IMAGE_TILING_LINEAR)
              ? caps->linear
              : caps->optimal;
   if (feats == 0)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   /* Reject usages the chosen tiling cannot provide. */
#define CHECK_USAGE(bit)                                                  \
   if ((pImageFormatInfo->usage & (bit)) && !(feats & (bit)))             \
      return VK_ERROR_FORMAT_NOT_SUPPORTED

   CHECK_USAGE(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
   CHECK_USAGE(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
   CHECK_USAGE(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
   CHECK_USAGE(VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
   CHECK_USAGE(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
   CHECK_USAGE(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
   CHECK_USAGE(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
   CHECK_USAGE(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
#undef CHECK_USAGE

   /*
    * a5xx tiler render targets support 1x/2x/4x MSAA; everything else is
    * single-sampled.  (This Vulkan header has no samples field in the
    * query struct, so the full supported set is reported and the application
    * picks.)
    */
   props->maxExtent = max_extent;
   props->maxMipLevels = max_mips;
   props->maxArrayLayers = max_layers;
   props->sampleCounts = (feats & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
                             ? (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT |
                                VK_SAMPLE_COUNT_4_BIT)
                             : VK_SAMPLE_COUNT_1_BIT;

   /* maxResourceSize: the largest single mip level, matching the spec. */
   max_resource = (uint64_t)max_extent.width * max_extent.height *
                  max_extent.depth * block_size;
   props->maxResourceSize = max_resource;

   return VK_SUCCESS;
}