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
#include "compiler/list.h"   /* nir_foreach_function */
#include "vk_util.h"
#include "vk_object.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_nir.h"
#include "vk_shader_module.h"
#include "vk_common_entrypoints.h"
#include "ta5_descriptor.h"   /* ta5_tex_binding, TA5_MAX_TEX_UNITS */
#include "ta5_render.h"       /* ta5_pipeline_layout */

#include "compiler/shader_enums.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_intrinsics.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/isaspec/isaspec.h"

#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"

/*
 * ir3_shader_disasm() would be nicer, but it walks so->ir, which create_variant()
 * destroys before it returns -- calling it from here segfaults.  The raw ISA
 * decoder takes only the assembled binary, so use that.  Its declaration lives
 * in a generated header, so bind it by assembler name and take the options
 * struct from its real header.
 */
extern "C" void ta5_ir3_isa_disasm(void *bin, int sz, FILE *out,
                                    const struct isa_decode_options *options)
   __asm__("ir3_isa_disasm");

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

/* ------------------------------------------------------------------ */
/* Texture unit numbering                                              */
/* ------------------------------------------------------------------ */

struct ta5_tex_unit_ctx
{
   const struct ta5_pipeline_layout *layout;
   VkShaderStageFlags stage;
};

/** Is this one of the texture instructions whose unit we have to number? */
static bool
ta5_is_tex_unit_instr(const nir_instr *instr, const void *data)
{
   if (instr->type != nir_instr_type_tex)
      return false;

   switch (nir_instr_as_tex(instr)->op) {
   case nir_texop_tex:
   case nir_texop_txf:
   case nir_texop_txd:
   case nir_texop_txb:
   case nir_texop_txl:
   case nir_texop_txs:
      return true;
   default:
      return false;
   }
}

/**
 * Replace texture_deref / sampler_deref sources with a texel unit number.
 *
 * spirv_to_nir() describes a sampled image as a dereference of a descriptor
 * variable:
 *
 *     %t = (float32)tex %3 (texture_deref), %3 (sampler_deref), coord
 *
 * IR3 has no case for that source type and stops with "Unhandled NIR tex src
 * type: 13", which is nir_tex_src_texture_deref.  What IR3 does have is the
 * older form, where the unit is an immediate in tex->texture_index and
 * tex->sampler_index -- and that is exactly a5xx's bindful model and the one
 * this driver emits into, so nothing is given up by converting to it.
 *
 * Turningip avoids the same problem by rewriting these to texture_handle and
 * sampler_handle, which selects IR3's bindless path.  That path computes an
 * offset into a descriptor heap, and a5xx has no heap: the descriptors are
 * written straight into the command stream and addressed by unit number.
 *
 * The number is the binding's position in the order returned by
 * ta5_pipeline_layout_tex_bindings(), which is the same order the draw-time
 * emitter fills the slots in.  Returns NULL: the instruction is edited in
 * place and kept, not replaced.
 */
static nir_def *
ta5_lower_tex_units(nir_builder *b, nir_instr *instr, void *data)
{
   const struct ta5_tex_unit_ctx *ctx =
      (const struct ta5_tex_unit_ctx *)data;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   struct ta5_tex_binding order[TA5_MAX_TEX_UNITS * 2];
   unsigned num_order;

   if (!ta5_is_tex_unit_instr(instr, NULL))
      return NULL;

   num_order = ta5_pipeline_layout_tex_bindings(ctx->layout, ctx->stage, order,
                                                ARRAY_SIZE(order));

   if (num_order > TA5_MAX_TEX_UNITS) {
      TA5_DBG_SHADER("shader wants %u texel units, a5xx addresses %u",
                     num_order, TA5_MAX_TEX_UNITS);
      num_order = TA5_MAX_TEX_UNITS;
   }

   for (int i = 0; i < tex->num_srcs; i++) {
      bool is_texture = tex->src[i].src_type == nir_tex_src_texture_deref;
      bool is_sampler = tex->src[i].src_type == nir_tex_src_sampler_deref;
      const nir_deref_instr *deref;
      const nir_variable *var;
      uint32_t set, binding, unit;

      if (!is_texture && !is_sampler)
         continue;

      if (!nir_src_is_deref(tex->src[i].src))
         continue;

      deref = nir_src_as_deref(tex->src[i].src);
      var = nir_deref_instr_get_variable(deref);
      if (!var)
         continue;

      /*
       * The set number is data.descriptor_set, not data.location.
       *
       * vtn_variables.c:2528 writes the SPIR-V DescriptorSet decoration to
       * data.descriptor_set and leaves data.location at its -1 initialiser
       * (vtn_variables.c:2243), so reading location compares 0xFFFFFFFF
       * against the real set numbers in ta5_pipeline_layout_tex_bindings()
       * and never matches.  Every texture and sampler instruction therefore
       * came out as unit 0.
       *
       * That is invisible with a single descriptor, which is all the 88-case
       * matrix had, so it is the same "works by accident" shape as the R/B
       * transposition.  Confirmed from the trace before the fix:
       *
       *     ta5[texunit] set=4294967295 binding=0 op=0 unit=0
       *
       * and it is 0 for every binding and every set, not just this one.
       */
      set = var->data.descriptor_set;
      binding = var->data.binding;

      unit = 0;
      for (unsigned k = 0; k < num_order; k++) {
         if (order[k].set == set && order[k].binding == binding) {
            unit = k;
            break;
         }
      }

      /*
       * Texture units and sampler units are numbered independently, so a
       * combined image sampler takes the same number in both spaces.
       */
      if (is_texture)
         tex->texture_index = unit;
      else
         tex->sampler_index = unit;

      if (getenv("TA5_TRACE_TEXUNIT"))
         fprintf(stderr,
                 "ta5[texunit] set=%u binding=%u op=%d unit=%u (tex=%u samp=%u)\n",
                 set, binding, (int)tex->op, unit, tex->texture_index,
                 tex->sampler_index);
   }

   /* Drop the deref sources; the immediates just set replace them. */
   for (int i = tex->num_srcs - 1; i >= 0; i--) {
      if (tex->src[i].src_type == nir_tex_src_texture_deref ||
          tex->src[i].src_type == nir_tex_src_sampler_deref)
         nir_tex_instr_remove_src(tex, i);
   }

   return NULL;
}

/* ------------------------------------------------------------------ */
/* Buffer descriptors                                                  */
/* ------------------------------------------------------------------ */

struct ta5_ubo_ctx
{
   const struct ta5_pipeline_layout *layout;
   VkShaderStageFlags stage;
};

/**
 * Is this a buffer descriptor the UBO pointer block can address?
 *
 * The type here is a nir_descriptor_type, not a VkDescriptorType: the
 * intrinsic records the SPIR-V storage class it came from, and
 * VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC has no representation at all --
 * its dynamic offset is not known until the draw, and nothing here lowers
 * that into the shader.
 */
static bool
ta5_is_buffer_descriptor(nir_descriptor_type type)
{
   return type == nir_descriptor_type_uniform_buffer ||
          type == nir_descriptor_type_storage_buffer;
}

/** Which intrinsics the buffer-descriptor lowering has to look at. */
static bool
ta5_is_buffer_descriptor_instr(const nir_instr *instr, const void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   switch (nir_instr_as_intrinsic(instr)->intrinsic) {
   case nir_intrinsic_vulkan_resource_index:
   case nir_intrinsic_load_vulkan_descriptor:
      return true;
   default:
      return false;
   }
}

/** Block number for one descriptor, or UINT32_MAX if the layout has no slot. */
static uint32_t
ta5_ubo_block(struct ta5_ubo_ctx *ctx, uint32_t set, uint32_t binding,
              uint32_t element)
{
   struct ta5_buffer_binding order[TA5_MAX_UBOS];
   unsigned num = ta5_pipeline_layout_buffer_bindings(ctx->layout, ctx->stage,
                                                      order, ARRAY_SIZE(order));

   for (unsigned i = 0; i < num; i++) {
      if (order[i].set == set && order[i].binding == binding &&
          order[i].element == element)
         return i;
   }

   return UINT32_MAX;
}

/**
 * Replace vulkan_resource_index / load_vulkan_descriptor with a UBO block number.
 *
 * spirv_to_nir() describes a buffer descriptor as
 *
 *     %i = @vulkan_resource_index (desc_set=0, binding=0, desc_type=UBO)
 *     %d = @load_vulkan_descriptor (%i)
 *     %l = @load_deref (... %d ...)
 *
 * and IR3 has no case for either intrinsic ("Unhandled intrinsic type:
 * vulkan_resource_index", followed by a segfault in the error path because
 * the message is formatted with the NULL context).
 *
 * a5xx is bindful, so both collapse to one thing: the block number, which
 * indexes the per-stage UBO pointer array in the constant file.  That is what
 * ta5_emit_ubo_pointers() fills at draw time, and IR3's emit_intrinsic_load_ubo
 * turns a constant source into uniform(ubo_ptrs_base + block * ptrsz).
 *
 * The block number comes from ta5_pipeline_layout_buffer_bindings(), the same
 * list the draw-time emitter walks, because two halves that each sort their
 * own copy is how the two drift apart.
 */
static nir_def *
ta5_lower_buffer_descriptors(nir_builder *b, nir_instr *instr, void *data)
{
   struct ta5_ubo_ctx *ctx = (struct ta5_ubo_ctx *)data;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   switch (intrin->intrinsic) {
   case nir_intrinsic_vulkan_resource_index: {
      if (!ta5_is_buffer_descriptor(nir_intrinsic_desc_type(intrin)))
         return NULL;

      /* src[0] is the array index, for a binding with descriptor_count > 1. */
      uint32_t element = 0;
      if (nir_src_is_const(intrin->src[0]))
         element = nir_src_as_uint(intrin->src[0]);

      uint32_t block = ta5_ubo_block(ctx, nir_intrinsic_desc_set(intrin),
                                     nir_intrinsic_binding(intrin), element);
      if (block == UINT32_MAX) {
         TA5_DBG_SHADER("buffer descriptor set=%u binding=%u element=%u has "
                        "no slot in the pipeline layout",
                        nir_intrinsic_desc_set(intrin),
                        nir_intrinsic_binding(intrin), element);
         return NULL;
      }

      return nir_imm_int(b, block);
   }

   case nir_intrinsic_load_vulkan_descriptor:
      /*
       * The descriptor is the block number and nothing else on this
       * generation: there is no descriptor heap to offset into and no shift
       * to fold away, so the load is the index it was given.
       *
       * The replacement is returned rather than applied with
       * nir_def_replace(): nir_function_impl_lower_instructions() detaches
       * the instruction's use list before calling this and reattaches it
       * after, and replacing the def in here frees the very list it is
       * holding -- which is a crash inside the iterator, not a wrong answer.
       */
      if (!nir_src_is_const(intrin->src[0]))
         return NULL;
      return intrin->src[0].ssa;

   default:
      return NULL;
   }
}

/** Deref chain depth this lowering is willing to walk. */
#define TA5_MAX_DEREF_DEPTH 16

/**
 * Turn a load off a buffer descriptor's deref chain into load_ubo.
 *
 * After ta5_lower_buffer_descriptors() the descriptor is a plain block number,
 * and spirv_to_nir()'s deref chain still hangs off it:
 *
 *     %b = load_const (0x0)                        <- the block number
 *     %c = deref_cast (buf *)%b (ubo buf)
 *     %s = deref_struct &%c->attr (ubo vec4[36])
 *     %a = deref_array &(*%s)[%2] (ubo vec4)        <- %2 is gl_VertexIndex
 *     %l = @load_deref (%a)
 *
 * Nothing between the Vulkan front end and IR3 does this conversion: there is
 * no deref-to-load_ubo pass anywhere in this tree, and IR3's emit_instr() has
 * no case for a deref load ("Unhandled intrinsic type: load_deref", followed
 * by a segfault while formatting the message).
 *
 * So the chain is walked once, here, accumulating a constant part and a list
 * of scaled dynamic terms.  A dynamic array index is not a problem: load_ubo's
 * second source is a byte offset and IR3 adds it to the block address itself.
 *
 * Only loads are handled.  A store into a buffer descriptor is not lowered,
 * so a shader that has one still fails to compile rather than silently writing
 * nowhere.
 */
static nir_def *
ta5_lower_ubo_derefs(nir_builder *b, nir_instr *instr, void *data)
{
   const nir_deref_instr *chain[TA5_MAX_DEREF_DEPTH];
   unsigned depth = 0;
   const nir_deref_instr *deref;
   nir_def *block = NULL;
   nir_def *offset = NULL;
   uint32_t offset_const = 0;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic != nir_intrinsic_load_deref)
      return NULL;

   deref = nir_src_as_deref(intrin->src[0]);

   /* Leaf to root.  Bail on anything deeper than the walk can hold. */
   while (deref && depth < TA5_MAX_DEREF_DEPTH) {
      chain[depth++] = deref;
      deref = nir_deref_instr_parent(deref);
   }

   /* Root first: either a cast of the block number or a ubo variable. */
   deref = chain[depth - 1];

   switch (deref->deref_type) {
   case nir_deref_type_cast:
      /*
       * deref_cast is the buffer-pointer cast spirv_to_nir() emits over a
       * descriptor: its base is the block number, not another deref.
       */
      if (nir_def_is_deref(deref->parent.ssa))
         return NULL;
      block = deref->parent.ssa;
      break;

   case nir_deref_type_var:
      /*
       * A bare ubo variable.  Vulkan descriptors never arrive this way --
       * they come in as the cast above -- so there is no block number to
       * give it, and guessing one would read a neighbouring block.
       */
      return NULL;

   default:
      return NULL;
   }

   /* Root to leaf, accumulating the byte offset. */
   for (int i = (int)depth - 2; i >= 0; i--) {
      const nir_deref_instr *d = chain[i];
      const struct glsl_type *parent_type =
         nir_deref_instr_parent(d)->type;

      switch (d->deref_type) {
      case nir_deref_type_struct:
         offset_const += glsl_get_struct_field_offset(parent_type,
                                                       d->strct.index);
         break;

      case nir_deref_type_array: {
         unsigned stride = nir_deref_instr_array_stride((nir_deref_instr *)d);

         if (!stride)
            return NULL;

         if (nir_src_is_const(d->arr.index)) {
            offset_const += nir_src_as_uint(d->arr.index) * stride;
         } else {
            /* offset = offset + index * stride */
            if (!offset)
               offset = nir_imm_int(b, 0);
            offset = nir_iadd(b, offset,
                              nir_imul_imm(b, d->arr.index.ssa, stride));
         }
         break;
      }

      case nir_deref_type_ptr_as_array:
         /*
          * A pointer used as an array, as variable pointers are spelled.
          * The stride is the cast's, which is the one the layout gave it.
          */
         if (!d->cast.ptr_stride || !nir_src_is_const(d->arr.index))
            return NULL;
         offset_const += nir_src_as_uint(d->arr.index) * d->cast.ptr_stride;
         break;

      case nir_deref_type_cast:
         /* The root cast is already accounted for; an inner one is a
          * reinterpret and moves no bytes. */
         break;

      default:
         return NULL;
      }
   }

   if (!offset)
      offset = nir_imm_int(b, offset_const);
   else if (offset_const)
      offset = nir_iadd(b, offset, nir_imm_int(b, offset_const));

   /*
    * The indices are set after the build rather than through a designated
    * initializer: a call that mixes positional and designated arguments is a
    * C99 extension, and this is C++.
    *
    * range = ~0 is not an oversight.  get_ubo_load_range() treats a known
    * range as permission to read the block at compile time, which is only
    * true for GL; see ta5_defuse_ubo_ranges() for the other half of this.
    */
   nir_def *load = nir_load_ubo(b, intrin->num_components,
                                intrin->def.bit_size, block, offset);
   nir_intrinsic_instr *load_intr = nir_def_as_intrinsic(load);

   nir_intrinsic_set_access(load_intr, nir_intrinsic_access(intrin));
   nir_intrinsic_set_range(load_intr, ~0u);
   return load;
}

/** Which instructions the buffer-deref lowering looks at. */
static bool
ta5_is_ubo_deref_instr(const nir_instr *instr, const void *data)
{
   return instr->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_deref;
}

/**
 * Stop IR3 from freezing a Vulkan UBO load into the constant file.
 *
 * IR3's UBO-to-constant optimisation ("ubo ranges") reads a block's contents
 * at compile time and bakes them into the shader's constant data.  That is
 * right for GL, where a uniform block is part of the shader, and wrong for
 * Vulkan, where the same block is whatever buffer the draw has bound -- and
 * where the compiler has no constant data to bake from at all.
 *
 * The optimisation is keyed off load_ubo's declared range, but only for a
 * dynamically indexed access: get_ubo_load_range() recomputes a static
 * access's range from its component count, ignoring what the range says.  So
 * a second, provably-zero term is added to the offset instead.
 *
 * Zero has to be built out of the block number rather than written as a
 * constant, or the constant folder collapses the whole thing back into a
 * constant offset and the range is recomputed anyway.  `(block << 31) & 1` is
 * 0 for every 32-bit input -- the shift can only set bit 31, and the mask
 * keeps only bit 0 -- and neither half of it is a fold: `(block << 31) << 31`
 * was the first attempt and *is* folded, because shift-of-shift adds.
 */
static void
ta5_defuse_ubo_ranges(nir_function_impl *impl)
{
   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

         if (intrin->intrinsic != nir_intrinsic_load_ubo ||
             !nir_src_is_const(intrin->src[0]) ||
             !nir_src_is_const(intrin->src[1]))
            continue;

         nir_builder b = nir_builder_at(nir_before_instr(instr));
         nir_def *always_zero =
            nir_iand(&b, nir_ishl_imm(&b, intrin->src[0].ssa, 31),
                     nir_imm_int(&b, 1));

         /*
          * nir_src_rewrite(), not an assignment: the source is already in the
          * old def's use list, and overwriting it without unlinking leaves the
          * list pointing at an instruction that no longer references it.  That
          * corruption surfaces much later -- measured as a crash inside
          * nir_opt_cse's nir_def_rewrite_uses(), well past this pass.
          */
         nir_src_rewrite(&intrin->src[1],
                         nir_iadd(&b, intrin->src[1].ssa, always_zero));
      }
   }
}

/* ------------------------------------------------------------------ */
/* Reading back an output                                              */
/* ------------------------------------------------------------------ */

/** One remembered output write: (location, array element) -> value. */
struct ta5_out_store
{
   uint32_t location;
   uint32_t component;
   nir_def *value;
};

#define TA5_MAX_TRACKED_STORES 32

/**
 * Replace a read of an output variable with the value written to it.
 *
 * SPIR-V allows a shader to read an Output variable it has already written,
 * and vkcube's vertex shader does: it stores gl_Position and then writes a
 * `frag_pos` varying from it.  NIR spells that read as
 *
 *     %p = @store_output (%v, location=VARYING_SLOT_POS, wrmask=xyzw)
 *     %g = @load_output (location=VARYING_SLOT_POS, component=0)
 *
 * and IR3 has no case for load_output -- "Unhandled intrinsic type:
 * load_output", then a segfault while it formats the message.  A store_output
 * is fine, because writing an output is what outputs are for; it is only the
 * read-back that has no instruction.
 *
 * So the writes are remembered per block and a read becomes the value that
 * was written.  The table is cleared at every block boundary and only stores
 * that precede the read in the same block are visible, so a read that no
 * visible write answers is left alone rather than answered with a value from
 * somewhere else.
 */
static bool
ta5_lower_output_reads(nir_function_impl *impl)
{
   bool progress = false;

   nir_foreach_block (block, impl) {
      struct ta5_out_store stores[TA5_MAX_TRACKED_STORES];
      uint32_t num_stores = 0;

      nir_foreach_instr_safe (instr, block) {
         nir_builder b;

         if (instr->type != nir_instr_type_intrinsic)
            continue;

         b = nir_builder_at(nir_before_instr(instr));

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         nir_io_semantics io;

         if (intrin->intrinsic != nir_intrinsic_store_output &&
             intrin->intrinsic != nir_intrinsic_load_output)
            continue;

         io = nir_intrinsic_io_semantics(intrin);

         if (intrin->intrinsic == nir_intrinsic_store_output) {
            unsigned wrmask = nir_intrinsic_write_mask(intrin);
            unsigned base = nir_intrinsic_component(intrin);
            nir_def *value = intrin->src[0].ssa;

            /* A partial write is recorded per component, so a later
             * single-component read finds the right piece of it. */
            for (unsigned i = 0; i < 4; i++) {
               if (!(wrmask & (1u << i)) || num_stores >= TA5_MAX_TRACKED_STORES)
                  continue;

               stores[num_stores].location = io.location;
               stores[num_stores].component = base + i;
               stores[num_stores].value =
                  (value->num_components == 4 && i < 4)
                     ? nir_channel(&b, value, i)
                     : value;
               num_stores++;
            }
            continue;
         }

         /* load_output */
         unsigned comp = nir_intrinsic_component(intrin);
         unsigned ncomp = intrin->def.num_components;
         nir_def *found[4] = {0};

         if (ncomp > ARRAY_SIZE(found))
            continue;

         for (unsigned i = 0; i < ncomp; i++) {
            for (uint32_t j = 0; j < num_stores; j++) {
               if (stores[j].location == io.location &&
                   stores[j].component == comp + i) {
                  found[i] = stores[j].value;
                  break;
               }
            }
            if (!found[i])
               break;
         }

         if (!found[0])
            continue;

         /* Every component has to be there, or the read stays. */
         for (unsigned i = 0; i < ncomp; i++) {
            if (!found[i])
               break;
         }

         if (ncomp == 1) {
            nir_def_replace(&intrin->def,
                            found[0]->num_components == 1
                               ? found[0]
                               : nir_channel(&b, found[0], 0));
            progress = true;
         } else if (ncomp == 4) {
            bool scalar = true;

            for (unsigned i = 0; i < 4; i++)
               scalar = scalar && found[i]->num_components == 1;

            if (scalar) {
               nir_def_replace(&intrin->def,
                               nir_vec4(&b, found[0], found[1], found[2],
                                        found[3]));
               progress = true;
            }
         }
      }
   }

   return progress;
}

bool
ta5_shader_compile(struct ta5_device *device, VkShaderStageFlagBits stage,
                   const struct ta5_pipeline_layout *layout,
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

   /*
    * Number the texture units before ir3_finalize_nir(), which is where the
    * texture lowering runs; the instruction has to be in the immediate form
    * by the time IR3 sees it.
    */
   {
      struct ta5_tex_unit_ctx ctx = {
         .layout = layout,
         .stage = (stage == VK_SHADER_STAGE_VERTEX_BIT)
                      ? VK_SHADER_STAGE_VERTEX_BIT
                      : VK_SHADER_STAGE_FRAGMENT_BIT,
      };

      if (ctx.stage)
         nir_shader_lower_instructions(nir, ta5_is_tex_unit_instr,
                                       ta5_lower_tex_units, &ctx);
   }

   /*
    * Buffer descriptors next, and before ir3_finalize_nir() for the same
    * reason: the intrinsics have to be gone by the time IR3 reads the shader.
    */
   {
      struct ta5_ubo_ctx ctx = {
         .layout = layout,
         .stage = (stage == VK_SHADER_STAGE_VERTEX_BIT)
                      ? VK_SHADER_STAGE_VERTEX_BIT
                      : VK_SHADER_STAGE_FRAGMENT_BIT,
      };

      nir_shader_lower_instructions(nir, ta5_is_buffer_descriptor_instr,
                                    ta5_lower_buffer_descriptors, &ctx);

      /* Then the deref chain hanging off it, then the range defusal. */
      nir_shader_lower_instructions(nir, ta5_is_ubo_deref_instr,
                                    ta5_lower_ubo_derefs, &ctx);

      nir_foreach_function (fn, nir) {
         if (fn->impl) {
            ta5_defuse_ubo_ranges(fn->impl);
            ta5_lower_output_reads(fn->impl);
         }
      }
   }

   ir3_finalize_nir(compiler, &shader_nir_options, nir);
   /*
    * IR3's own disassembly, on a knob.  This is how the compiled texture
    * instruction gets read back: the descriptor and the sampler are correct
    * and still not enough, because the unit the shader indexes with lives in
    * the instruction and nowhere else.
    */
   if (getenv("TA5_TRACE_SHADER"))
      fprintf(stderr, "--- ta5: NIR after lowering ---\n"), nir_log_shaderi(nir);

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
    * IR3 machine code, on a knob.  Disassemble the assembled binary rather
    * than the IR: create_variant() destroys the IR before it returns, so the
    * instruction stream is the only thing still alive here.  This is how the
    * compiled tex instruction gets read back -- the descriptor and the sampler
    * can both be correct and still not be enough, because the unit the shader
    * indexes with lives in the instruction and nowhere else.
    */
   if (getenv("TA5_TRACE_DISASM")) {
      struct isa_decode_options opts = {};
      opts.gpu_id = (uint32_t)compiler->gen * 100;
      opts.show_errors = true;
      opts.branch_labels = true;
      opts.max_errors = 4;
      fprintf(stderr, "--- ta5: IR3 for %s shader ---\n",
              variant->type == MESA_SHADER_VERTEX ? "vertex" : "fragment");
      ta5_ir3_isa_disasm(variant->bin, variant->info.sizedwords * 4, stderr,
                         &opts);
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

   /*
    * Hand the variant itself to the caller rather than destroying it here.
    * ir3_link_shaders() and ir3_find_*_regid() run at record time, not
    * compile time, and they read the variant.
    */
   out->variant = variant;
   out->shader = ir3_shader;
   out->nir_ctx = mem_ctx;
   ir3_shader = NULL;
   mem_ctx = NULL;
   ok = true;

fail:
   ralloc_free(mem_ctx);
   return ok;
}

void
ta5_compiled_shader_free(struct ta5_compiled_shader *shader)
{
   if (shader->shader)
      ir3_shader_destroy((struct ir3_shader *)shader->shader);
   shader->shader = NULL;
   shader->variant = NULL;
   ralloc_free(shader->nir_ctx);
   shader->nir_ctx = NULL;
   free(shader->code);
   shader->code = NULL;
   shader->code_size = 0;
}