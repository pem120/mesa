/*
 * Texel descriptors.
 *
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 */

/**
 * \file ta5_texel.cc
 */

#include "ta5_texel.h"
#include "ta5_memory.h"
#include "ta5_render.h"
#include "ta5_device.h"

#include "fd5_hw.h" /* a5xx.xml.h */

#include "util/format/u_format.h"

/* ------------------------------------------------------------------ */
/* Format mapping                                                      */
/* ------------------------------------------------------------------ */

uint32_t
ta5_texel_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_UNORM:
   case VK_FORMAT_R8_SRGB:
      return TFMT5_8_UNORM;
   case VK_FORMAT_R8_SNORM:
      return TFMT5_8_SNORM;
   case VK_FORMAT_R8_UINT:
      return TFMT5_8_UINT;
   case VK_FORMAT_R8_SINT:
      return TFMT5_8_SINT;
   case VK_FORMAT_A8_UNORM:
      return TFMT5_A8_UNORM;

   case VK_FORMAT_R8G8_UNORM:
      return TFMT5_8_8_UNORM;
   case VK_FORMAT_R8G8_UINT:
      return TFMT5_8_8_UINT;
   case VK_FORMAT_R8G8_SINT:
      return TFMT5_8_8_SINT;

   case VK_FORMAT_R8G8B8A8_UNORM:
      return TFMT5_8_8_8_8_UNORM;
   case VK_FORMAT_R8G8B8A8_SRGB:
      /* a5xx spells sRGB formats by the _UNORM constant plus the SRGB bit in
       * TEX_CONST_0, which is what the caller sets from ta5_format_is_srgb. */
      return TFMT5_8_8_8_8_UNORM;
   case VK_FORMAT_R8G8B8A8_SNORM:
      return TFMT5_8_8_8_8_SNORM;
   case VK_FORMAT_R8G8B8A8_UINT:
      return TFMT5_8_8_8_8_UINT;
   case VK_FORMAT_R8G8B8A8_SINT:
      return TFMT5_8_8_8_8_SINT;

   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return TFMT5_8_8_8_8_UNORM;
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
      return TFMT5_8_8_8_8_UNORM;

   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
      return TFMT5_10_10_10_2_UNORM;

   case VK_FORMAT_R16_UNORM:
      return TFMT5_16_UNORM;
   case VK_FORMAT_R16_SNORM:
      return TFMT5_16_SNORM;
   case VK_FORMAT_R16_UINT:
      return TFMT5_16_UINT;
   case VK_FORMAT_R16_SINT:
      return TFMT5_16_SINT;

   case VK_FORMAT_R16G16_UNORM:
      return TFMT5_16_16_UNORM;
   case VK_FORMAT_R16G16_SNORM:
      return TFMT5_16_16_SNORM;
   case VK_FORMAT_R16G16_SFLOAT:
      return TFMT5_16_16_FLOAT;
   case VK_FORMAT_R16G16_UINT:
      return TFMT5_16_16_UINT;
   case VK_FORMAT_R16G16_SINT:
      return TFMT5_16_16_SINT;

   case VK_FORMAT_R16G16B16A16_UNORM:
      return TFMT5_16_16_16_16_UNORM;
   case VK_FORMAT_R16G16B16A16_SNORM:
      return TFMT5_16_16_16_16_SNORM;
   case VK_FORMAT_R16G16B16A16_SFLOAT:
      return TFMT5_16_16_16_16_FLOAT;
   case VK_FORMAT_R16G16B16A16_UINT:
      return TFMT5_16_16_16_16_UINT;
   case VK_FORMAT_R16G16B16A16_SINT:
      return TFMT5_16_16_16_16_SINT;

   case VK_FORMAT_R32_UINT:
      return TFMT5_32_UINT;
   case VK_FORMAT_R32_SINT:
      return TFMT5_32_SINT;

   case VK_FORMAT_R32G32_UINT:
      return TFMT5_32_32_UINT;
   case VK_FORMAT_R32G32_SINT:
      return TFMT5_32_32_SINT;

   case VK_FORMAT_R32G32B32A32_UINT:
      return TFMT5_32_32_32_32_UINT;
   case VK_FORMAT_R32G32B32A32_SINT:
      return TFMT5_32_32_32_32_SINT;

   default:
      return TFMT5_NONE;
   }
}

/** Is this format one the fetch unit must treat as sRGB? */
static bool
ta5_texel_is_srgb(VkFormat format)
{
   return format == VK_FORMAT_R8G8B8A8_SRGB ||
          format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8_SRGB;
}

/* ------------------------------------------------------------------ */
/* Descriptor                                                          */
/* ------------------------------------------------------------------ */

/** a5xx texel type for a view, from fd5_tex_type() (fd5_texture.h:47). */
static enum a5xx_tex_type
ta5_texel_type(VkImageViewType view_type)
{
   switch (view_type) {
   case VK_IMAGE_VIEW_TYPE_1D:
      return A5XX_TEX_1D;
   case VK_IMAGE_VIEW_TYPE_3D:
      return A5XX_TEX_3D;
   case VK_IMAGE_VIEW_TYPE_CUBE:
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      return A5XX_TEX_CUBE;
   default:
      return A5XX_TEX_2D;
   }
}

/**
 * Component swizzle, from the view's VkComponentMapping.
 *
 * There are two separate things in TEX_CONST_0 and they are easy to confuse:
 * SWIZ_X..W select *which* stored component each result component reads, while
 * SWAP selects how the stored bytes are ordered.  Only the first is the view's
 * component mapping.
 *
 * fd5_tex_swiz() applies the standard Vulkan rule that
 * VK_COMPONENT_SWIZZLE_IDENTITY means "same as the component index", so an
 * identity mapping on a two-channel format leaves .b reading .a rather than
 * reading a component the format does not have.
 */
static uint32_t
ta5_texel_swizzle(const struct ta5_image_view *view)
{
   enum a5xx_tex_swiz c[4];

   for (int i = 0; i < 4; i++) {
      switch (view->comp[i]) {
      case VK_COMPONENT_SWIZZLE_ZERO:
         c[i] = A5XX_TEX_ZERO;
         break;
      case VK_COMPONENT_SWIZZLE_ONE:
         c[i] = A5XX_TEX_ONE;
         break;
      default:
         c[i] = (enum a5xx_tex_swiz)i;
         break;
      }
   }

   return A5XX_TEX_CONST_0_SWIZ_X(c[0]) | A5XX_TEX_CONST_0_SWIZ_Y(c[1]) |
          A5XX_TEX_CONST_0_SWIZ_Z(c[2]) | A5XX_TEX_CONST_0_SWIZ_W(c[3]);
}

/**
 * Byte order of the stored texel, from the format.
 *
 * B8G8R8A8 and A8B8G8R8 store the colour channels in the opposite order from
 * the sampler result, so the hardware is told to swap while sampling.  Without
 * this, a B8G8R8A8 texture reads back with red and blue transposed -- which
 * looks like a shader bug rather than a format one.
 */
static enum a3xx_color_swap
ta5_texel_swap(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return WZYX;
   default:
      return XYZW;
   }
}

unsigned
ta5_texel_descriptor_count(const struct ta5_image_view *view)
{
   /* A cube array is several cubes, but a5xx fetches a cube as one unit: the
    * hardware picks the face from the direction vector, and the cube array
    * index is folded into the descriptor's array pitch by the shader. */
   return 1;
}

void
ta5_texel_descriptor(const struct ta5_image_view *view, uint32_t *desc,
                     uint64_t *address)
{
   const struct ta5_image *image = view->image;
   const struct ta5_image_level *lvl =
      &image->levels[MIN2(view->base_mip_level, image->mip_levels - 1)];
   enum a5xx_tex_type type = ta5_texel_type(view->view_type);
   unsigned miplevels = view->level_count - 1;
   enum a5xx_tex_fmt fmt =
      (enum a5xx_tex_fmt)ta5_texel_format(view->format);
   VkDeviceSize offset;

   memset(desc, 0, TA5_TEXEL_DESC_DWORDS * sizeof(uint32_t));

   /* TEX_CONST_0: format, mip count, component swizzle, tiling. */
   desc[0] = A5XX_TEX_CONST_0_FMT(fmt) |
             A5XX_TEX_CONST_0_MIPLVLS(miplevels) |
             ta5_texel_swizzle(view) | /* the four SWIZ fields, non-overlapping */
             A5XX_TEX_CONST_0_SWAP(ta5_texel_swap(view->format)) |
             A5XX_TEX_CONST_0_TILE_MODE(TILE5_LINEAR);

   if (ta5_texel_is_srgb(view->format))
      desc[0] |= A5XX_TEX_CONST_0_SRGB;

   /* TEX_CONST_1: width and height of the view's base mip level. */
   desc[1] =
      A5XX_TEX_CONST_1_WIDTH(lvl->width) | A5XX_TEX_CONST_1_HEIGHT(lvl->height);

   /* TEX_CONST_2: pitch, its alignment, and the texel type. */
   desc[2] = A5XX_TEX_CONST_2_PITCHALIGN(0) |
             A5XX_TEX_CONST_2_PITCH(lvl->row_pitch / 64) |
             A5XX_TEX_CONST_2_TYPE(type);

   switch (view->view_type) {
   case VK_IMAGE_VIEW_TYPE_3D:
      /*
       * A 3D image has no layer pitch, so the slice stride goes in
       * MIN_LAYERSZ and the total level size in ARRAY_PITCH.  Both must come
       * from this level's layout: level 0 of a mipped 3D image is not the same
       * size as level 0 of a 2D one, and using the wrong one makes the fetch
       * walk off the end of the image on the last slices.
       */
      desc[3] = A5XX_TEX_CONST_3_MIN_LAYERSZ(lvl->layer_pitch / 64) |
                A5XX_TEX_CONST_3_ARRAY_PITCH(lvl->level_size / 64);
      desc[5] = A5XX_TEX_CONST_5_DEPTH(lvl->depth);
      break;

   case VK_IMAGE_VIEW_TYPE_CUBE:
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      desc[3] = A5XX_TEX_CONST_3_ARRAY_PITCH(lvl->layer_pitch);
      desc[5] = A5XX_TEX_CONST_5_DEPTH(view->layer_count / 6);
      break;

   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      desc[3] = A5XX_TEX_CONST_3_ARRAY_PITCH(lvl->layer_pitch);
      desc[5] = A5XX_TEX_CONST_5_DEPTH(view->layer_count);
      break;

   default:
      desc[3] = A5XX_TEX_CONST_3_ARRAY_PITCH(lvl->layer_pitch);
      desc[5] = A5XX_TEX_CONST_5_DEPTH(1);
      break;
   }

   /*
    * The address is a relocation, not a value: the kernel resolves the IOVA at
    * submit time, and until then this slot must hold nothing but the OR mask.
    * It is returned separately so the caller can emit it as a reloc.
    */
   offset = image->offset + lvl->offset +
            (uint64_t)view->base_array_layer * lvl->layer_pitch;
   *address = offset;
}