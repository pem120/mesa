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
 * \file ta5_pipeline.h
 *
 * Adreno 5xx Vulkan driver: graphics pipelines.
 */

#ifndef TA5_PIPELINE_H
#define TA5_PIPELINE_H

#include <stdint.h>

#include "vk_object.h"
#include "vk_device.h"
#include "vk_graphics_state.h"

#include "ta5_instance.h"
#include "ta5_shader.h"
#include "ta5_render.h"

/**
 * Graphics pipeline.
 *
 * Holds both compiled stages plus the fixed-function state needed at draw
 * time.  a5xx runs one binning pass followed by one render pass, so the
 * state the GPU needs is emitted in that order rather than per-draw.
 */
struct ta5_pipeline
{
   struct vk_object_base base;
   struct ta5_device *device;
   struct ta5_pipeline_layout *layout;
   struct ta5_render_pass *render_pass;
   uint32_t subpass;

   /** Runtime-owned graphics state, filled from our render pass. */
   struct vk_graphics_pipeline_state graphics_state;

   struct ta5_compiled_shader vertex;
   struct ta5_compiled_shader fragment;

   bool has_vertex;
   bool has_fragment;

   /** Shaders were given as gl_Position/gl_FragCoord-only; no vertex buffer
    * fetch is configured yet. */
   bool uses_vertex_buffers;

   /*
    * Fixed-function state is borrowed from the VkGraphicsPipelineCreateInfo,
    * not copied: Vulkan 1.1 passes each piece as a pointer to the state
    * struct itself (there is no enclosing pNext-style state member), and the
    * create info is guaranteed to outlive pipeline creation.
    */
   const VkPipelineRasterizationStateCreateInfo *raster;
   const VkPipelineColorBlendStateCreateInfo *blend;
   const VkPipelineDepthStencilStateCreateInfo *depth_stencil;
   const VkPipelineMultisampleStateCreateInfo *multisample;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_pipeline, base, VkPipeline,
                               VK_OBJECT_TYPE_PIPELINE)

#endif /* TA5_PIPELINE_H */