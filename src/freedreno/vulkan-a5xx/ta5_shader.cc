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
 * \file ta5_shader.cc
 *
 * Adreno 5xx Vulkan driver: shader modules and IR3 compilation.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_math.h"
#include "util/u_atomic.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_nir.h"
#include "vk_shader_module.h"
#include "vk_common_entrypoints.h"

#include "compiler/shader_enums.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_intrinsics.h"
#include "compiler/spirv/nir_spirv.h"

#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_shader.h"
#include "ta5_entrypoints.h"
#include "vk_nir.h"


#define TA5_DBG_SHADER(fmt, ...)                                          \
   do {                                                                   \
      if (getenv("TA5_TRACE_NULL"))                                       \
         fprintf(stderr, "ta5[shader]: " fmt "\n", ##__VA_ARGS__);        \
   } while (0)

/* ------------------------------------------------------------------ */
/* Shader modules                                                      */
/* ------------------------------------------------------------------ */

/*
 * The Vulkan runtime already provides generic implementations of these: they
 * store the SPIR-V word stream, compute a blake3 identifier and track
 * allocation.  They must still be defined here -- the generated dispatch table
 * references ta5_<Name> by symbol name and the runtime does not alias them,
 * so an undefined symbol becomes a NULL slot the loader calls into.
 *
 * Compilation happens at pipeline creation, not here, because a module is
 * stage-agnostic and a pipeline knows what it is building.
 */
VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateShaderModule(VkDevice _device,
                       const VkShaderModuleCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkShaderModule *pShaderModule)
{
   return vk_common_CreateShaderModule(_device, pCreateInfo, pAllocator,
                                       pShaderModule);
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyShaderModule(VkDevice _device, VkShaderModule _shaderModule,
                        const VkAllocationCallbacks *pAllocator)
{
   struct vk_shader_module *module =
      (struct vk_shader_module *)_shaderModule;

   if (!module)
      return;

   vk_common_DestroyShaderModule(_device, _shaderModule, pAllocator);
}

/* ------------------------------------------------------------------ */
/* Compilation                                                         */
/* ------------------------------------------------------------------ */

static mesa_shader_stage
ta5_shader_stage(VkShaderStageFlagBits stage)
{
   switch (stage) {
   case VK_SHADER_STAGE_VERTEX_BIT:
      return MESA_SHADER_VERTEX;
   case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      return MESA_SHADER_TESS_CTRL;
   case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
      return MESA_SHADER_TESS_EVAL;
   case VK_SHADER_STAGE_GEOMETRY_BIT:
      return MESA_SHADER_GEOMETRY;
   case VK_SHADER_STAGE_FRAGMENT_BIT:
      return MESA_SHADER_FRAGMENT;
   case VK_SHADER_STAGE_COMPUTE_BIT:
      return MESA_SHADER_COMPUTE;
   default:
      return MESA_SHADER_NONE;
   }
}

/*
 * Assign consecutive driver locations to the interface variables.
 *
 * The GL backend does this per shader kind in freedreno_program.c, and the
 * I/O lowering below only reads var->data.driver_location -- it never assigns
 * one.  Without this, every store_output gets base = 0 and IR3's
 * setup_output() indexes ctx->outputs[] out of bounds, tripping
 * "failed assert: idx < ctx->noutputs" (ctx->noutputs is nir->num_outputs * 4).
 *
 * Assigning them densely also renumbers the Gallium-style slots that
 * spirv_to_nir() produces (VARYING_SLOT_POS, VARYING_SLOT_VAR0, ...) into the
 * plain 0-based locations a Vulkan interface expects.
 */
static void
ta5_assign_driver_locations(nir_shader *nir)
{
   nir->num_inputs = 0;
   nir->num_outputs = 0;

   /* gl_Position has to stay first: IR3 relies on driver_location 0 being the
    * position output. */
   unsigned out_n = 0;
   nir_foreach_shader_out_variable(var, nir) {
      if (var->data.location == VARYING_SLOT_POS)
         continue;
      var->data.driver_location = ++out_n;
   }
   nir->num_outputs = out_n + 1;

   unsigned in_n = 0;
   nir_foreach_shader_in_variable(var, nir)
      var->data.driver_location = in_n++;
   nir->num_inputs = in_n;
}

/*
 * spirv_to_nir()'s own logging is compiled out of NDEBUG builds, so failures
 * come back as a bare NULL with nothing to explain it.  Install a callback so
 * a rejected module says why.
 */
static void
ta5_spirv_debug(void *private_data, enum nir_spirv_debug_level level,
                 size_t spirv_offset, const char *message)
{
   static const char *names[] = {"error", "warning", "info"};
   fprintf(stderr, "ta5[spirv] %s @word %zu: %s\n",
           names[level < 3 ? level : 2], spirv_offset / 4, message);
}

bool
ta5_shader_compile(struct ta5_device *device, VkShaderStageFlagBits stage,
                   const uint32_t *code, size_t code_size,
                   struct ta5_compiled_shader *out)
{
   struct ir3_compiler *compiler = device->compiler;
   const nir_shader_compiler_options *nir_options;
   struct spirv_to_nir_options spirv_options = {};
   struct ir3_shader_options shader_options = {};
   struct ir3_shader_nir_options shader_nir_options = {};
   struct ir3_shader *ir3_shader;
   struct ir3_shader_variant *variant;
   struct ir3_shader_key key = {};
   nir_shader *nir;
   void *mem_ctx;
   bool ok = false;

   memset(out, 0, sizeof(*out));

   if (!compiler) {
      TA5_DBG_SHADER("no ir3 compiler on device");
      return false;
   }

   mem_ctx = ralloc_context(NULL);
   nir_options = ir3_get_compiler_options(compiler);

   /*
    * A5xx descriptors are bindful, so unlike Turnip there is no bindless
    * descriptor lowering to do here -- IR3's generation-gated a4xx back end
    * already targets this part's layout.
    */
   /*
    * Not a library.  Turnip gets away with create_library = true only because
    * its graphics path then runs tu_nir_lower_vs over a clone; the library
    * form keeps load/store deref intrinsics that IR3's emit_intrinsic has no
    * case for ("Unhandled intrinsic type").  Entry-point NIR is what we want.
    */
   spirv_options.create_library = false;

   if (getenv("TA5_TRACE_NULL")) {
      spirv_options.debug.func = ta5_spirv_debug;
      spirv_options.debug.private_data = device;
   }

   /*
    * Go through the runtime's wrapper rather than calling spirv_to_nir()
    * directly.  It does three things we were each getting wrong:
    *
    *  - it divides code_size by 4, because spirv_to_nir() takes a word count.
    *    Passing bytes made the parser read four times past the end of the
    *    module and report a bare "SPIR-V parsing FAILED".
    *
    *  - it runs the deref lowering (inline, copy-prop, then nir_opt_deref).
    *    Doing it ourselves straight after nir_lower_system_values left
    *    load_deref/store_deref in the IR, which IR3's emit_intrinsic() has no
    *    case for -- "Unhandled intrinsic type: store_deref", followed by a
    *    segfault inside the error path, since ir3_get_src_maybe_shared() is
    *    handed the NULL context.
    *
    *  - it supplies the device's descriptor sizes and SPIR-V capabilities.
    */
   nir = vk_spirv_to_nir(&device->vk, code, code_size,
                         ta5_shader_stage(stage), "main", NULL, &spirv_options,
                         nir_options, false /* internal */, mem_ctx);
   if (!nir) {
      TA5_DBG_SHADER("spirv_to_nir failed for stage %u", stage);
      goto fail;
   }

   /*
    * a5xx runs 64-lane waves and cannot subdivide them, so only the single
    * (full) wavesize is API-visible.  Vulkan has no subGroupSize control
    * here anyway, so this is not a capability loss.
    */
   /*
    * Lower system values (gl_VertexIndex, gl_InstanceIndex, ...) before IR3
    * sees them.  A vertex shader that reads gl_VertexIndex -- which is how a
    * triangle with no vertex buffer is generated -- fails to compile without
    * this.  Turnip does the same at tu_shader.cc:76.
    */
   NIR_PASS(_, nir, nir_lower_system_values);

   /*
    * Lower the I/O derefs that spirv_to_nir() produced into load_input /
    * store_output intrinsics.
    *
    * spirv_to_nir() writes an output varying as
    *
    *    %1 = deref_var &v_color (shader_out vec3)
    *        @store_deref (%1, %2) (wrmask=xyz)
    *
    * and IR3 cannot read that form: emit_instr() has no case for
    * nir_instr_type_load / nir_instr_type_store, and emit_intrinsic() falls
    * through to "Unhandled intrinsic type: store_deref" -- then segfaults
    * while formatting the message, because ir3_get_src_maybe_shared() is
    * handed the NULL context.  IR3 does handle load_input and store_output.
    *
    * ir3_finalize_nir() does not do this for us.  Its only call to
    * ir3_nir_lower_io() (which is what runs nir_lower_io) is gated on
    * MESA_SHADER_GEOMETRY, because the GL backend already lowered I/O before
    * calling into IR3 at ir3_gallium.c:499.  Arriving from Vulkan, nothing has
    * run it, so run the same pair the GL path uses.
    */
   if (!nir->info.io_lowered) {
      ta5_assign_driver_locations(nir);
      ir3_nir_lower_io(nir);
   }

   shader_options.api_wavesize = IR3_SINGLE_ONLY;
   shader_options.real_wavesize = IR3_SINGLE_ONLY;

   /*
    * No robustness2 extensions are advertised, so no access is guaranteed
    * bounds-checked and every mode stays conservative.
    */
   shader_nir_options.robust_modes = (nir_variable_mode)0;
   shader_options.nir_options = shader_nir_options;

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   ir3_finalize_nir(compiler, &shader_nir_options, nir);
   ir3_shader = ir3_shader_from_nir(compiler, nir, &shader_options);
   if (!ir3_shader) {
      TA5_DBG_SHADER("ir3_shader_from_nir failed");
      goto fail;
   }

   variant = ir3_shader_create_variant(ir3_shader, &key, /*keep_ir=*/false);
   if (!variant || !variant->bin) {
      TA5_DBG_SHADER("ir3 variant compile failed");
      ir3_shader_destroy(ir3_shader);
      goto fail;
   }

   /*
    * The assembled microcode lives in the variant; copy it out because
    * ir3_shader_destroy() below frees it.
    */
   out->code_size = variant->info.sizedwords;
   out->instrlen = variant->instrlen;
   out->code = (uint32_t *)malloc(out->code_size * sizeof(uint32_t));
   if (!out->code) {
      ir3_shader_destroy(ir3_shader);
      goto fail;
   }
   memcpy(out->code, variant->bin, out->code_size * sizeof(uint32_t));
   out->constlen = variant->constlen;

   ir3_shader_destroy(ir3_shader);
   ok = true;

fail:
   ralloc_free(mem_ctx);
   return ok;
}

void
ta5_compiled_shader_free(struct ta5_compiled_shader *shader)
{
   free(shader->code);
   shader->code = NULL;
   shader->code_size = 0;
}