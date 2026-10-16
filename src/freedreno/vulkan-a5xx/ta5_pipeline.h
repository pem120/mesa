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

   /*
    * Runtime-owned graphics state, filled from our render pass.
    *
    * This is also where the fixed-function pipeline state lives -- rasterizer
    * (graphics_state.rs), colour blend (.cb), depth stencil (.ds),
    * multisample (.ms) and vertex input (.vi) -- so nothing in ta5_pipeline
    * should ever hold a pointer into VkGraphicsPipelineCreateInfo.
    *
    * Those pointers would be borrowed, not owned.  The spec states the rule
    * outright for sibling members of the same structure: renderPass "must not
    * be accessed by the implementation outside of the duration of the command
    * this structure is passed to", and layout is held to the same rule from
    * Vulkan 1.3.  An application is entitled to release the state structures
    * the moment vkCreateGraphicsPipelines returns, so a pipeline holding
    * pRasterizationState and friends would be holding dangling pointers by the
    * time it is handed back.
    *
    * vk_graphics_pipeline_state_fill() deep copies all of it into all_state
    * below, which the pipeline owns and which is therefore valid for the
    * pipeline's whole life.  Read the state from there.
    */
   struct vk_graphics_pipeline_state graphics_state;

   /*
    * Backing store for graphics_state.  The runtime plants pointers into
    * this (state->mv points at all_state.mv), so it has to outlive the
    * vk_graphics_pipeline_state_fill() call -- a stack local would dangle.
    */
   struct vk_graphics_pipeline_all_state all_state;

   struct ta5_compiled_shader vertex;
   struct ta5_compiled_shader fragment;

   bool has_vertex;
   bool has_fragment;

   /**
    * Compiled shader microcode, staged in one buffer.
    *
    * The shader fetch unit wants the instruction stream in memory, so this is
    * uploaded with CP_LOAD_STATE4/SS4_INDIRECT rather than inline in the
    * command stream.  Emitting it inline (SS4_DIRECT) is something the GL
    * backend only does behind its DIRECT debug flag, and the GPU faults on
    * it here -- the CP opcode error lands mid-upload.
    */
   struct ta5_bo *vs_bo;
   struct ta5_bo *fs_bo;

   /** Shaders were given as gl_Position/gl_FragCoord-only; no vertex buffer
    * fetch is configured yet. */
   bool uses_vertex_buffers;

   /** pRasterizationState: which faces are discarded before rasterisation,
    * and which winding counts as front-facing.
    *
    * These were not read at all before, so every pipeline rendered both
    * windings of every triangle.  vkcube draws each cube face twice with
    * opposite winding and asks for VK_CULL_MODE_BACK_BIT, so the driver was
    * drawing all twelve triangles instead of six: each pair is coplanar, and
    * the two triangles fight along the diagonal they share, which is what put
    * a dashed seam across the middle of every face of the cube on screen.
    */
   bool cull_front;
   bool cull_back;
   bool front_cw;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_pipeline, base, VkPipeline,
                               VK_OBJECT_TYPE_PIPELINE)

#endif /* TA5_PIPELINE_H */