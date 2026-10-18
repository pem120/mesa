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
 * \file ta5_render.cc
 *
 * Adreno 5xx Vulkan driver: image views, render passes, framebuffers and
 * pipeline layouts.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_math.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_memory.h"
#include "ta5_descriptor.h"
#include "ta5_formats.h"
#include "ta5_render.h"
#include "ta5_entrypoints.h"

/* ------------------------------------------------------------------ */
/* Image views                                                         */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateImageView(VkDevice _device, const VkImageViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_image_view *view;
   struct ta5_image *image;
   uint32_t block_size;
   uint32_t layers;
   uint32_t level_count;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   image = ta5_image_from_handle(pCreateInfo->image);

   if (getenv("TA5_TRACE_VIEW"))
      fprintf(stderr,
              "ta5[view] type=%d fmt=%d mips=%u baseMip=%u levels=%u "
              "baseLayer=%u layers=%u img(mips=%u layers=%u %ux%u tiling=%d) "
              "pitch=%u usage=0x%x\n",
              (int)pCreateInfo->viewType, (int)pCreateInfo->format,
              image->mip_levels, pCreateInfo->subresourceRange.baseMipLevel,
              pCreateInfo->subresourceRange.levelCount,
              pCreateInfo->subresourceRange.baseArrayLayer,
              pCreateInfo->subresourceRange.layerCount, image->mip_levels,
              image->array_layers, image->extent.width, image->extent.height,
              (int)image->tiling, 0u, (unsigned)image->usage);

   block_size = ta5_format_block_size(pCreateInfo->format);
   if (!block_size)
      return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);

   /*
    * The aspect has to be one the format actually has.
    *
    * This required VK_IMAGE_ASPECT_COLOR_BIT unconditionally, which is what a
    * depth attachment runs into first: vkCreateImageView on a D32_SFLOAT image
    * with aspectMask DEPTH came back VK_ERROR_FORMAT_NOT_SUPPORTED before any
    * depth code had run, so a render pass with a depth attachment could not be
    * built at all.  Measured, not inferred -- it is the error the first depth
    * case produced on the device.
    *
    * A depth format may be viewed as depth or stencil, because
    * D24_UNORM_S8_UINT has both and an application is entitled to ask for
    * either half.  A colour format may not be viewed as either: there is no
    * depth there to address.
    */
   if (pCreateInfo->subresourceRange.aspectMask == 0 ||
       (pCreateInfo->subresourceRange.aspectMask &
        (ta5_format_is_color(pCreateInfo->format)
             ? ~(VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT
             : ~(VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT |
                                     VK_IMAGE_ASPECT_STENCIL_BIT))))
      return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);

   /*
    * VK_REMAINING_MIP_LEVELS and VK_REMAINING_ARRAY_LAYERS both mean "to the
    * end of the image's levels/layers", and they are the idiomatic way to say
    * "all of them" -- several toolkits use nothing else.
    *
    * Taken literally they are 0xffffffff, so the range check further down
    * rejected every such view with VK_ERROR_FORMAT_NOT_SUPPORTED:
    *
    *     ta5[view] type=1 fmt=44 ... levels=4294967295 baseMip=0
    *
    * which is what stopped GTK Inspector under GSK_RENDERER=vulkan: nine of
    * its image views came back FORMAT_NOT_SUPPORTED and it then segfaulted.
    * vkcube and vkmark pass explicit counts, which is why they never hit it.
    */
   layers = pCreateInfo->subresourceRange.layerCount;
   if (layers == VK_REMAINING_ARRAY_LAYERS)
      layers = image->array_layers -
               pCreateInfo->subresourceRange.baseArrayLayer;

   level_count = pCreateInfo->subresourceRange.levelCount;
   if (level_count == VK_REMAINING_MIP_LEVELS)
      level_count = image->mip_levels -
                    pCreateInfo->subresourceRange.baseMipLevel;

   /*
    * The view type has to agree with the image, and the mismatches are the
    * ones that produce plausible-looking garbage rather than an error later:
    * a CUBE view over six layers is addressed with a direction vector and
    * picks the layer by sign of the dominant component, while a 2D_ARRAY view
    * is addressed by an index. Get this wrong and sampling returns the wrong
    * face of the wrong image rather than failing.
    */
   switch (pCreateInfo->viewType) {
   case VK_IMAGE_VIEW_TYPE_1D:
      if (image->image_type != VK_IMAGE_TYPE_1D)
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
      break;

   case VK_IMAGE_VIEW_TYPE_2D:
      if (image->image_type != VK_IMAGE_TYPE_2D)
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
      break;

   case VK_IMAGE_VIEW_TYPE_3D:
      if (image->image_type != VK_IMAGE_TYPE_3D || layers != 1)
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
      break;

   case VK_IMAGE_VIEW_TYPE_CUBE:
      if (image->image_type != VK_IMAGE_TYPE_2D || layers != 6 ||
          !(image->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
          pCreateInfo->subresourceRange.baseArrayLayer % 6 != 0)
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
      break;

   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      if (image->image_type != VK_IMAGE_TYPE_2D || layers == 0 || layers % 6 != 0 ||
          !(image->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ||
          pCreateInfo->subresourceRange.baseArrayLayer % 6 != 0)
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
      break;

   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      if (image->image_type != VK_IMAGE_TYPE_2D || layers <= 1)
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
      break;

   default:
      return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }

   if (pCreateInfo->subresourceRange.baseMipLevel + level_count >
          image->mip_levels ||
       pCreateInfo->subresourceRange.baseArrayLayer + layers >
          image->array_layers)
      return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);

   view = (struct ta5_image_view *)vk_zalloc(pAllocator, sizeof(*view), 8,
                                             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!view)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &view->base, VK_OBJECT_TYPE_IMAGE_VIEW);
   view->device = device;
   view->image = image;
   view->view_type = pCreateInfo->viewType;
   view->format = pCreateInfo->format;
   view->aspect = pCreateInfo->subresourceRange.aspectMask;
   view->base_mip_level = pCreateInfo->subresourceRange.baseMipLevel;
   view->level_count = level_count;
   view->base_array_layer = pCreateInfo->subresourceRange.baseArrayLayer;
   view->layer_count = layers;
   view->comp[0] = pCreateInfo->components.r;
   view->comp[1] = pCreateInfo->components.g;
   view->comp[2] = pCreateInfo->components.b;
   view->comp[3] = pCreateInfo->components.a;
   /*
    * The row pitch is the image's, whatever the tiling flag says.
    *
    * This zeroed the pitch for anything that was not VK_IMAGE_TILING_LINEAR,
    * on the reasoning that an optimal image is gmem and is addressed by the
    * tile system rather than by a stride.  That reasoning does not hold here:
    * ta5_CreateImage lays every image out linearly and computes image->row_pitch
    * from extent and block size with no reference to tiling at all, and the RB
    * addresses attachments with RB_DEPTH_BUFFER_PITCH and the colour buffer
    * pitch the same way.  So an optimal-tiled attachment got a pitch of zero,
    * the register was programmed with zero, and the depth buffer was addressed
    * as if every row started at the same address.
    *
    * vkcube is what found it, because vkcube creates its depth attachment with
    * VK_IMAGE_TILING_OPTIMAL.  Its depth test then compared fragments against
    * the wrong rows, and roughly two thirds of the cube was rejected: the
    * picture came out as a few disconnected faces with the background showing
    * between them, and with no fault, because nothing was wrong from the
    * hardware's point of view.
    *
    * Every one of the 115 regression cases creates its depth attachment with
    * linear tiling, which is why this survived all of them -- and why the
    * harness hid it rather than exposing it: with a zero pitch the depth test
    * reads rows that nothing ever wrote, so nothing is ever rejected and a
    * single non-self-occluding draw still comes out complete.
    */
   view->row_pitch = image->row_pitch;

   *pView = ta5_image_view_to_handle(view);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyImageView(VkDevice _device, VkImageView _view,
                     const VkAllocationCallbacks *pAllocator)
{
   struct ta5_image_view *view = ta5_image_view_from_handle(_view);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_free(pAllocator, view);
}

/* ------------------------------------------------------------------ */
/* Render passes                                                       */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateRenderPass(VkDevice _device,
                     const VkRenderPassCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkRenderPass *pRenderPass)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_render_pass *rp;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   rp = (struct ta5_render_pass *)vk_zalloc(pAllocator, sizeof(*rp), 8,
                                            VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!rp)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &rp->base, VK_OBJECT_TYPE_RENDER_PASS);
   rp->device = device;
   rp->attachment_count = pCreateInfo->attachmentCount;
   rp->subpass_count = pCreateInfo->subpassCount;
   rp->dependency_count = pCreateInfo->dependencyCount;

   rp->attachments = (struct ta5_render_pass_attachment *)vk_zalloc(
      pAllocator, rp->attachment_count * sizeof(*rp->attachments), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!rp->attachments) {
      vk_free(pAllocator, rp);
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   for (uint32_t i = 0; i < rp->attachment_count; i++) {
      const VkAttachmentDescription *a = &pCreateInfo->pAttachments[i];
      struct ta5_render_pass_attachment *dst = &rp->attachments[i];

      if (!ta5_format_block_size(a->format)) {
         vk_free(pAllocator, rp->attachments);
         vk_free(pAllocator, rp);
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
      }

      dst->format = a->format;
      dst->samples = a->samples;
      dst->initial_layout = a->initialLayout;
      dst->final_layout = a->finalLayout;
      /*
       * Vulkan 1.1's VkAttachmentDescription has no usage field (that arrived
       * in 1.2), so whether an attachment is a colour target is taken from
       * its format instead.
       */
      dst->used_as_color = ta5_format_is_color(a->format);
      dst->load_op = a->loadOp;
   }
   /*
    * The render area varies at tile granularity.
    *
    * This is tile_align_w/h, NOT tile_max_w/h.  The "max" fields are upper
    * bounds on what the hardware will accept -- they read 1024x4064 on the
    * 506, which are the largest values the RB bin-size bitfield can encode
    * (freedreno_devices.py even annotates them max_bitfield_val(7, 0, 5)),
    * not the tile size.  freedreno computes bin sizes aligned to
    * tile_align in layout_gmem(), so 64x32 is the real granularity.
    */
   rp->granularity.width =
      MAX2(1u, device->physical_device->gpu_info.tile_align_w);
   rp->granularity.height =
      MAX2(1u, device->physical_device->gpu_info.tile_align_h);

   *pRenderPass = ta5_render_pass_to_handle(rp);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyRenderPass(VkDevice _device, VkRenderPass _renderPass,
                      const VkAllocationCallbacks *pAllocator)
{
   struct ta5_render_pass *rp = ta5_render_pass_from_handle(_renderPass);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_free(pAllocator, rp->attachments);
   vk_free(pAllocator, rp);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetRenderAreaGranularity(VkDevice _device, VkRenderPass _renderPass,
                             VkExtent2D *pGranularity)
{
   struct ta5_render_pass *rp = ta5_render_pass_from_handle(_renderPass);

   *pGranularity = rp->granularity;
}

/* ------------------------------------------------------------------ */
/* Framebuffers                                                         */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateFramebuffer(VkDevice _device,
                      const VkFramebufferCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkFramebuffer *pFramebuffer)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_render_pass *rp =
      ta5_render_pass_from_handle(pCreateInfo->renderPass);
   struct ta5_framebuffer *fb;

   if (pCreateInfo->attachmentCount > ARRAY_SIZE(fb->attachments)) {
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   fb = (struct ta5_framebuffer *)vk_zalloc(pAllocator, sizeof(*fb), 8,
                                            VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!fb)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &fb->base, VK_OBJECT_TYPE_FRAMEBUFFER);
   fb->device = device;
   fb->render_pass = rp;
   fb->width = pCreateInfo->width;
   fb->height = pCreateInfo->height;
   fb->flags = pCreateInfo->flags;
   fb->attachment_count = pCreateInfo->attachmentCount;

   for (uint32_t i = 0; i < fb->attachment_count; i++) {
      struct ta5_image_view *view =
         ta5_image_view_from_handle(pCreateInfo->pAttachments[i]);

      if (view->format != rp->attachments[i].format) {
         vk_free(pAllocator, fb);
         return vk_error(&device->instance->vk,
                         VK_ERROR_FORMAT_NOT_SUPPORTED);
      }

      fb->attachments[i] = view;
   }

   *pFramebuffer = ta5_framebuffer_to_handle(fb);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyFramebuffer(VkDevice _device, VkFramebuffer _framebuffer,
                       const VkAllocationCallbacks *pAllocator)
{
   struct ta5_framebuffer *fb = ta5_framebuffer_from_handle(_framebuffer);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_free(pAllocator, fb);
}

/* ------------------------------------------------------------------ */
/* Pipeline layouts                                                     */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreatePipelineLayout(VkDevice _device,
                         const VkPipelineLayoutCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkPipelineLayout *pPipelineLayout)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_pipeline_layout *layout;

   if (pCreateInfo->setLayoutCount >
       device->physical_device->vk.properties.maxBoundDescriptorSets)
      return vk_error(&device->instance->vk,
                      VK_ERROR_INITIALIZATION_FAILED);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   layout = (struct ta5_pipeline_layout *)vk_zalloc(pAllocator, sizeof(*layout),
                                                    8,
                                                    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!layout)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &layout->base,
                       VK_OBJECT_TYPE_PIPELINE_LAYOUT);
   layout->device = device;
   layout->set_count = pCreateInfo->setLayoutCount;

   layout->set_layouts = (struct ta5_descriptor_set_layout **)vk_zalloc(
      pAllocator, layout->set_count * sizeof(*layout->set_layouts), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!layout->set_layouts) {
      vk_free(pAllocator, layout);
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /*
    * The bindful model charges hardware descriptor slots per binding: a
    * combined image sampler costs two (texture array plus sampler array),
    * everything else costs one.  Accumulating the total here is what a
    * pipeline later checks against what the hardware can address.
    */
   for (uint32_t i = 0; i < layout->set_count; i++) {
      struct ta5_descriptor_set_layout *sl =
         ta5_descriptor_set_layout_from_handle(pCreateInfo->pSetLayouts[i]);

      layout->set_layouts[i] = sl;
      layout->total_slots += sl->total_slots;
   }

   *pPipelineLayout = ta5_pipeline_layout_to_handle(layout);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyPipelineLayout(VkDevice _device, VkPipelineLayout _pipelineLayout,
                          const VkAllocationCallbacks *pAllocator)
{
   struct ta5_pipeline_layout *layout =
      ta5_pipeline_layout_from_handle(_pipelineLayout);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_free(pAllocator, layout->set_layouts);
   vk_free(pAllocator, layout);
}