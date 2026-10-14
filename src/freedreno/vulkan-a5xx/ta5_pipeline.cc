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
 * \file ta5_pipeline.cc
 *
 * Adreno 5xx Vulkan driver: graphics pipelines.
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_math.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_shader_module.h"
#include "vk_graphics_state.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_shader.h"
#include "ta5_render.h"
#include "ta5_formats.h"
#include "ta5_pipeline.h"
#include "ta5_entrypoints.h"
#include "ta5_submit.h"

#include "drm/msm_drm.h"

#include "ir3/ir3_shader.h"

/**
 * Bytes one variant occupies in the staging buffer.
 *
 * IR3 lays a variant out as microcode at offset 0 followed by the stage's
 * constant data at info.constant_data_offset, so the variant is as large as
 * whichever of the two reaches further.
 */
static uint32_t
ta5_variant_size(const struct ir3_shader_variant *v)
{
   uint32_t microcode = v->info.sizedwords * 4;
   uint32_t constants = v->info.constant_data_offset + v->constant_data_size;

   return MAX2(microcode, constants);
}

/** Copy a variant's microcode and constant data into the staging buffer. */
static void
ta5_copy_variant(void *dst, const struct ta5_compiled_shader *shader,
                 const struct ir3_shader_variant *v)
{
   memcpy(dst, shader->code, v->info.sizedwords * 4);

   if (v->constant_data_size > 0)
      memcpy((char *)dst + v->info.constant_data_offset, v->constant_data,
             v->constant_data_size);
}

/**
 * Allocate and fill the object holding one stage's program.
 *
 * CPU-written, GPU-read, so it needs a WC mapping: this kernel's
 * GEM_CPU_PREP is a no-op ("TODO cache maintenance"), so a cached mapping
 * is never flushed and the shader fetch would read whatever stale DRAM
 * held.  WC bypasses the CPU cache, matching the ring buffer and freedreno,
 * which allocate WC by default.
 */
static VkResult
ta5_stage_bo_alloc(struct ta5_device *device,
                   const VkAllocationCallbacks *pAllocator,
                   struct ta5_pipeline *pipe,
                   const struct ir3_shader_variant *v,
                   struct ta5_compiled_shader *shader, struct ta5_bo **out)
{
   uint32_t size = ta5_variant_size(v);
   void *map;

   *out = ta5_bo_alloc(device, size ? size : 4, MSM_BO_WC);
   if (*out == NULL)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   map = ta5_bo_map(*out);
   if (map == NULL) {
      ta5_bo_unref(*out);
      *out = NULL;
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   ta5_copy_variant(map, shader, v);

   /*
    * Make those stores visible before the GPU is told to fetch them.
    *
    * The mapping is write-combining, which on ARM still drains through the
    * store buffer, and this kernel's GEM_CPU_PREP is a no-op ("TODO cache
    * maintenance"), so nothing else orders them.  The barrier is correct
    * regardless of the fragment fault -- it was added while chasing that and
    * did not change it -- but nothing should be relying on the stores being
    * ordered by luck.
    */
   __sync_synchronize();

   if (getenv("TA5_DUMP_SHADER_BO")) {
      const uint32_t *d = (const uint32_t *)map;
      fprintf(stderr,
              "ta5[shader-bo] size=%u sizedwords=%u instrlen=%u\n", size,
              v->info.sizedwords, v->instrlen);
      for (uint32_t i = 0; i < (size + 3) / 4; i++)
         fprintf(stderr, "ta5[shader-bo] %05x: %08x\n", i * 4, d[i]);
   }

   return VK_SUCCESS;
}

/** Copy the SPIR-V word stream out of a runtime shader module. */
static const uint32_t *
ta5_module_words(const struct vk_shader_module *module, size_t *out_size)
{
   *out_size = module->size;
   return (const uint32_t *)module->data;
}

static VkResult
ta5_pipeline_compile_stage(struct ta5_device *device,
                           VkShaderStageFlagBits stage,
                           const struct ta5_pipeline_layout *layout,
                           const struct vk_shader_module *module,
                           struct ta5_compiled_shader *out)
{
   size_t size;
   const uint32_t *words = ta5_module_words(module, &size);

   if (!words || size < 4 || size % 4 != 0)
      return VK_ERROR_INVALID_SHADER_NV;

   if (!ta5_shader_compile(device, stage, layout, words, size, out)) {
      if (getenv("TA5_TRACE_NULL"))
         fprintf(stderr,
                 "ta5[shader]: stage 0x%x failed to compile (%zu bytes SPIR-V)\n",
                 stage, size);
      return VK_ERROR_INVALID_SHADER_NV;
   }

   if (getenv("TA5_TRACE_NULL"))
      fprintf(stderr, "ta5[shader]: stage 0x%x -> %u dwords of microcode\n",
              stage, out->code_size);

   return VK_SUCCESS;
}

/**
 * Fill the runtime's vk_render_pass_state for one subpass.
 *
 * The runtime's graphics-state machinery owns this structure; the driver
 * only describes the subpass to it.  Turnip does exactly this before calling
 * vk_graphics_pipeline_state_fill().
 */
static void
ta5_fill_render_pass_state(struct vk_render_pass_state *rp,
                           const struct ta5_render_pass *pass)
{
   rp->color_attachment_count = 0;
   rp->depth_attachment_format = VK_FORMAT_UNDEFINED;
   rp->stencil_attachment_format = VK_FORMAT_UNDEFINED;
   rp->attachments = MESA_VK_RP_ATTACHMENT_INFO_INVALID;

   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      const struct ta5_render_pass_attachment *att = &pass->attachments[i];

      if (att->used_as_color) {
         if (rp->color_attachment_count < MESA_VK_MAX_COLOR_ATTACHMENTS) {
            uint8_t n = rp->color_attachment_count++;
            rp->color_attachment_formats[n] = (VkFormat)att->format;
            rp->color_attachment_samples[n] = (uint8_t)att->samples;
            rp->attachments |= MESA_VK_RP_ATTACHMENT_COLOR_BIT(n);
         }
      } else if (att->format == VK_FORMAT_S8_UINT) {
         rp->stencil_attachment_format = (VkFormat)att->format;
         rp->depth_stencil_attachment_samples = (uint8_t)att->samples;
         rp->attachments |= MESA_VK_RP_ATTACHMENT_STENCIL_BIT;
      } else {
         rp->depth_attachment_format = (VkFormat)att->format;
         if (att->format == VK_FORMAT_D24_UNORM_S8_UINT)
            rp->stencil_attachment_format = (VkFormat)att->format;
         rp->depth_stencil_attachment_samples = (uint8_t)att->samples;
         rp->attachments |= MESA_VK_RP_ATTACHMENT_DEPTH_BIT;
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateGraphicsPipelines(VkDevice _device, VkPipelineCache pipelineCache,
                            uint32_t createInfoCount,
                            const VkGraphicsPipelineCreateInfo *pCreateInfos,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   VkResult overall = VK_SUCCESS;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   for (uint32_t i = 0; i < createInfoCount; i++) {
      const VkGraphicsPipelineCreateInfo *ci = &pCreateInfos[i];
      struct ta5_pipeline *pipe;
      VkResult result;

      /* VK_EXT fail_on_pipeline_compile_required is not advertised, so the
       * caller must not pre-fill the handles. */
      if (pPipelines[i] != VK_NULL_HANDLE)
         return vk_error(&device->instance->vk, VK_ERROR_INITIALIZATION_FAILED);

      pipe = (struct ta5_pipeline *)vk_zalloc(pAllocator, sizeof(*pipe), 8,
                                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!pipe)
         return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

      vk_object_base_init(&device->vk, &pipe->base, VK_OBJECT_TYPE_PIPELINE);
      pipe->device = device;
      pipe->layout = ta5_pipeline_layout_from_handle(ci->layout);
      pipe->render_pass = ta5_render_pass_from_handle(ci->renderPass);
      pipe->subpass = ci->subpass;

      /*
       * Compile the stages the application supplied.  A missing stage is
       * legal in Vulkan and simply means that stage is disabled, so only
       * what was given is compiled.
       */
      if (ci->stageCount > 0 && ci->pStages[0].module) {
         struct ta5_compiled_shader vs;
         result = ta5_pipeline_compile_stage(
            device, VK_SHADER_STAGE_VERTEX_BIT, pipe->layout,
            (const struct vk_shader_module *)ci->pStages[0].module, &vs);
         if (result != VK_SUCCESS) {
            vk_free(pAllocator, pipe);
            return vk_error(&device->instance->vk, result);
         }
         pipe->vertex = vs;
         pipe->has_vertex = true;
      }

      if (ci->stageCount > 1 && ci->pStages[1].module) {
         struct ta5_compiled_shader fs;
         result = ta5_pipeline_compile_stage(
            device, VK_SHADER_STAGE_FRAGMENT_BIT, pipe->layout,
            (const struct vk_shader_module *)ci->pStages[1].module, &fs);
         if (result != VK_SUCCESS) {
            ta5_compiled_shader_free(&pipe->vertex);
            vk_free(pAllocator, pipe);
            return vk_error(&device->instance->vk, result);
         }
         pipe->fragment = fs;
         pipe->has_fragment = true;
      }

      /* Stage both shader variants into one buffer for indirect upload. */
      {
         /*
          * Copy each variant's *entire* BO, not just variant->bin.  IR3 lays a
          * variant out as
          *
          *     [ microcode @ 0 ][ constant data @ info.constant_data_offset ]
          *
          * and that constant data is what the shader's indexed `c[...]` reads
          * resolve against.  Copying only the microcode leaves those reads
          * pointing at whatever happened to be in the object beforehand, which
          * the UCHE reports as an out-of-bounds access when the draw runs.
          *
          * The shader fetch unit wants each variant 64-byte aligned, so the
          * fragment stage starts on the next 64-byte boundary -- and the
          * allocation has to include the gap.  Sizing the buffer from the
          * unaligned total puts the tail of the fragment variant past the end
          * of the object.
          */
         const struct ir3_shader_variant *vs_v =
            pipe->has_vertex
               ? (const struct ir3_shader_variant *)pipe->vertex.variant
               : NULL;
         const struct ir3_shader_variant *fs_v =
            pipe->has_fragment
               ? (const struct ir3_shader_variant *)pipe->fragment.variant
               : NULL;

         /*
          * One object per stage, the way IR3 hands out one BO per variant.
          *
          * Packing both programs into a single staging object at an offset
          * of our choosing reconstructs that layout rather than being it,
          * and every register the draw programs is now byte-identical to
          * what fd5 emits.  So if the fragment path is still unhappy, what
          * it is unhappy about has to be the object it is reading from.
          * Two objects puts each program at the base of its own allocation,
          * at an address the hardware picked, as in GL's stream.
          *
          * (Page-aligning the stages *inside one object* was tried first and
          * disproved: same faults, same IOVAs, same empty readback.  What
          * changes here is the object, not the offset within it.)
          */
         VkResult st;

         st = ta5_stage_bo_alloc(device, pAllocator, pipe, vs_v,
                                 &pipe->vertex, &pipe->vs_bo);
         if (st != VK_SUCCESS)
            return st;

         if (fs_v) {
            st = ta5_stage_bo_alloc(device, pAllocator, pipe, fs_v,
                                    &pipe->fragment, &pipe->fs_bo);
            if (st != VK_SUCCESS)
               return st;
         }
      }

      /*
       * Hand the runtime the subpass description it expects.  This is the
       * step the generic implementation could not do for us.
       */
      struct vk_render_pass_state rp_state = {};
      struct vk_multiview_state mv_state = {};
      ta5_fill_render_pass_state(&rp_state, pipe->render_pass);

      /*
       * mv_state is not optional, even though a single-view render pass needs
       * no multiview state.  vk_multiview_state_init() has no equivalent of
       * the driver_rp guard that vk_render_pass_state_init() has, so with
       * driver_mv == NULL it falls through to
       * vk_get_pipeline_rendering_create_info(), which does:
       *
       *    VK_FROM_HANDLE(vk_render_pass, render_pass, info->renderPass);
       *    return &render_pass->subpasses[info->subpass].pipeline_info;
       *
       * That reads a struct vk_render_pass out of what is really a
       * struct ta5_render_pass.  subpass_count happens to line up, the
       * subpasses pointer does not, and the result is a wild pointer that gets
       * dereferenced for viewMask.  Handing it a real (zeroed) state makes it
       * take the early return instead.  Turnip always passes one too.
       *
       * all_state must be a real object.  Passing NULL tells the runtime that
       * it may allocate, and it then writes the result through alloc_ptr_out --
       * which is NULL too, so it dereferences NULL (vk_graphics_state.c:1802).
       * Turnip passes &builder->all_state for the same reason.
       */
      vk_graphics_pipeline_state_fill(
         &device->vk, &pipe->graphics_state, ci, &mv_state, &rp_state, 0,
         &pipe->all_state, NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, NULL);

      /*
       * Depth and stencil state the draw path cannot honour.
       *
       * Checked here rather than at draw time so that the pipeline either
       * carries a request the hardware will carry out or does not exist.
       * Silently dropping a stencil test produces a picture that looks
       * plausible and is wrong, which is worse than a pipeline that fails.
       *
       * Depth itself is implemented (ta5_emit_depth_state); what is refused is
       * everything around it:
       *
       *   - a stencil test or write, because RB_STENCIL_CONTROL is written to
       *     0 on every draw and no stencil path exists yet
       *   - a depth attachment whose format has no hardware code
       *   - depthBoundsTest, which needs a second test the unit has no
       *     register for on this part
       *   - a pipeline that asks to test depth with no depth attachment, which
       *     the spec makes undefined rather than a no-op
       */
      {
         const VkPipelineDepthStencilStateCreateInfo *ds =
            ci->pDepthStencilState;
         const VkPipelineRasterizationStateCreateInfo *rs =
            ci->pRasterizationState;
         int didx = ta5_render_pass_depth_attachment(pipe->render_pass);

         if (ds && (ds->stencilTestEnable == VK_TRUE ||
                    ds->depthBoundsTestEnable == VK_TRUE)) {
            fprintf(stderr,
                    "ta5[pipe] stencil and depthBounds are not implemented "
                    "(stencilTestEnable=%u depthBoundsTestEnable=%u)\n",
                    ds->stencilTestEnable, ds->depthBoundsTestEnable);
            return vk_error(&device->instance->vk,
                            VK_ERROR_FEATURE_NOT_PRESENT);
         }

         if (didx >= 0 &&
             ta5_format_depth_hw(pipe->render_pass->attachments[didx].format)
                == 0) {
            fprintf(stderr,
                    "ta5[pipe] depth format %d has no hardware code\n",
                    pipe->render_pass->attachments[didx].format);
            return vk_error(&device->instance->vk,
                            VK_ERROR_FORMAT_NOT_SUPPORTED);
         }

         if (ds && ds->depthTestEnable == VK_TRUE && didx < 0) {
            fprintf(stderr,
                    "ta5[pipe] depthTestEnable with no depth attachment\n");
            return vk_error(&device->instance->vk,
                            VK_ERROR_FEATURE_NOT_PRESENT);
         }

         /*
          * Depth writes with no test are legal and common -- a prepass that
          * only fills z -- but they still need a depth attachment, and they are
          * the one case where Z_WRITE_ENABLE alone would silently discard
          * every fragment if the base register were left at zero.
          */
         if (ds && ds->depthWriteEnable == VK_TRUE && didx < 0) {
            fprintf(stderr,
                    "ta5[pipe] depthWriteEnable with no depth attachment\n");
            return vk_error(&device->instance->vk,
                            VK_ERROR_FEATURE_NOT_PRESENT);
         }

         /*
          * The rasteriser's depth clamp and the pipeline's are different
          * things; GRAS/RB depth clamp is emitted unconditionally as off in
          * ta5_emit_draw_state, so a pipeline that asks for it would get the
          * opposite of what it asked for.
          */
         if (rs && rs->depthClampEnable == VK_TRUE) {
            fprintf(stderr, "ta5[pipe] depthClampEnable is not implemented\n");
            return vk_error(&device->instance->vk,
                            VK_ERROR_FEATURE_NOT_PRESENT);
         }
      }

      /*
       * Fixed-function state is read from graphics_state at draw time, never
       * from the create info: those structures are borrowed for the duration of
       * this call only.  See the note on graphics_state in ta5_pipeline.h.
       */

      /*
       * Vertex input description and viewport state live in the command
       * buffer for Vulkan (they are not baked into the pipeline), so they are
       * recorded at draw time rather than captured here.
       *
       * pVertexInputState is optional and graphics_state.vi is NULL when the
       * application left it out, which is exactly what a shader that derives
       * everything from gl_VertexIndex does.  The guard is what keeps that
       * legal case from becoming a NULL dereference.
       */
      pipe->uses_vertex_buffers =
         pipe->graphics_state.vi && pipe->graphics_state.vi->bindings_valid;

      pPipelines[i] = ta5_pipeline_to_handle(pipe);
   }

   return overall;
}


VKAPI_ATTR void VKAPI_CALL
ta5_DestroyPipeline(VkDevice _device, VkPipeline _pipeline,
                    const VkAllocationCallbacks *pAllocator)
{
   struct ta5_pipeline *pipe = ta5_pipeline_from_handle(_pipeline);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   ta5_compiled_shader_free(&pipe->vertex);
   ta5_compiled_shader_free(&pipe->fragment);
   ta5_bo_unref(pipe->vs_bo);
   ta5_bo_unref(pipe->fs_bo);
   vk_free(pAllocator, pipe);
}