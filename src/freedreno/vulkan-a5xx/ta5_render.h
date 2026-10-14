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
 * \file ta5_render.h
 *
 * Adreno 5xx Vulkan driver: image views, render passes, framebuffers and
 * pipeline layouts.
 */

#ifndef TA5_RENDER_H
#define TA5_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "util/list.h"
#include "vk_object.h"
#include "vk_device.h"

#include "ta5_instance.h"
#include "ta5_descriptor.h"

/**
 * Image view.
 *
 * A5xx samples and renders through fixed-format views; the tile size the
 * view's extent implies is what the render target configuration is derived
 * from at bin time.
 */
struct ta5_image_view
{
   struct vk_object_base base;
   struct ta5_device *device;

   struct ta5_image *image;
   VkImageViewType view_type;
   VkFormat format;
   VkImageAspectFlags aspect;
   uint32_t base_mip_level;
   uint32_t level_count;
   uint32_t base_array_layer;
   uint32_t layer_count;

   /**
    * Component swizzle, VkComponentSwizzle in G, B, R, A order.
    *
    * a5xx has this in the texel descriptor (A5XX_TEX_CONST_0_SWAP), so unlike
    * the image types it is not something the hardware infers from the view.
    */
   uint8_t comp[4];

   /** Linear row pitch when the view is linear-tiled, else 0. */
   uint32_t row_pitch;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_image_view, base, VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW)

/** One attachment description, colour or depth/stencil. */
struct ta5_render_pass_attachment
{
   VkFormat format;
   VkSampleCountFlagBits samples;
   VkImageLayout initial_layout;
   VkImageLayout final_layout;
   bool used_as_color;

   /** Load op, so the command buffer knows whether to clear on begin. */
   VkAttachmentLoadOp load_op;
   VkClearValue clear_value;
};

/**
 * Render pass.
 *
 * Single subpass only.  a5xx bins and renders in one pass anyway (the
 * bin/render split is a hardware command stream feature, not a Vulkan
 * subpass feature), so a subpass loop would need explicit PM4 work with no
 * benefit here.
 */
struct ta5_render_pass
{
   struct vk_object_base base;
   struct ta5_device *device;

   uint32_t attachment_count;
   struct ta5_render_pass_attachment *attachments;

   uint32_t subpass_count;
   uint32_t dependency_count;

   VkExtent2D granularity;
};

/*
 * The depth/stencil attachment is found by format, not by index.
 *
 * Vulkan makes no promise about where it sits in pAttachments -- it only says
 * the subpass's pDepthStencilAttachment names it -- so "attachment 1" is not a
 * rule, it is a coincidence of the order one particular application happened
 * to pass.  ta5_fill_render_pass_state() already classifies the same way, and
 * two places answering "which attachment is the depth one" differently is how
 * the clear ends up going to a colour target.
 */
static inline int
ta5_render_pass_depth_attachment(const struct ta5_render_pass *pass)
{
   for (uint32_t i = 0; i < pass->attachment_count; i++)
      if (!pass->attachments[i].used_as_color)
         return (int)i;
   return -1;
}

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_render_pass, base, VkRenderPass,
                               VK_OBJECT_TYPE_RENDER_PASS)

struct ta5_framebuffer
{
   struct vk_object_base base;
   struct ta5_device *device;

   struct ta5_render_pass *render_pass;
   VkFramebufferCreateFlags flags;

   uint32_t width;
   uint32_t height;
   uint32_t attachment_count;
   struct ta5_image_view *attachments[8];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_framebuffer, base, VkFramebuffer,
                               VK_OBJECT_TYPE_FRAMEBUFFER)

/**
 * Pipeline layout.
 *
 * This is where the bindful descriptor model bites: the number of hardware
 * descriptor slots a layout needs is the sum over bindings of
 * class-cost x descriptorCount, and that total is what a pipeline must be
 * able to address.  See ta5_descriptor.h.
 */
struct ta5_pipeline_layout
{
   struct vk_object_base base;
   struct ta5_device *device;

   uint32_t set_count;
   struct ta5_descriptor_set_layout **set_layouts;

   /** Total hardware descriptor slots across all sets. */
   uint32_t total_slots;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_pipeline_layout, base, VkPipelineLayout,
                               VK_OBJECT_TYPE_PIPELINE_LAYOUT)

#endif /* TA5_RENDER_H */