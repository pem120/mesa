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
 * \file ta5_shader.h
 *
 * Adreno 5xx Vulkan driver: shader modules and IR3 compilation.
 */

#ifndef TA5_SHADER_H
#define TA5_SHADER_H

#include <stdint.h>

#include "vk_object.h"
#include "vk_device.h"

#include "ta5_instance.h"

/**
 * Compiled shader stage.
 *
 * a5xx is generation 5, so IR3 selects its a4xx back end
 * (ir3_context.c: gen >= 6 -> a6xx_funcs, gen >= 4 -> a4xx_funcs).  That is
 * the same compiler the GL backend uses on this part, and it emits
 * CP_LOAD_STATE4 -- which is the only state-load family a5xx has.
 */
struct ta5_compiled_shader
{
   void *nir;          /**< nir_shader*, kept for diagnostics */
   /**
    * The compiled variant, owned by this object.
    *
    * CP_LOAD_STATE4 only needs the microcode, but almost all of the program
    * state the hardware wants alongside it -- HLSQ config, register
    * footprints, the register ids the vertex fetch unit reads -- lives on the
    * variant, so it has to outlive compilation.  See
    * ta5_emit_program_state().
    */
   void *variant;      /**< struct ir3_shader_variant * */
   void *shader;       /**< struct ir3_shader *, owns variant */
   /**
    * ralloc context that owns the NIR handed to ir3_shader_from_nir().
    *
    * Kept alive alongside the shader: ir3_shader_destroy() frees
    * shader->nir, and that NIR is a child of this context, so the context
    * has to outlive the shader or the destroy double-frees it.
    */
   void *nir_ctx;
   uint32_t *code;     /**< the assembled PM4 microcode */
   uint32_t code_size; /**< in dwords */
   uint32_t instrlen;  /**< dwords, for CP_LOAD_STATE4_0_NUM_UNIT */
   uint32_t constlen;  /**< constant bank size in dwords */
};

struct ta5_shader_module
{
   struct vk_object_base base;
   struct ta5_device *device;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_shader_module, base, VkShaderModule,
                               VK_OBJECT_TYPE_SHADER_MODULE)

/**
 * Compile one SPIR-V module for one stage.
 *
 * Returns false if the shader does not compile; the caller maps that to
 * VK_ERROR_INVALID_SHADER_NV / VK_ERROR compilation failure at pipeline
 * creation.
 */
bool ta5_shader_compile(struct ta5_device *device, VkShaderStageFlagBits stage,
                        const struct ta5_pipeline_layout *layout,
                        const uint32_t *code, size_t code_size,
                        struct ta5_compiled_shader *out);

void ta5_compiled_shader_free(struct ta5_compiled_shader *shader);

#endif /* TA5_SHADER_H */