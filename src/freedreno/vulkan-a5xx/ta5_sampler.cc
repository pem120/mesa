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
   if (filter == VK_FILTER_LINEAR && maxAnisotropy > 1.0f && magnification)
      return A5XX_TEX_ANISO;
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

   if (pCreateInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST) {
      /* The hardware has no "pick one level" mode: it always filters between
       * MIN_LOD and MAX_LOD.  Collapsing the pair onto the nearer of the two
       * is what makes a NEAREST mipmap mode behave. */
      min_lod = MAX2(min_lod, max_lod - 0.125f);
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
      return vk_error(&device->instance->vk, VK_ERROR_FEATURE_NOT_PRESENT);

   /* Aniso filtering is not implemented: the register exists and is
    * programmed, but the shader lowering for it is not wired up, and
    * pretending otherwise would give the wrong answer silently. */
   if (pCreateInfo->maxAnisotropy > 1.0f)
      return vk_error(&device->instance->vk, VK_ERROR_FEATURE_NOT_PRESENT);

   if (pCreateInfo->compareEnable &&
       pCreateInfo->compareOp != VK_COMPARE_OP_NEVER)
      return vk_error(&device->instance->vk, VK_ERROR_FEATURE_NOT_PRESENT);

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

   vk_free(pAllocator, sampler);
}