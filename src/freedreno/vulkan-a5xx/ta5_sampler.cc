/*
 * Samplers.
 *
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 */

#include "ta5_sampler.h"
#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_entrypoints.h"

#include "fd5_hw.h" /* a5xx_tex_filter, a5xx_tex_clamp, TEX_SAMP macros */
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_util.h"

/** Dwords in one a5xx sampler: TEX_SAMP0..3. */
#define TA5_SAMPLER_DESC_DWORDS 4

/** Anisotropy, as a power of two, from the Vulkan float. */
static enum a5xx_tex_aniso
ta5_sampler_aniso(float maxAnisotropy)
{
   if (maxAnisotropy <= 1.0f)
      return A5XX_TEX_ANISO_1;
   if (maxAnisotropy <= 2.0f)
      return A5XX_TEX_ANISO_2;
   if (maxAnisotropy <= 4.0f)
      return A5XX_TEX_ANISO_4;
   if (maxAnisotropy <= 8.0f)
      return A5XX_TEX_ANISO_8;
   return A5XX_TEX_ANISO_16;
}

/**
 * Address mode from a Vulkan sampler address mode.
 *
 * A5XX has no separate mirror-to-edge mode; MIRROR_REPEAT and
 * MIRROR_CLAMP_TO_EDGE both mean "mirror", and MIRROR_CLAMP_TO_BORDER has no
 * equivalent, so it degrades to clamp-to-edge rather than sampling memory
 * outside the image.  Border colour is a later addition and is not plumbed
 * here, so a real border would be worse than a clamp.
 */
static enum a5xx_tex_clamp
ta5_sampler_wrap(VkSamplerAddressMode mode)
{
   switch (mode) {
   case VK_SAMPLER_ADDRESS_MODE_REPEAT:
      return A5XX_TEX_REPEAT;
   case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
      return A5XX_TEX_MIRROR_REPEAT;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
      return A5XX_TEX_CLAMP_TO_EDGE;
   case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
   default:
      return A5XX_TEX_CLAMP_TO_EDGE;
   }
}

static enum a5xx_tex_filter
ta5_sampler_filter(VkFilter filter, float maxAnisotropy, bool magnification)
{
   /*
    * With aniso on, LINEAR becomes the hardware's ANISO filter for
    * magnification *and* minification -- that is what fd5's tex_filter() does
    * (fd5_texture.c:46), and aniso on minification is the entire point of
    * the feature.
    */
   if (filter == VK_FILTER_LINEAR && maxAnisotropy > 1.0f)
      return A5XX_TEX_ANISO;
   (void)magnification;
   return filter == VK_FILTER_LINEAR ? A5XX_TEX_LINEAR : A5XX_TEX_NEAREST;
}

/**
 * Build the four sampler dwords.
 *
 * Transcribed from fd5_sampler_state_create() (fd5_texture.c:59), which
 * produces the same structure as texsamp0..3.
 */
static void
ta5_sampler_encode(const VkSamplerCreateInfo *pCreateInfo, uint32_t *desc)
{
   enum a5xx_tex_aniso aniso = ta5_sampler_aniso(pCreateInfo->maxAnisotropy);
   enum a5xx_tex_filter mag = ta5_sampler_filter(pCreateInfo->magFilter,
                                                 pCreateInfo->maxAnisotropy,
                                                 true);
   enum a5xx_tex_filter min = ta5_sampler_filter(pCreateInfo->minFilter,
                                                 pCreateInfo->maxAnisotropy,
                                                 false);
   bool miplinear = pCreateInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR;
   float min_lod = pCreateInfo->minLod;
   float max_lod = pCreateInfo->maxLod;

   /*
    * VK_SAMPLER_MIPMAP_MODE_NEAREST, on this hardware's terms.
    *
    * a5xx has no "select exactly level N" mode.  The LOD carried in the
    * instruction is used to interpolate *within* [MIN_LOD, MAX_LOD]: frac =
    * LOD - floor(LOD) blends levels floor(LOD) and floor(LOD)+1, so the only
    * way to land on one level is for both ends of the interval to sit inside
    * [N, N+1).  fd5_texture.c:99 does the same thing for the equivalent GL
    * case, clamping both ends so the hardware can still choose between mag
    * and min of level 0.
    *
    * Two narrower forms were measured here first, and both left the chain
    * unreachable:
    *
    *   min_lod = MAX2(min_lod, max_lod - 0.125f)      (the previous code)
    *      Only ever raises MIN_LOD.  With minLod 0 and maxLod 3 -- an ordinary
    *      four-level chain -- this is a no-op, the interval stayed [0, 3], and
    *      every level came back as a blend of levels 0 and 1.
    *
    *   min_lod = MIN2(min_lod + 0.125f, max_lod)
    *      Raises MIN_LOD only when it is already at the bottom, so LAYER=0
    *      produced level 0 and LAYER=1, 2 and 3 all produced level 1.
    *
    * So both ends move, each into its own level.  The 0.125 is a quarter of
    * the value GL uses: a5xx quantises LOD to 1/8, so it has to be small
    * enough not to reach into the next level and large enough not to quantise
    * away to nothing.
    */
   if (pCreateInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST) {
      float lo = CLAMP(floorf(min_lod), 0.0f, max_lod);
      float hi = CLAMP(floorf(max_lod), 0.0f, max_lod);

      min_lod = lo + 0.125f;
      max_lod = hi + 0.125f;
   }

   desc[0] = A5XX_TEX_SAMP_0_XY_MAG(mag) | A5XX_TEX_SAMP_0_XY_MIN(min) |
             A5XX_TEX_SAMP_0_ANISO(aniso) |
             A5XX_TEX_SAMP_0_WRAP_S(
                ta5_sampler_wrap(pCreateInfo->addressModeU)) |
             A5XX_TEX_SAMP_0_WRAP_T(
                ta5_sampler_wrap(pCreateInfo->addressModeV)) |
             A5XX_TEX_SAMP_0_WRAP_R(
                ta5_sampler_wrap(pCreateInfo->addressModeW)) |
             A5XX_TEX_SAMP_0_LOD_BIAS(pCreateInfo->mipLodBias);

   desc[1] = A5XX_TEX_SAMP_1_MIN_LOD(min_lod) |
             A5XX_TEX_SAMP_1_MAX_LOD(max_lod);

   /*
    * Unnormalized coordinates let a shader index texels by integer rather
    * than by normalised coordinate.  fd5 only sets it when the Gallium
    * sampler asked for it, and here it is the whole point of
    * unnormalizedCoordinates, so it is set whenever the caller asked.
    */
   if (pCreateInfo->unnormalizedCoordinates)
      desc[1] |= A5XX_TEX_SAMP_1_UNNORM_COORDS;

   desc[2] = A5XX_TEX_SAMP_2_BCOLOR_OFFSET(0);
   desc[3] = 0;

   if (miplinear)
      desc[1] |= A5XX_TEX_SAMP_1_MIPFILTER_LINEAR_FAR;

   return;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateSampler(VkDevice _device, const VkSamplerCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_sampler *sampler;

   /*
    * vk_alloc() dereferences the allocator without checking it, so a NULL one
    * has to be replaced before any allocation here.
    */
   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   if (pCreateInfo->mipLodBias > 14.0f || pCreateInfo->mipLodBias < -14.0f)
      return vk_error(&device->physical_device->vk, VK_ERROR_FEATURE_NOT_PRESENT);

   /*
    * Aniso is accepted: the sampler descriptor carries it (TEX_SAMP_0_ANISO
    * plus the ANISO filter, the way fd5_sampler_state_create() encodes it for
    * GL) and the shader needs nothing -- on a5xx aniso is sampler state, not
    * a texture-instruction modifier.
    */

   if (pCreateInfo->compareEnable &&
       pCreateInfo->compareOp != VK_COMPARE_OP_NEVER)
      return vk_error(&device->physical_device->vk, VK_ERROR_FEATURE_NOT_PRESENT);

   sampler = (struct ta5_sampler *)vk_zalloc(
      pAllocator, sizeof(*sampler), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!sampler)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &sampler->base, VK_OBJECT_TYPE_SAMPLER);
   sampler->device = device;
   ta5_sampler_encode(pCreateInfo, sampler->desc);

   *pSampler = ta5_sampler_to_handle(sampler);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroySampler(VkDevice _device, VkSampler _sampler,
                   const VkAllocationCallbacks *pAllocator)
{
   struct ta5_sampler *sampler = ta5_sampler_from_handle(_sampler);

   if (!sampler)
      return;

   /*
    * pAllocator may legitimately be NULL -- the spec permits it on every
    * entry point -- and vk_free() dereferences it unconditionally
    * (vk_alloc.h:78), so it has to be normalised here.
    *
    * ta5_CreateSampler() does this, which is why create-then-destroy worked
    * as long as the caller happened to pass an allocator both times.  Every
    * other destroy path in this driver normalises too; this one did not, and
    * vkDestroySampler(dev, sampler, NULL) segfaulted in vk_free.  Found by
    * valgrind once the harness was given a teardown -- before that, nothing
    * in the matrix ever called a destroy at all.
    */
   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_free(pAllocator, sampler);
}