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
#include "ta5_pipeline.h"
#include "ta5_entrypoints.h"

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
                           const struct vk_shader_module *module,
                           struct ta5_compiled_shader *out)
{
   size_t size;
   const uint32_t *words = ta5_module_words(module, &size);

   if (!words || size < 4 || size % 4 != 0)
      return VK_ERROR_INVALID_SHADER_NV;

   if (!ta5_shader_compile(device, stage, words, size, out)) {
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
            device, VK_SHADER_STAGE_VERTEX_BIT,
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
            device, VK_SHADER_STAGE_FRAGMENT_BIT,
            (const struct vk_shader_module *)ci->pStages[1].module, &fs);
         if (result != VK_SUCCESS) {
            ta5_compiled_shader_free(&pipe->vertex);
            vk_free(pAllocator, pipe);
            return vk_error(&device->instance->vk, result);
         }
         pipe->fragment = fs;
         pipe->has_fragment = true;
      }

      /*
       * Hand the runtime the subpass description it expects.  This is the
       * step the generic implementation could not do for us.
       */
      struct vk_render_pass_state rp_state = {};
      ta5_fill_render_pass_state(&rp_state, pipe->render_pass);
      vk_graphics_pipeline_state_fill(&device->vk, &pipe->graphics_state, ci,
                                      NULL, &rp_state, 0, NULL, NULL,
                                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, NULL);

      pipe->raster = ci->pRasterizationState;
      pipe->blend = ci->pColorBlendState;
      pipe->depth_stencil = ci->pDepthStencilState;
      pipe->multisample = ci->pMultisampleState;

      /*
       * Vertex input description and viewport state live in the command
       * buffer for Vulkan (they are not baked into the pipeline), so they are
       * recorded at draw time rather than captured here.
       */
      pipe->uses_vertex_buffers = ci->pVertexInputState->vertexBindingDescriptionCount > 0;

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
   vk_free(pAllocator, pipe);
}