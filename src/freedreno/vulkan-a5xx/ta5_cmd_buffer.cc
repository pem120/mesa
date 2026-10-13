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
 * \file ta5_cmd_buffer.cc
 *
 * Adreno 5xx Vulkan driver: command pools and command buffers.
 *
 * PM4 is a flat word stream, so recording is appending 32-bit words to a
 * growable buffer.  Bin and render state are gathered separately and emitted
 * at flush time -- that split is a5xx specific and is why this is not shared
 * with Turnip (see A5XX_VULKAN_SPEC.md, bin/render split at cmdbuf+0x2f8).
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
#include "vk_command_buffer.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_sync.h"
#include "ta5_cmd_buffer.h"
#include "ta5_entrypoints.h"

/** Initial command stream capacity, in 32-bit words. */
#define TA5_CS_INITIAL_WORDS 1024

/* ------------------------------------------------------------------ */
/* Command pools                                                       */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateCommandPool(VkDevice _device,
                      const VkCommandPoolCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkCommandPool *pCommandPool)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_cmd_pool *pool;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   pool = (struct ta5_cmd_pool *)vk_zalloc(pAllocator, sizeof(*pool), 8,
                                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pool->base,
                       VK_OBJECT_TYPE_COMMAND_POOL);
   pool->device = device;
   pool->queue_family_index = pCreateInfo->queueFamilyIndex;
   pool->flags = pCreateInfo->flags;

   *pCommandPool = ta5_cmd_pool_to_handle(pool);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyCommandPool(VkDevice _device, VkCommandPool _commandPool,
                       const VkAllocationCallbacks *pAllocator)
{
   struct ta5_cmd_pool *pool = ta5_cmd_pool_from_handle(_commandPool);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   /* Command buffers allocated from this pool must already be freed. */
   vk_free(pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_ResetCommandPool(VkDevice _device, VkCommandPool _commandPool,
                     VkCommandPoolResetFlags flags)
{
   /*
    * This driver keeps no per-pool command buffer list, so there is nothing
    * to release: every command buffer is owned by the application until it
    * is freed, and a reset pool simply allows new allocations.
    */
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_TrimCommandPool(VkDevice _device, VkCommandPool _commandPool,
                    VkCommandPoolTrimFlags flags)
{
}

/* ------------------------------------------------------------------ */
/* Command buffers                                                     */
/* ------------------------------------------------------------------ */

static VkResult
ta5_cmd_buffer_alloc_cs(struct ta5_cmd_buffer *cmd, uint32_t words)
{
   void *mem;

   if (words <= cmd->cs_size)
      return VK_SUCCESS;

   mem = vk_realloc(&cmd->device->vk.alloc, cmd->cs,
                    words * sizeof(uint32_t), 4,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!mem)
      return vk_error(&cmd->device->instance->vk,
                      VK_ERROR_OUT_OF_HOST_MEMORY);

   cmd->cs = (uint32_t *)mem;
   cmd->cs_size = words;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_AllocateCommandBuffers(
   VkDevice _device, const VkCommandBufferAllocateInfo *pAllocateInfo,
   VkCommandBuffer *pCommandBuffers)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_cmd_pool *pool =
      ta5_cmd_pool_from_handle(pAllocateInfo->commandPool);
   VkResult result;

   for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      struct ta5_cmd_buffer *cmd =
         (struct ta5_cmd_buffer *)vk_zalloc(&device->vk.alloc, sizeof(*cmd), 8,
                                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!cmd)
         return vk_error(&device->instance->vk,
                         VK_ERROR_OUT_OF_HOST_MEMORY);

      vk_object_base_init(&device->vk, &cmd->base,
                          VK_OBJECT_TYPE_COMMAND_BUFFER);
      cmd->device = device;
      cmd->pool = pool;
      cmd->state = MESA_VK_COMMAND_BUFFER_STATE_INITIAL;

      result = ta5_cmd_buffer_alloc_cs(cmd, TA5_CS_INITIAL_WORDS);
      if (result != VK_SUCCESS) {
         vk_free(&device->vk.alloc, cmd);
         return result;
      }

      pCommandBuffers[i] = ta5_cmd_buffer_to_handle(cmd);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_FreeCommandBuffers(VkDevice _device, VkCommandPool _commandPool,
                       uint32_t commandBufferCount,
                       const VkCommandBuffer *pCommandBuffers)
{
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      struct ta5_cmd_buffer *cmd =
         ta5_cmd_buffer_from_handle(pCommandBuffers[i]);

      /*
       * All three arrays are grown by vk_realloc during recording: cs by
       * ta5_cmd_buffer_alloc_cs, relocs by ta5_cmd_buffer_grow_relocs and
       * reloc_bos by ta5_cs_reloc.  Freeing only cs leaks the other two per
       * command buffer, and relocs scales with how much the buffer recorded.
       *
       * device->vk.alloc, not pAllocator, because that is what all three were
       * allocated with -- ta5_AllocateCommandBuffers likewise ignores the
       * allocator from VkCommandBufferAllocateInfo, so there is nothing
       * per-buffer to honour here.
       */
      vk_free(&cmd->device->vk.alloc, cmd->reloc_bos);
      vk_free(&cmd->device->vk.alloc, cmd->relocs);
      vk_free(&cmd->device->vk.alloc, cmd->cs);
      vk_free(&cmd->device->vk.alloc, cmd);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_BeginCommandBuffer(VkCommandBuffer _cmd,
                       const VkCommandBufferBeginInfo *pBeginInfo)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);
   VkResult result;

   /* Recording into a buffer that is already recording or pending is
    * undefined behaviour per the Vulkan spec, so this is an assert rather
    * than an error return -- the same choice Turnip makes. */
   assert(cmd->state == MESA_VK_COMMAND_BUFFER_STATE_INITIAL);

   cmd->cs_count = 0;
   cmd->reloc_count = 0;
   cmd->reloc_bo_count = 0;
   cmd->submitted = 0;
   cmd->fence = NULL;
   cmd->state = MESA_VK_COMMAND_BUFFER_STATE_RECORDING;

   /*
    * No stream header is emitted: an a5xx command stream is a bare sequence
    * of PM4 packets.  (An a6xx-style CP_START header here is what caused a
    * GPU fault on the first submission -- see the kernel's "opcode error"
    * report.)
    */
   result = ta5_cmd_buffer_alloc_cs(cmd, TA5_CS_INITIAL_WORDS);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_EndCommandBuffer(VkCommandBuffer _cmd)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   assert(cmd->state == MESA_VK_COMMAND_BUFFER_STATE_RECORDING);

   /*
    * A command buffer that recorded nothing is legal and becomes executable.
    * Its stream is left empty; vkQueueSubmit recognises that and signals the
    * fence without going to the GPU.  Fabricating a packet here would put
    * invented PM4 on the ring, which the a5xx CP faults on.
    */
   cmd->state = MESA_VK_COMMAND_BUFFER_STATE_EXECUTABLE;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_ResetCommandBuffer(VkCommandBuffer _cmd, VkCommandBufferResetFlags flags)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   cmd->cs_count = 0;
   /*
    * The relocation list has to go too.  It is appended to as the stream is
    * recorded and is never trimmed, so a buffer that is reset and recorded
    * again -- which is what every per-frame app does -- would carry the first
    * recording's relocations into the second.  The submission path then
    * patches GPU addresses into payload dwords at the old stream offsets,
    * which are now past the end of the shorter stream: silent corruption of
    * the PM4 rather than a crash.
    */
   cmd->reloc_count = 0;
   cmd->reloc_bo_count = 0;
   cmd->submitted = 0;
   cmd->fence = NULL;
   cmd->state = MESA_VK_COMMAND_BUFFER_STATE_INITIAL;

   return VK_SUCCESS;
}
/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

#include "util/half_float.h"  /* a5xx.xml.h needs _mesa_float_to_half */

#include "freedreno_pm4.h"   /* pulls in adreno_pm4.xml.h */
#include "a5xx.xml.h"

#include "ir3/ir3_shader.h"
#include "drm/msm_drm.h"
#include "ta5_render.h"
#include "ta5_pipeline.h"
#include "ta5_descriptor.h"
#include "ta5_submit.h"
#include "ta5_memory.h"
#include "util/format/format_utils.h"

/** Clear colour, packed the way RB_CLEAR_COLOR_DW0..3 wants it. */
union ta5_color
{
   float f32[4];
   uint32_t u32[4];
};

/** Convert a VkClearColorValue into RB_CLEAR_COLOR_DW0..3 order. */
static union ta5_color
ta5_color_from_clear(VkClearColorValue c)
{
   union ta5_color out;

   /* The RB wants RGBA in dword 0..3 for an RGBA8 target. */
   out.u32[0] = _mesa_float_to_unorm(c.float32[0], 8);
   out.u32[1] = _mesa_float_to_unorm(c.float32[1], 8);
   out.u32[2] = _mesa_float_to_unorm(c.float32[2], 8);
   out.u32[3] = _mesa_float_to_unorm(c.float32[3], 8);
   return out;
}

static VkResult
ta5_cmd_buffer_grow_relocs(struct ta5_cmd_buffer *cmd, uint32_t count)
{
   void *mem;

   if (count <= cmd->reloc_size)
      return VK_SUCCESS;

   mem = vk_realloc(&cmd->device->vk.alloc, cmd->relocs,
                    count * sizeof(*cmd->relocs), 4,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!mem)
      return vk_error(&cmd->device->instance->vk,
                      VK_ERROR_OUT_OF_HOST_MEMORY);
   cmd->relocs = (struct ta5_reloc *)mem;
   cmd->reloc_size = count;
   return VK_SUCCESS;
}

/**
 * Emit a relocated GPU address, the way freedreno's OUT_RELOC() does.
 *
 * a5xx (fd_dev_gen() >= 5) carries buffer addresses in 64 bit register
 * fields, so one address is *two* stream dwords and the kernel has to patch
 * both: the low half with shift 0, the high half with shift -32 so that it
 * lands in bits 32..63.  Getting this wrong is silent -- the low half looks
 * perfectly plausible, so the RB is handed a base address of
 * (addr << 32) | <the next packet header> and quietly renders nowhere.
 *
 * The low dword carries any fixed bits in or_mask, exactly like OUT_RELOC().
 */
static VkResult
ta5_cs_reloc(struct ta5_cmd_buffer *cmd, struct ta5_bo *bo, uint64_t bo_offset,
             uint32_t or_mask)
{
   uint32_t i;
   VkResult ret;

   ret = ta5_cmd_buffer_grow_relocs(cmd, cmd->reloc_count + 2);
   if (ret != VK_SUCCESS)
      return ret;

   for (i = 0; i < cmd->reloc_bo_count; i++) {
      if (cmd->reloc_bos[i] == bo)
         break;
   }
   if (i == cmd->reloc_bo_count) {
      cmd->reloc_bos =
         (struct ta5_bo **)vk_realloc(&cmd->device->vk.alloc,
                                       cmd->reloc_bos,
                                       (cmd->reloc_bo_count + 1) *
                                          sizeof(*cmd->reloc_bos),
                                       8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!cmd->reloc_bos)
         return vk_error(&cmd->device->instance->vk,
                         VK_ERROR_OUT_OF_HOST_MEMORY);
      cmd->reloc_bos[i] = bo;
      cmd->reloc_bo_count++;
   }

   cmd->cs[cmd->cs_count++] = or_mask;
   cmd->cs[cmd->cs_count++] = 0;

   cmd->relocs[cmd->reloc_count].bo_index = i;
   cmd->relocs[cmd->reloc_count].bo_offset = bo_offset;
   cmd->relocs[cmd->reloc_count].dst_off = (cmd->cs_count - 2) * 4;
   cmd->relocs[cmd->reloc_count].or_mask = or_mask;
   cmd->relocs[cmd->reloc_count].shift = 0;
   cmd->reloc_count++;

   cmd->relocs[cmd->reloc_count].bo_index = i;
   cmd->relocs[cmd->reloc_count].bo_offset = bo_offset;
   cmd->relocs[cmd->reloc_count].dst_off = (cmd->cs_count - 1) * 4;
   cmd->relocs[cmd->reloc_count].or_mask = 0;   /* or_mask >> 32 */
   cmd->relocs[cmd->reloc_count].shift = -32;
   cmd->reloc_count++;

   return VK_SUCCESS;
}

/** Emit one dword into the recorded stream. */
static void
ta5_cs_push(struct ta5_cmd_buffer *cmd, uint32_t word)
{
   cmd->cs[cmd->cs_count++] = word;
}

/** Type-4 packet: write `cnt` dwords to a register page. */
static void
ta5_pkt4(struct ta5_cmd_buffer *cmd, uint32_t reg, uint32_t cnt)
{
   ta5_cs_push(cmd, CP_TYPE4_PKT | cnt | (ta5_pm4_odd_parity(cnt) << 7) |
                     ((reg & 0x3ffff) << 8) |
                     (ta5_pm4_odd_parity(reg) << 27));
}

/**
 * Type-7 packet.
 *
 * `cnt` is freedreno's OUT_PKT7() count field -- total dwords in the packet
 * minus one, i.e. the number of payload dwords.  CP_LOAD_STATE4(3) is
 * therefore three payload dwords, not four.
 */
static void
ta5_pkt7(struct ta5_cmd_buffer *cmd, uint32_t opcode, uint32_t cnt)
{
   ta5_cs_push(cmd, ta5_pm4_pkt7(opcode, cnt));
}

/** OUT_WFI5: a zero-count CP_WAIT_FOR_IDLE. */
static void
ta5_wfi(struct ta5_cmd_buffer *cmd)
{
   if (getenv("TA5_SKIP_WFI"))
      return;
   ta5_cs_push(cmd, ta5_pm4_pkt7(CP_WAIT_FOR_IDLE, 0));
}

/** CP_EVENT_WRITE without a timestamp. */
static void
ta5_event_write(struct ta5_cmd_buffer *cmd, uint32_t evt)
{
   ta5_pkt7(cmd, CP_EVENT_WRITE, 1);
   ta5_cs_push(cmd, CP_EVENT_WRITE_0_EVENT((enum vgt_event_type)evt));
}

/**
 * CP_EVENT_WRITE with a timestamp, which is what a clear needs.
 *
 * The untimestamped form tells the CCU to resolve but does not wait for it,
 * and the clear then never lands.  The timestamp destination is a reloc, so
 * this can fail where ta5_event_write() cannot.
 */
static VkResult
ta5_event_write_ts(struct ta5_cmd_buffer *cmd, struct ta5_bo *bo, uint32_t evt)
{
   VkResult ret;

   ta5_pkt7(cmd, CP_EVENT_WRITE, 4);
   ta5_cs_push(cmd, CP_EVENT_WRITE_0_EVENT((enum vgt_event_type)evt));
   ret = ta5_cs_reloc(cmd, bo, 0, 0);
   if (ret != VK_SUCCESS)
      return ret;
   ta5_cs_push(cmd, 0x00000000);   /* CP_EVENT_WRITE_3_DATA */
   return VK_SUCCESS;
}

/**
 * Emit the program state fd5_program_emit() emits for a VS+FS pipeline.
 *
 * CP_LOAD_STATE4 on its own is not enough: HLSQ needs to be told where each
 * stage's constant bank and instruction stream live, how big the register
 * footprints are, and which registers the vertex fetch unit and the rasteriser
 * should read.  GL emits all of this from fd5_program_emit(); without it the
 * draw packet is accepted and discarded.
 *
 * The layout follows fd5_program_emit() for the subset this driver supports:
 * vertex plus fragment, one colour target, no tessellation, geometry or
 * stream-out.  Stages are laid out exactly the way setup_stages() does --
 * VS at the bottom of the constant space, FS taking the rest -- so that the
 * register values can be diffed against the GL backend's.
 */
/*
 * Advance a register id, keeping the invalid marker intact.  Mirrors
 * next_regid() in fd5_program.c: an invalid regid has no neighbours, so the
 * result stays invalid rather than wrapping into a real register.
 */
static inline uint32_t
ta5_next_regid(uint32_t reg, uint32_t increment)
{
   if (VALIDREG(reg))
      return reg + increment;
   else
      return regid(63, 0);
}

/**
 * Length of a shader program in CP_LOAD_STATE4 units of 16 instructions.
 *
 * One unit is 16 *instructions* and an instruction is a 64-bit qword, so a unit
 * is 32 dwords.  ir3_compiler.c:361 sets instr_align = 16 for a5xx,
 * ir3.c:394 computes v->instrlen = DIV_ROUND_UP(instr_count, instr_align), and
 * ir3.c:400 computes info->size = instrlen * instr_align * 8 bytes.  8 bytes
 * per instruction is the "each instruction is 64bits (qword)" rule, so
 * info->size == instrlen * 128 bytes == instrlen * 32 dwords exactly.
 *
 * Measured on this driver's own shaders: the vertex stage reports sizedwords
 * 64 with instrlen 2, the fragment stage sizedwords 32 with instrlen 1.  So
 * sizedwords == instrlen * 32, and the divisor below is 16 -- twice the true
 * value, counting instructions as though they were dwords.
 *
 * That is known-wrong arithmetic, and it is being kept deliberately because
 * the two configurations were measured on the device and the wrong one is the
 * one that works:
 *
 *   divisor 16 (VS 4 units, FS 2 units)   0 faults, every run
 *   divisor 32 (VS 2 units, FS 1 unit)    2 faults, every run
 *   TA5_SHADER_UNITS=2 on both stages     0 faults, every run
 *
 * The third line isolates it: divisor 32 differs from units=2 only in the
 * fragment stage, so FS NUM_UNIT=1 is what faults.  Our FS is exactly 32
 * dwords -- precisely one unit, with no second unit behind it for the fetch to
 * land in.  GL's own stream does load its FS program with NUM_UNIT=1 and works
 * (glsys.cs.0.bo5 [178]), so 1 is a length GL genuinely uses, but GL's shader
 * is a different one and that is not a like-for-like comparison.
 *
 * Neither configuration renders the triangle, so this is not that bug.  It is
 * left alone because a deterministic fault is the worse outcome while the
 * reason is unknown.  The divisor becomes 32 once it is clear why a one-unit
 * fragment load faults.
 */
static uint32_t
ta5_shader_units(const struct ta5_compiled_shader *shader)
{
   const struct ir3_shader_variant *v =
      (const struct ir3_shader_variant *)shader->variant;
   uint32_t units = MAX2(shader->instrlen, DIV_ROUND_UP(v->info.sizedwords, 16));
   const char *ov = getenv("TA5_SHADER_UNITS");

   if (ov) {
      uint32_t o = (uint32_t)atoi(ov);
      /* Settable in both directions so this can be swept down as well as up. */
      units = o ? o : units;
   }

   return units;
}

/**
 * The length HLSQ_xS_CNTL.INSTRLEN is given for a stage.
 *
 * Kept separate from ta5_shader_units() because the two do not want the same
 * answer: raising NUM_UNIT without raising INSTRLEN takes the fragment fault
 * from two unmapped reads down to one, while raising both puts the second
 * back.  The two fields are not obviously the same unit, and which one is
 * right is settled by measurement, so the override exists to sweep them.
 */
static uint32_t
ta5_instrlen(const struct ta5_compiled_shader *shader)
{
   uint32_t units = shader->instrlen;
   const char *ov = getenv("TA5_INSTRLEN");

   if (ov) {
      uint32_t o = (uint32_t)atoi(ov);
      if (o > units)
         units = o;
   }

   return units;
}

static void
ta5_emit_program_state(struct ta5_cmd_buffer *cmd, struct ta5_pipeline *pipe)
{
   const struct ir3_shader_variant *vs =
      (const struct ir3_shader_variant *)pipe->vertex.variant;
   const struct ir3_shader_variant *fs =
      (const struct ir3_shader_variant *)pipe->fragment.variant;

   /* constlen is in units of 4 * vec4 (setup_stages()). */
   uint32_t vs_constlen = pipe->vertex.constlen / 4;
   uint32_t fs_constlen = pipe->fragment.constlen / 4;
   uint32_t vs_constoff = 0;
   uint32_t fs_constoff = vs_constlen;
   uint32_t vs_instrlen = ta5_instrlen(&pipe->vertex);
   uint32_t fs_instrlen = ta5_instrlen(&pipe->fragment);
   uint32_t fs_instroff = 64 - fs_instrlen;

   struct ir3_shader_linkage l = {0};
   uint32_t pos_regid, psize_regid, clip0_regid, clip1_regid;
   uint32_t vertex_regid, instance_regid, color_regid;
   uint8_t pos_loc = 0, psize_loc = ~0;
   uint8_t clip_mask = vs->clip_mask, cull_mask = vs->cull_mask;
   uint8_t clip_cull_mask = clip_mask | cull_mask;
   enum a3xx_threadsize fssz =
      (fs->info.double_threadsize || getenv("TA5_FS_FOUR_QUADS")) ? FOUR_QUADS
                                                                   : TWO_QUADS;

   pos_regid = ir3_find_output_regid(vs, VARYING_SLOT_POS);
   psize_regid = ir3_find_output_regid(vs, VARYING_SLOT_PSIZ);
   clip0_regid = ir3_find_output_regid(vs, VARYING_SLOT_CLIP_DIST0);
   clip1_regid = ir3_find_output_regid(vs, VARYING_SLOT_CLIP_DIST1);
   vertex_regid =
      ir3_find_sysval_regid(vs, SYSTEM_VALUE_VERTEX_ID_ZERO_BASE);
   instance_regid = ir3_find_sysval_regid(vs, SYSTEM_VALUE_INSTANCE_ID);

   color_regid = fs->color0_mrt
                     ? ir3_find_output_regid(fs, FRAG_RESULT_COLOR)
                     : ir3_find_output_regid(fs, FRAG_RESULT_DATA0);

   ir3_link_shaders(&l, vs, fs, true);

   if (getenv("TA5_TRACE_NULL"))
      fprintf(stderr,
              "ta5[link]: pre  pos_regid=0x%x cnt=%u max_loc=%u "
              "varmask0=0x%x\n",
              pos_regid, l.cnt, l.max_loc, l.varmask[0]);

   /*
    * VPC_VAR_DISABLE is programmed from the *inter-stage* linkage alone.
    * fd5 emits it before appending position/psize/clip distances to the
    * linkage map (fd5_program.c:427, then :437-444), so position is
    * deliberately absent from the mask.  Building the map first and
    * programming the mask from it afterwards -- as this used to do -- marks
    * varying slots 0-3 as live, and the VPC then goes looking for an
    * interpolated position that the FS never consumes and the VS, whose
    * outputs start at slot NUMNONPOSVAR, never stored.
    */
   const uint32_t varmask[4] = {l.varmask[0], l.varmask[1], l.varmask[2],
                                l.varmask[3]};

   /* a5xx appends pos/psize to the end of the linkage map. */
   pos_loc = l.max_loc;
   if (VALIDREG(pos_regid))
      ir3_link_add(&l, VARYING_SLOT_POS, pos_regid, 0xf, l.max_loc);
   if (VALIDREG(psize_regid)) {
      psize_loc = l.max_loc;
      ir3_link_add(&l, VARYING_SLOT_PSIZ, psize_regid, 0x1, l.max_loc);
   }
   if (l.clip0_loc == 0xff && VALIDREG(clip0_regid) &&
       (clip_cull_mask & 0xf) != 0) {
      l.clip0_loc = l.max_loc;
      ir3_link_add(&l, VARYING_SLOT_CLIP_DIST0, clip0_regid,
                   clip_cull_mask & 0xf, l.max_loc);
   }
   if (l.clip1_loc == 0xff && VALIDREG(clip1_regid) &&
       (clip_cull_mask >> 4) != 0) {
      l.clip1_loc = l.max_loc;
      ir3_link_add(&l, VARYING_SLOT_CLIP_DIST1, clip1_regid,
                   clip_cull_mask >> 4, l.max_loc);
   }

   if (getenv("TA5_TRACE_NULL"))
      fprintf(stderr,
              "ta5[link]: post pos_regid=0x%x cnt=%u var0.regid=0x%x "
              "var0.loc=%u var0.compmask=0x%x pos_loc=%u\n",
              pos_regid, l.cnt, l.cnt ? l.var[0].regid : 0,
              l.cnt ? l.var[0].loc : 0, l.cnt ? l.var[0].compmask : 0,
              pos_loc);

   /* HLSQ_xS_CONFIG: constant-object offset, shader-object offset, enable. */
   ta5_pkt4(cmd, REG_A5XX_HLSQ_VS_CONFIG, 5);
   ta5_cs_push(cmd, A5XX_HLSQ_VS_CONFIG_CONSTOBJECTOFFSET(vs_constoff) |
                     A5XX_HLSQ_VS_CONFIG_SHADEROBJOFFSET(0) |
                     A5XX_HLSQ_VS_CONFIG_ENABLED);
   ta5_cs_push(cmd, A5XX_HLSQ_FS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                     A5XX_HLSQ_FS_CONFIG_SHADEROBJOFFSET(fs_instroff) |
                     A5XX_HLSQ_FS_CONFIG_ENABLED);
   /*
    * HS/DS/GS are disabled but their CONFIG is still part of the constant
    * object layout: freedreno gives them the constoff accumulated past the
    * fragment stage and the fragment stage's shader-object offset.  Leaving
    * them at zero is not the same thing as "off" to the HLSQ.
    */
   ta5_cs_push(cmd, A5XX_HLSQ_HS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                     A5XX_HLSQ_HS_CONFIG_SHADEROBJOFFSET(fs_instroff));
   ta5_cs_push(cmd, A5XX_HLSQ_DS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                     A5XX_HLSQ_DS_CONFIG_SHADEROBJOFFSET(fs_instroff));
   ta5_cs_push(cmd, A5XX_HLSQ_GS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                     A5XX_HLSQ_GS_CONFIG_SHADEROBJOFFSET(fs_instroff));

   ta5_pkt4(cmd, REG_A5XX_HLSQ_CS_CONFIG, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_HLSQ_VS_CNTL, 5);
   ta5_cs_push(cmd, A5XX_HLSQ_VS_CNTL_INSTRLEN(vs_instrlen));
   ta5_cs_push(cmd, A5XX_HLSQ_FS_CNTL_INSTRLEN(fs_instrlen));
   ta5_cs_push(cmd, 0x00000000);   /* HLSQ_HS_CNTL */
   ta5_cs_push(cmd, 0x00000000);   /* HLSQ_DS_CNTL */
   ta5_cs_push(cmd, 0x00000000);   /* HLSQ_GS_CNTL */

   ta5_pkt4(cmd, REG_A5XX_SP_VS_CONFIG, 5);
   ta5_cs_push(cmd, A5XX_SP_VS_CONFIG_CONSTOBJECTOFFSET(vs_constoff) |
                     A5XX_SP_VS_CONFIG_SHADEROBJOFFSET(0) |
                     A5XX_SP_VS_CONFIG_ENABLED);
   ta5_cs_push(cmd, A5XX_SP_FS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                     A5XX_SP_FS_CONFIG_SHADEROBJOFFSET(fs_instroff) |
                     A5XX_SP_FS_CONFIG_ENABLED);
   ta5_cs_push(cmd, A5XX_SP_HS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                     A5XX_SP_HS_CONFIG_SHADEROBJOFFSET(fs_instroff));
   ta5_cs_push(cmd, A5XX_SP_DS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                     A5XX_SP_DS_CONFIG_SHADEROBJOFFSET(fs_instroff));
   ta5_cs_push(cmd, A5XX_SP_GS_CONFIG_CONSTOBJECTOFFSET(fs_constoff) |
                     A5XX_SP_GS_CONFIG_SHADEROBJOFFSET(fs_instroff));

   ta5_pkt4(cmd, REG_A5XX_SP_CS_CONFIG, 1);
   ta5_cs_push(cmd, 0x00000000);

   /* HLSQ_xS_CONSTLEN / HLSQ_xS_INSTRLEN, per stage. */
   ta5_pkt4(cmd, REG_A5XX_HLSQ_VS_CONSTLEN, 2);
   ta5_cs_push(cmd, vs_constlen);
   ta5_cs_push(cmd, vs_instrlen);

   ta5_pkt4(cmd, REG_A5XX_HLSQ_FS_CONSTLEN, 2);
   ta5_cs_push(cmd, fs_constlen);
   ta5_cs_push(cmd, fs_instrlen);

   ta5_pkt4(cmd, REG_A5XX_HLSQ_HS_CONSTLEN, 2);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_HLSQ_DS_CONSTLEN, 2);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_HLSQ_GS_CONSTLEN, 2);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_HLSQ_CS_CONSTLEN, 2);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_SP_VS_CTRL_REG0, 1);
   ta5_cs_push(cmd,
               A5XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT(vs->info.max_half_reg + 1) |
                  A5XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(vs->info.max_reg + 1) |
                  A5XX_SP_VS_CTRL_REG0_BUFFER | 0x2 |
                  A5XX_SP_VS_CTRL_REG0_BRANCHSTACK(
                     ir3_shader_branchstack_hw(vs)));

   ta5_pkt4(cmd, REG_A5XX_SP_FS_CTRL_REG0, 1);
   ta5_cs_push(cmd,
               (fs->total_in > 0 ? A5XX_SP_FS_CTRL_REG0_VARYING : 0) |
                  0x40002 /* XXX set pretty much everywhere */ |
                  A5XX_SP_FS_CTRL_REG0_BUFFER |
                  A5XX_SP_FS_CTRL_REG0_THREADSIZE(fssz) |
                  A5XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT(
                     fs->info.max_half_reg + 1) |
                  A5XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(fs->info.max_reg + 1) |
                  A5XX_SP_FS_CTRL_REG0_BRANCHSTACK(
                     ir3_shader_branchstack_hw(fs)));

   ta5_pkt4(cmd, REG_A5XX_HLSQ_CONTROL_0_REG, 5);
   ta5_cs_push(cmd, A5XX_HLSQ_CONTROL_0_REG_FSTHREADSIZE(fssz) |
                     A5XX_HLSQ_CONTROL_0_REG_CSTHREADSIZE(TWO_QUADS) |
                     0x00000880 /* XXX HLSQ_CONTROL_0 */);
   ta5_cs_push(cmd, A5XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD(63));

   /*
    * CONTROL_2..4 hold the register ids the rasterizer uses to deliver face,
    * sample and barycentric values to the fragment shader.  For a shader that
    * consumes none of them every field must be the *invalid* regid, regid(63,
    * 0) == 0xfc, which is what ir3_find_sysval_regid() returns for a missing
    * sysval.  Writing 0 here instead names a real register -- one the shader
    * never allocated for interpolation -- and the draw faults with
    * UCHE_OOB_ACCESS | RBBM_AHB_ERROR.
    */
   {
      uint32_t face_regid =
         ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRONT_FACE);
      uint32_t samp_id_regid =
         ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_ID);
      uint32_t samp_mask_regid =
         ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_MASK_IN);
      uint32_t coord_regid =
         ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRAG_COORD);
      uint32_t zwcoord_regid = ta5_next_regid(coord_regid, 2);
      uint32_t ij_regid[IJ_COUNT];

      for (unsigned i = 0; i < IJ_COUNT; i++)
         ij_regid[i] = ir3_find_sysval_regid(
            fs, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + i);

      ta5_cs_push(cmd,
                  A5XX_HLSQ_CONTROL_2_REG_FACEREGID(face_regid) |
                     A5XX_HLSQ_CONTROL_2_REG_SAMPLEID(samp_id_regid) |
                     A5XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(samp_mask_regid) |
                     A5XX_HLSQ_CONTROL_2_REG_CENTERRHW(
                        ij_regid[IJ_PERSP_CENTER_RHW]));
      ta5_cs_push(cmd,
                  A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL(
                     ij_regid[IJ_PERSP_PIXEL]) |
                     A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL(
                        ij_regid[IJ_LINEAR_PIXEL]) |
                     A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID(
                        ij_regid[IJ_PERSP_CENTROID]) |
                     A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID(
                        ij_regid[IJ_LINEAR_CENTROID]));
      ta5_cs_push(cmd,
                  A5XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(coord_regid) |
                     A5XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(zwcoord_regid) |
                     A5XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE(
                        ij_regid[IJ_PERSP_SAMPLE]) |
                     A5XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE(
                        ij_regid[IJ_LINEAR_SAMPLE]));
   }

   ta5_pkt4(cmd, REG_A5XX_VPC_GS_SIV_CNTL, 1);
   ta5_cs_push(cmd, 0x0000ffff);   /* XXX */

   ta5_pkt4(cmd, REG_A5XX_SP_SP_CNTL, 1);
   ta5_cs_push(cmd, 0x00000010);   /* XXX */

   ta5_pkt4(cmd, REG_A5XX_GRAS_CNTL, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CONTROL0, 2);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   /*
    * Only attachment 0 is bound.  The unused slots must carry the invalid
    * regid (0xfc) rather than 0, so a stray write to an unbound MRT cannot be
    * attributed to a real register.
    */
   ta5_pkt4(cmd, REG_A5XX_SP_FS_OUTPUT_REG(0), 8);
   for (int i = 0; i < 8; i++) {
      ta5_cs_push(cmd, (i == 0 ? color_regid : regid(63, 0)) |
                        ((i == 0 && VALIDREG(color_regid) &&
                          (color_regid & HALF_REG_ID))
                            ? A5XX_SP_FS_OUTPUT_REG_HALF_PRECISION
                            : 0));
   }

   ta5_pkt4(cmd, REG_A5XX_VPC_PACK, 1);
   ta5_cs_push(cmd, A5XX_VPC_PACK_NUMNONPOSVAR(pos_loc) |
                     A5XX_VPC_PACK_PSIZELOC(VALIDREG(psize_regid) ? psize_loc
                                                                  : 0xff));

   ta5_pkt4(cmd, REG_A5XX_VPC_VAR_DISABLE(0), 4);
   ta5_cs_push(cmd, ~varmask[0]);
   ta5_cs_push(cmd, ~varmask[1]);
   ta5_cs_push(cmd, ~varmask[2]);
   ta5_cs_push(cmd, ~varmask[3]);

   ta5_pkt4(cmd, REG_A5XX_VPC_VARYING_INTERP_MODE(0), 8);
   for (int i = 0; i < 8; i++)
      ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_VPC_VARYING_PS_REPL_MODE(0), 8);
   for (int i = 0; i < 8; i++)
      ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_GRAS_VS_CL_CNTL, 1);
   ta5_cs_push(cmd, A5XX_GRAS_VS_CL_CNTL_CLIP_MASK(clip_mask) |
                     A5XX_GRAS_VS_CL_CNTL_CULL_MASK(cull_mask));

   ta5_pkt4(cmd, REG_A5XX_VPC_CLIP_CNTL, 1);
   ta5_cs_push(cmd, A5XX_VPC_CLIP_CNTL_CLIP_MASK(clip_cull_mask) |
                     A5XX_VPC_CLIP_CNTL_CLIP_DIST_03_LOC(l.clip0_loc) |
                     A5XX_VPC_CLIP_CNTL_CLIP_DIST_47_LOC(l.clip1_loc));

   ta5_pkt4(cmd, REG_A5XX_PC_CLIP_CNTL, 1);
   ta5_cs_push(cmd, A5XX_PC_CLIP_CNTL_CLIP_MASK(clip_mask));

   ta5_pkt4(cmd, REG_A5XX_PC_PRIM_VTX_CNTL, 1);
   ta5_cs_push(cmd, VALIDREG(psize_regid) ? A5XX_PC_PRIM_VTX_CNTL_PSIZE : 0);

   ta5_pkt4(cmd, REG_A5XX_SP_PRIMITIVE_CNTL, 1);
   /* VSOUT counts outputs the VS actually produces, which is l.cnt -- not the
    * l.max_loc stride, which is a different (larger) number and tells the SP
    * to emit more vectors per vertex than exist.
    */
   ta5_cs_push(cmd, A5XX_SP_PRIMITIVE_CNTL_VSOUT(l.cnt));

   ta5_pkt4(cmd, REG_A5XX_VPC_CNTL_0, 1);
   ta5_cs_push(cmd, A5XX_VPC_CNTL_0_STRIDE_IN_VPC(l.max_loc) |
                     (fs->total_in > 0 ? A5XX_VPC_CNTL_0_VARYING : 0) |
                     0x00010000 /* XXX */);

   /*
    * The primitive assembler reads vertices out of the VPC with the same
    * stride, so it has to agree with VPC_CNTL_0 above.  It lives here rather
    * than in the draw state because that is where the stride is known --
    * freedreno notes the same overlap and emits it after the program state.
    * A stride that disagrees with the VPC reads positions from the wrong
    * offsets and the draw rasterises nothing.
    */
   ta5_pkt4(cmd, REG_A5XX_PC_PRIMITIVE_CNTL, 1);
   ta5_cs_push(cmd, A5XX_PC_PRIMITIVE_CNTL_STRIDE_IN_VPC(l.max_loc) |
                     A5XX_PC_PRIMITIVE_CNTL_PROVOKING_VTX_LAST);

   ta5_pkt4(cmd, REG_A5XX_VFD_CONTROL_0, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_VFD_INDEX_OFFSET, 2);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_VFD_CONTROL_1, 5);
   ta5_cs_push(cmd, A5XX_VFD_CONTROL_1_REGID4VTX(vertex_regid) |
                     A5XX_VFD_CONTROL_1_REGID4INST(instance_regid) | 0xfc0000);
   ta5_cs_push(cmd, 0x0000fcfc);   /* VFD_CONTROL_2 */
   ta5_cs_push(cmd, 0x0000fcfc);   /* VFD_CONTROL_3 */
   ta5_cs_push(cmd, 0x000000fc);   /* VFD_CONTROL_4 */
   ta5_cs_push(cmd, 0x00000000);   /* VFD_CONTROL_5 */

   /*
    * Pulse last: HLSQ latches the config words above when this is written.
    * GL emits it mid-program, but nothing between there and the draw reads
    * HLSQ state, so emitting it once here is equivalent.
    */
   /*
    * SP_VS_OUT / SP_VS_VPC_DST: where the shader core drops each vertex output
    * and where VPC then places it.  Two linked varyings per SP_VS_OUT word and
    * four locations per SP_VS_VPC_DST word.  GL emits these from
    * fd5_program_emit(); without them the vertex stage has nowhere to put its
    * results and the draw produces nothing, however correct the shaders are.
    */
   for (int i = 0, j = 0; i < 16 && j < l.cnt; i++) {
      uint32_t reg = 0;

      ta5_pkt4(cmd, REG_A5XX_SP_VS_OUT(i), 1);

      reg |= A5XX_SP_VS_OUT_REG_A_REGID(l.var[j].regid) |
             A5XX_SP_VS_OUT_REG_A_COMPMASK(l.var[j].compmask);
      j++;

      reg |= A5XX_SP_VS_OUT_REG_B_REGID(l.var[j].regid) |
             A5XX_SP_VS_OUT_REG_B_COMPMASK(l.var[j].compmask);
      j++;

      ta5_cs_push(cmd, reg);
   }

   for (int i = 0, j = 0; i < 8 && j < l.cnt; i++) {
      uint32_t reg = 0;

      ta5_pkt4(cmd, REG_A5XX_SP_VS_VPC_DST(i), 1);

      reg |= A5XX_SP_VS_VPC_DST_REG_OUTLOC0(l.var[j++].loc);
      reg |= A5XX_SP_VS_VPC_DST_REG_OUTLOC1(l.var[j++].loc);
      reg |= A5XX_SP_VS_VPC_DST_REG_OUTLOC2(l.var[j++].loc);
      reg |= A5XX_SP_VS_VPC_DST_REG_OUTLOC3(l.var[j++].loc);

      ta5_cs_push(cmd, reg);
   }

   ta5_pkt4(cmd, REG_A5XX_HLSQ_UPDATE_CNTL, 1);
   ta5_cs_push(cmd, 0x020fffff);   /* XXX */
}

/**
 * Upload one stage's immediate constants into its constant file.
 *
 * a5xx does *not* carry shader constants in the shader preamble.  That is an
 * a7xx feature -- GPUProps.load_shader_consts_via_preamble is only set on
 * a7xx_gen3 -- so on this generation immediates and constant data sit in the
 * shader's constant file and the CP must fetch them with a
 * CP_LOAD_STATE4/ST4_CONSTANTS load ahead of the draw.  A vertex shader that
 * indexes a constant array, which is the ordinary way to spell a triangle,
 * then reads c[...] that was never written, and the UCHE reports it as an
 * out-of-bounds access at the draw.
 *
 * This mirrors ir3_emit_immediates() in ir3_const.h; we cannot call it
 * directly because it emits into an fd_ringbuffer rather than our command
 * buffer.
 */
static void
ta5_emit_consts(struct ta5_cmd_buffer *cmd, struct ta5_pipeline *pipe,
                const struct ta5_compiled_shader *shader,
                enum a4xx_state_block state_block, struct ta5_bo *bo)
{
   const struct ir3_shader_variant *v =
      (const struct ir3_shader_variant *)shader->variant;
   const struct ir3_const_state *const_state = ir3_const_state(v);
   const struct ir3_ubo_analysis_state *state = &const_state->ubo_state;
   uint32_t base = const_state->allocs.max_const_offset_vec4;
   uint32_t size, regid, sizedwords;

   /*
    * Driver parameters.  Every vertex stage reserves a block at c0.x and the
    * shader reads it -- this triangle's reads c0.y, which is vtxid_base, and
    * adds it to the incoming vertex id.  They live in the variant rather than
    * the shader BO, so they are pushed inline with SS4_DIRECT, exactly as
    * ir3_emit_driver_params() does.
    *
    * Without this the vertex id is added to whatever happened to be in the
    * constant file, and every vertex lands somewhere arbitrary.
    */
   if (v->need_driver_params) {
      struct ir3_driver_params_vs params = {0};
      uint32_t offset =
         const_state->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4;
      uint32_t nparams = MIN2(const_state->num_driver_params,
                              (v->constlen - offset) * 4);

      params.vtxid_base = cmd->draw_index_size ? 0 : cmd->draw_first_vertex;
      params.instid_base = cmd->draw_first_instance;
      /* No transform feedback, and AUTO_INDEX rather than a real index
       * buffer, so both of these are zero.
       */
      params.vtxcnt_max = 0;
      params.is_indexed_draw = 0;

      if (nparams > 0) {
         ta5_pkt7(cmd, CP_LOAD_STATE4, 3 + nparams);
         ta5_cs_push(cmd, CP_LOAD_STATE4_0_DST_OFF(offset) |
                           CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                           CP_LOAD_STATE4_0_STATE_BLOCK(state_block) |
                           CP_LOAD_STATE4_0_NUM_UNIT(nparams / 4));
         ta5_cs_push(cmd, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                           CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
         ta5_cs_push(cmd, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
         for (uint32_t i = 0; i < nparams; i++)
            ta5_cs_push(cmd, ((uint32_t *)&params)[i]);
      }
   }

   /* a7xx+ folds these into the preamble instead; nothing to upload. */
   if (v->compiler->info->props.load_shader_consts_via_preamble)
      return;

   /*
    * Immediate constants.  Truncate to the constants this shader actually
    * uses.  These live in the variant, not the shader BO, so they are pushed
    * inline via SS4_DIRECT.
    */
   size = DIV_ROUND_UP(v->imm_state.count, 4);
   size = MIN2(size + base, v->constlen) - base;
   regid = base * 4;          /* vec4 -> dwords */
   sizedwords = size * 4;
   if (sizedwords > 0) {
      ta5_pkt7(cmd, CP_LOAD_STATE4, 3 + sizedwords);
      ta5_cs_push(cmd, CP_LOAD_STATE4_0_DST_OFF(regid / 4) |
                        CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                        CP_LOAD_STATE4_0_STATE_BLOCK(state_block) |
                        CP_LOAD_STATE4_0_NUM_UNIT(sizedwords / 4));
      ta5_cs_push(cmd, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                        CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
      ta5_cs_push(cmd, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
      for (uint32_t i = 0; i < sizedwords; i++)
         ta5_cs_push(cmd, v->imm_state.values[i]);
   }

   /*
    * Constant data.  This is the other half and the one that matters for a
    * plain triangle: the vertex shader's position array is NIR constant data
    * indexed by the vertex id, and IR3 stores it in the variant BO after the
    * microcode.  ta5_pipeline.cc copies that whole BO, so the data now lives
    * at bo_offset + v->info.constant_data_offset in the shader BO; load it
    * from there into the constant file at the regid the analysis assigned.
    *
    * Mirrors ir3_emit_constant_data() in ir3_const.h.
    */
   for (unsigned i = 0; i < state->num_enabled; i++) {
      unsigned ubo = state->range[i].ubo.block;

      if (ubo != const_state->consts_ubo.idx)
         continue;

      size = state->range[i].end - state->range[i].start;

      /* Pre-a6xx, a range can be enabled in the shader but unused in the
       * binning variant. */
      if (16 * v->constlen <= state->range[i].offset)
         continue;

      /* and even if the start is before first_immediate, the end may not be */
      size = MIN2(size, (16 * v->constlen) - state->range[i].offset);

      if (size == 0)
         continue;

      /*
       * ir3_emit_constant_data() works in *bytes*: range.offset is where the
       * range starts in the constant file and start/end are byte offsets into
       * the variant's constant_data.  The disassembly says so directly --
       *
       *     ubo_ranges   c4.x (4 vec4)
       *     offset:      c4.x
       *     size:        4 vec4 (0b -> 64b)
       *
       * so c4.x is dword 16 and the range is 64 *bytes*.  DST_OFF and NUM_UNIT
       * are in vec4, hence the two steps: bytes -> dwords -> vec4.  Feeding
       * the raw byte counts straight into those fields loads four times too
       * high and far too much, and the vertex shader's c[16..21] stays
       * uninitialised -- which is why the triangle rasterised nothing while
       * the stream itself stayed fault-free.
       */
      uint32_t offset_dwords = state->range[i].offset / 4;
      uint32_t size_dwords = size / 4;

      ta5_pkt7(cmd, CP_LOAD_STATE4, 3);
      ta5_cs_push(cmd, CP_LOAD_STATE4_0_DST_OFF(offset_dwords / 4) |
                        CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                        CP_LOAD_STATE4_0_STATE_BLOCK(state_block) |
                        CP_LOAD_STATE4_0_NUM_UNIT(size_dwords / 4));
      ta5_cs_reloc(cmd, bo,
                   v->info.constant_data_offset + state->range[i].start,
                   CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
   }
}

/**
 * Program SP_xS_OBJ_START, the descriptor HLSQ fetches the shader from.
 *
 * CP_LOAD_STATE4 only pushes the microcode into HLSQ's state block; the draw
 * itself is driven from the *shader object* whose start address lives in this
 * register.  fd5_emit_shader_obj() emits it immediately before
 * fd5_emit_shader(), and GL's captured stream shows both.  Without it HLSQ
 * has nothing to fetch and the draw completes without producing a pixel.
 *
 * The remaining four dwords describe the per-fiber private memory.  The
 * shaders this driver compiles need none of it, so they are all zero, which
 * is exactly what GL emits for them.
 */
static VkResult
ta5_emit_shader_obj(struct ta5_cmd_buffer *cmd, struct ta5_pipeline *pipe,
                    uint32_t reg, struct ta5_bo *bo)
{
   VkResult ret;

   ta5_pkt4(cmd, reg, 6);
   ret = ta5_cs_reloc(cmd, bo, 0, 0);
   if (ret != VK_SUCCESS)
      return ret;
   ta5_cs_push(cmd, 0x00000000);   /* SP_xS_PVT_MEM_PARAM */
   ta5_cs_push(cmd, 0x00000000);   /* SP_xS_PVT_MEM_ADDR lo */
   ta5_cs_push(cmd, 0x00000000);   /* SP_xS_PVT_MEM_ADDR hi */
   ta5_cs_push(cmd, 0x00000000);   /* SP_xS_PVT_MEM_SIZE_TOTALPVTMEMSIZE */
   return VK_SUCCESS;
}

/**
 * Upload one compiled shader stage with CP_LOAD_STATE4.
 *
 * a5xx keeps constants inside the shader stream, so the whole variant goes
 * over as a single ST4_SHADER blob rather than separate CONSTANTS/UBO/texture
 * loads.  SS4_INDIRECT is how GL does it too: the microcode stays in a BO and
 * the packet carries its address, rather than inlining the dwords.
 */
static VkResult
ta5_emit_shader(struct ta5_cmd_buffer *cmd, struct ta5_pipeline *pipe,
                const struct ta5_compiled_shader *shader,
                enum a4xx_state_block state_block, uint32_t obj_reg,
                struct ta5_bo *bo)
{
   VkResult ret;

   ret = ta5_emit_shader_obj(cmd, pipe, obj_reg, bo);
   if (ret != VK_SUCCESS)
      return ret;

   /*
    * TA5_SKIP_FS suppresses only the program load, not the object address.
    * Dropping the whole of ta5_emit_shader() would leave SP_FS_OBJ_START at
    * its reset value of zero, and a fragment-stage fault at IOVA 0x0 is
    * exactly what a missing program at address zero looks like -- which is
    * indistinguishable from the thing being investigated.
    */
   if (getenv("TA5_SKIP_FS"))
      return VK_SUCCESS;

   ta5_pkt7(cmd, CP_LOAD_STATE4, 3);
   ta5_cs_push(cmd, CP_LOAD_STATE4_0_DST_OFF(0) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(state_block) |
                     CP_LOAD_STATE4_0_NUM_UNIT(ta5_shader_units(shader)));
   ret = ta5_cs_reloc(cmd, bo, 0, CP_LOAD_STATE4_1_STATE_TYPE(ST4_SHADER));
   if (ret != VK_SUCCESS)
      return ret;

   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Render pass / draw                                                   */
/* ------------------------------------------------------------------ */

/*
 * Debug/bisect switches.
 *
 * The a5xx command stream still does not execute end-to-end on real hardware
 * (the CP fetches part of it and then hangs), and finding out where needs to
 * be able to take the stream apart without rebuilding each time.  All of
 * these are off unless the variable is set:
 *
 *   TA5_SKIP_RENDER_MODE   drop CP_SET_RENDER_MODE from the stream
 *   TA5_SKIP_WFI           drop every CP_WAIT_FOR_IDLE
 *   TA5_SKIP_PREP          stop after the state restore
 *   TA5_SKIP_DRAW          record no draw state, shaders or CP_DRAW_INDX_OFFSET
 *   TA5_SKIP_FS            load no fragment program at all
 *   TA5_FS_NO_DEPTH_OUT    leave DEPTH_REGID out of SP_FS_OUTPUT_CNTL
 *   TA5_CLEAR_IN_DRAW      emit a magenta clear at the top of the draw path,
 *                          as a reachability probe (if it lands, the CP got
 *                          that far; if not, it never did)
 *   TA5_SHADER_UNITS=N     raise the CP_LOAD_STATE4 program-load length
 *   TA5_INSTRLEN=N         raise HLSQ_xS_CNTL.INSTRLEN
 *   TA5_PREAMBLE_ROWS=N    emit only the first N state-restore register rows
 *
 * TA5_TRACE_NULL additionally dumps the recorded stream, its relocations and
 * the prep parameters.
 */

/** CP_SET_RENDER_MODE(BYPASS) -- must be the first packet in a stream. */
static void
ta5_set_render_mode(struct ta5_cmd_buffer *cmd)
{
   if (getenv("TA5_SKIP_RENDER_MODE"))
      return;
   ta5_pkt7(cmd, CP_SET_RENDER_MODE, 5);
   ta5_cs_push(cmd, CP_SET_RENDER_MODE_0_MODE(BYPASS));
   ta5_cs_push(cmd, 0x00000000); /* ADDR_LO */
   ta5_cs_push(cmd, 0x00000000); /* ADDR_HI */
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);
}

static void ta5_emit_draw_state(struct ta5_cmd_buffer *cmd);

/**
 * a5xx state restore.
 *
 * Transcribed from fd5_emit_restore() in the GL backend.  a5xx will not run a
 * draw without it: the stream has to begin with CP_SET_RENDER_MODE(BYPASS)
 * and a cache flush, and a long list of SP/VPC/TPL1 mode registers have to be
 * returned to a known state.  Leaving it out produces "CP | opcode error" on
 * the very first packet of the stream.
 *
 * The values are generated from the GL source, so they are transcribed rather
 * than typed by hand.
 */
static void
ta5_emit_restore(struct ta5_cmd_buffer *cmd)
{
   static const struct {
      uint32_t reg;
      uint8_t count;
      uint32_t val[6];
   } restore[] = {
      { REG_A5XX_HLSQ_UPDATE_CNTL, 1, { 0x000fffff } },
      { REG_A5XX_PC_RESTART_INDEX, 1, { 0xffffffff } },
      { REG_A5XX_PC_RASTER_CNTL, 1, { 0x00000012 } },
      { REG_A5XX_GRAS_SU_POINT_MINMAX, 2,
        { A5XX_GRAS_SU_POINT_MINMAX_MIN(1.0f) |
              A5XX_GRAS_SU_POINT_MINMAX_MAX(4092.0f),
          A5XX_GRAS_SU_POINT_SIZE(0.5f) } },
      { REG_A5XX_GRAS_SU_CONSERVATIVE_RAS_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_GRAS_SC_SCREEN_SCISSOR_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_SP_VS_CONFIG_MAX_CONST, 1, { 0x00000000 } },
      { REG_A5XX_SP_FS_CONFIG_MAX_CONST, 1, { 0x00000000 } },
      { REG_A5XX_UNKNOWN_E292, 2, { 0x00000000, 0x00000000 } },
      { REG_A5XX_RB_MODE_CNTL, 1, { 0x00000044 } },
      { REG_A5XX_RB_DBG_ECO_CNTL, 1, { 0x00100000 } },
      { REG_A5XX_VFD_MODE_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_PC_MODE_CNTL, 1, { 0x0000001f } },
      { REG_A5XX_SP_MODE_CNTL, 1, { 0x0000001e } },
      { REG_A5XX_SP_DBG_ECO_CNTL, 1, { 0x40000800 } },
      { REG_A5XX_TPL1_MODE_CNTL, 1, { 0x00000544 } },
      { REG_A5XX_HLSQ_TIMEOUT_THRESHOLD_0, 2, { 0x00000080, 0x00000000 } },
      { REG_A5XX_VPC_DBG_ECO_CNTL, 1, { 0x00000400 } },
      { REG_A5XX_HLSQ_MODE_CNTL, 1, { 0x00000001 } },
      { REG_A5XX_VPC_MODE_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_GRAS_SU_CONSERVATIVE_RAS_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_GRAS_SC_BIN_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_GRAS_SC_BIN_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_VPC_FS_PRIMITIVEID_CNTL, 1, { 0x000000ff } },
      { REG_A5XX_VPC_SO_OVERRIDE, 1, { A5XX_VPC_SO_OVERRIDE_SO_DISABLE } },
      { REG_A5XX_VPC_SO_BUFFER_BASE(0), 3, { 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_VPC_SO_FLUSH_BASE(0), 2, { 0x00000000, 0x00000000 } },
      { REG_A5XX_PC_GS_PARAM, 1, { 0x00000000 } },
      { REG_A5XX_PC_HS_PARAM, 1, { 0x00000000 } },
      { REG_A5XX_TPL1_TP_FS_ROTATION_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_UNKNOWN_E004, 1, { 0x00000000 } },
      { REG_A5XX_GRAS_SU_LAYERED, 1, { 0x00000000 } },
      { REG_A5XX_VPC_SO_BUF_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_VPC_SO_BUFFER_OFFSET(0), 1, { 0x00000000 } },
      { REG_A5XX_PC_GS_LAYERED, 1, { 0x00000000 } },
      { REG_A5XX_UNKNOWN_E5AB, 1, { 0x00000000 } },
      { REG_A5XX_UNKNOWN_E5C2, 1, { 0x00000000 } },
      { REG_A5XX_VPC_SO_BUFFER_BASE(1), 3, { 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_VPC_SO_BUFFER_OFFSET(1), 6, { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_VPC_SO_BUFFER_OFFSET(2), 6, { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_VPC_SO_BUFFER_OFFSET(3), 3, { 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_UNKNOWN_E5DB, 1, { 0x00000000 } },
      { REG_A5XX_SP_HS_CTRL_REG0, 1, { 0x00000000 } },
      { REG_A5XX_SP_GS_CTRL_REG0, 1, { 0x00000000 } },
      { REG_A5XX_TPL1_VS_TEX_COUNT, 4, { 0x00000000, 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_TPL1_FS_TEX_COUNT, 2, { 0x00000000, 0x00000000 } },
      { REG_A5XX_UNKNOWN_E7C0, 3, { 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_UNKNOWN_E7C5, 3, { 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_UNKNOWN_E7CA, 3, { 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_UNKNOWN_E7CF, 3, { 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_UNKNOWN_E7D4, 3, { 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_UNKNOWN_E7D9, 3, { 0x00000000, 0x00000000, 0x00000000 } },
      { REG_A5XX_RB_CLEAR_CNTL, 1, { 0x00000000 } },
   };
   size_t i;
   uint32_t j;

   ta5_set_render_mode(cmd);

   ta5_pkt4(cmd, REG_A5XX_UCHE_CACHE_INVALIDATE_MIN, 5);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000012);
   ta5_wfi(cmd);

   const char *lim = getenv("TA5_PREAMBLE_ROWS");
   size_t nrows = lim ? (size_t)atoi(lim) : ARRAY_SIZE(restore);

   for (i = 0; i < nrows && i < ARRAY_SIZE(restore); i++) {
      for (j = 0; j < restore[i].count; j++) {
         if (j == 0)
            ta5_pkt4(cmd, restore[i].reg, restore[i].count);
         ta5_cs_push(cmd, restore[i].val[j]);
      }
   }

   /* Draw-state groups are unused here; make sure none is left armed. */
   ta5_pkt7(cmd, CP_SET_DRAW_STATE, 3);
   ta5_cs_push(cmd, CP_SET_DRAW_STATE__0_COUNT(0) |
                     CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS |
                     CP_SET_DRAW_STATE__0_GROUP_ID(0));
   ta5_cs_push(cmd, CP_SET_DRAW_STATE__1_ADDR_LO(0));
   ta5_cs_push(cmd, CP_SET_DRAW_STATE__2_ADDR_HI(0));
}
/** Single-sample setup, from fd5_gmem.c:emit_msaa(). */
static void
ta5_emit_msaa(struct ta5_cmd_buffer *cmd)
{
   ta5_pkt4(cmd, REG_A5XX_TPL1_TP_RAS_MSAA_CNTL, 2);
   ta5_cs_push(cmd, A5XX_TPL1_TP_RAS_MSAA_CNTL_SAMPLES(MSAA_ONE));
   ta5_cs_push(cmd, A5XX_TPL1_TP_DEST_MSAA_CNTL_SAMPLES(MSAA_ONE) |
                     A5XX_TPL1_TP_DEST_MSAA_CNTL_MSAA_DISABLE);

   ta5_pkt4(cmd, REG_A5XX_RB_RAS_MSAA_CNTL, 2);
   ta5_cs_push(cmd, A5XX_RB_RAS_MSAA_CNTL_SAMPLES(MSAA_ONE));
   ta5_cs_push(cmd, A5XX_RB_DEST_MSAA_CNTL_SAMPLES(MSAA_ONE) |
                     A5XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE);

   ta5_pkt4(cmd, REG_A5XX_GRAS_SC_RAS_MSAA_CNTL, 2);
   ta5_cs_push(cmd, A5XX_GRAS_SC_RAS_MSAA_CNTL_SAMPLES(MSAA_ONE));
   ta5_cs_push(cmd, A5XX_GRAS_SC_DEST_MSAA_CNTL_SAMPLES(MSAA_ONE) |
                     A5XX_GRAS_SC_DEST_MSAA_CNTL_MSAA_DISABLE);
}

/**
 * Begin a render pass rendering straight to sysmem (a5xx "bypass" mode).
 *
 * This is fd5_emit_sysmem_prep() from the GL backend with gmem == NULL, so
 * RB_MRT points straight at the attachment buffer instead of GMEM.  It skips
 * the binning pass, the VSC, and the GMEM resolve entirely -- all of which
 * have to be undone by a gmem2mem copy afterwards -- which makes it by far
 * the shortest correct path to a first triangle.
 */
static VkResult
ta5_emit_sysmem_prep(struct ta5_cmd_buffer *cmd,
                     const struct ta5_framebuffer *fb)
{
   struct ta5_image *img =
      ta5_image_view_from_handle((VkImageView)fb->attachments[0])->image;
   struct ta5_bo *bo = img->bound_memory->bo;
   uint32_t w = fb->width, h = fb->height;
   uint32_t stride = (uint32_t)img->row_pitch;
   VkResult ret;

   if (getenv("TA5_TRACE_NULL"))
      fprintf(stderr, "ta5[prep]: fb %ux%u, img %ux%u fmt %d, row_pitch %llu, "
              "array_pitch %llu, offset %llu, bo %p\n",
              w, h, img->extent.width, img->extent.height, (int)img->format,
              (unsigned long long)img->row_pitch,
              (unsigned long long)img->array_pitch,
              (unsigned long long)img->offset, (void *)bo);

   ta5_emit_restore(cmd);

   if (getenv("TA5_SKIP_PREP"))
      return VK_SUCCESS;

   /* fd5_emit_lrz_flush() */
   ta5_pkt4(cmd, REG_A5XX_GRAS_LRZ_CNTL, 1);
   ta5_cs_push(cmd, A5XX_GRAS_LRZ_CNTL_ENABLE);
   ta5_event_write(cmd, LRZ_FLUSH_INVALIDATE);

   ta5_pkt7(cmd, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
   ta5_cs_push(cmd, 0);

   ta5_event_write(cmd, PC_CCU_INVALIDATE_COLOR);

   uint32_t num_sp = cmd->device->physical_device->gpu_info.num_sp_cores;

   ta5_pkt4(cmd, REG_A5XX_PC_POWER_CNTL, 1);
   ta5_cs_push(cmd, num_sp - 1);

   ta5_pkt4(cmd, REG_A5XX_VFD_POWER_CNTL, 1);
   ta5_cs_push(cmd, num_sp - 1);

   /*
    * No wait-for-idle here.  GL calls fd_wfi() at this point, but that is
    * guarded on batch->needs_wfi, and the cache flush in ta5_emit_restore()
    * has already reset and consumed the flag -- so GL emits exactly one WFI
    * per stream, in the cache flush.  Emitting a second one parks the CP
    * waiting on shader cores that never go idle here.
    */

   ta5_pkt4(cmd, REG_A5XX_RB_CCU_CNTL, 1);
   ta5_cs_push(cmd, 0x10000000);

   /* Bypass: no tile size, render straight to the attachment. */
   ta5_pkt4(cmd, REG_A5XX_RB_CNTL, 1);
   ta5_cs_push(cmd, A5XX_RB_CNTL_WIDTH(0) | A5XX_RB_CNTL_HEIGHT(0) |
                     A5XX_RB_CNTL_BYPASS);

   ta5_pkt4(cmd, REG_A5XX_GRAS_SC_WINDOW_SCISSOR_TL, 2);
   ta5_cs_push(cmd, A5XX_GRAS_SC_WINDOW_SCISSOR_TL_X(0) |
                     A5XX_GRAS_SC_WINDOW_SCISSOR_TL_Y(0));
   ta5_cs_push(cmd, A5XX_GRAS_SC_WINDOW_SCISSOR_BR_X(w - 1) |
                     A5XX_GRAS_SC_WINDOW_SCISSOR_BR_Y(h - 1));

   ta5_pkt4(cmd, REG_A5XX_RB_RESOLVE_CNTL_1, 2);
   ta5_cs_push(cmd, A5XX_RB_RESOLVE_CNTL_1_X(0) | A5XX_RB_RESOLVE_CNTL_1_Y(0));
   ta5_cs_push(cmd, A5XX_RB_RESOLVE_CNTL_2_X(w - 1) |
                     A5XX_RB_RESOLVE_CNTL_2_Y(h - 1));

   ta5_pkt4(cmd, REG_A5XX_RB_WINDOW_OFFSET, 1);
   ta5_cs_push(cmd, A5XX_RB_WINDOW_OFFSET_X(0) | A5XX_RB_WINDOW_OFFSET_Y(0));

   ta5_pkt4(cmd, REG_A5XX_VPC_SO_OVERRIDE, 1);
   ta5_cs_push(cmd, 0);

   /*
    * Tells the VSC to drop visibility, so the draws below are not treated as
    * binning draws.  Without this the draws would be discarded because no bin
    * data exists.
    */
   ta5_pkt7(cmd, CP_SET_VISIBILITY_OVERRIDE, 1);
   ta5_cs_push(cmd, 0x1);

   /* Color attachment 0.  BASE_LO/BASE_HI is a 64 bit reloc. */
   ta5_pkt4(cmd, REG_A5XX_RB_MRT_BUF_INFO(0), 5);
   ta5_cs_push(cmd, A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(RB5_R8G8B8A8_UNORM) |
                     A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(TILE5_LINEAR) |
                     A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(WZYX));
   ta5_cs_push(cmd, A5XX_RB_MRT_PITCH(stride));
   ta5_cs_push(cmd, A5XX_RB_MRT_ARRAY_PITCH(stride * h));
   ret = ta5_cs_reloc(cmd, bo, img->offset, 0);
   if (ret != VK_SUCCESS)
      return ret;

   ta5_pkt4(cmd, REG_A5XX_SP_FS_MRT_REG(0), 1);
   ta5_cs_push(cmd, A5XX_SP_FS_MRT_REG_COLOR_FORMAT(RB5_R8G8B8A8_UNORM));

   ta5_pkt4(cmd, REG_A5XX_RB_MRT_FLAG_BUFFER(0), 4);
   ta5_cs_push(cmd, 0);
   ta5_cs_push(cmd, 0);
   ta5_cs_push(cmd, 0);
   ta5_cs_push(cmd, 0);

   /*
    * The unused colour slots.  A slot is CONTROL, BLEND_CONTROL, BUF_INFO,
    * PITCH, ARRAY_PITCH, BASE_LO, BASE_HI -- seven dwords -- so writing five
    * dwords at RB_MRT(i) reaches BUF_INFO but never the base address.  fd5's
    * emit_mrt() writes RB_MRT_BUF_INFO(i) as its own five-dword packet and
    * zeroes the base of every slot it is not given a buffer for; leaving
    * those two dwords at whatever a previous client configured is how the
    * CCU ends up with a second live target.
    */
   for (uint32_t i = 1; i < 8; i++) {
      ta5_pkt4(cmd, REG_A5XX_RB_MRT_BUF_INFO(i), 5);
      for (uint32_t j = 0; j < 5; j++)
         ta5_cs_push(cmd, 0x00000000);

      ta5_pkt4(cmd, REG_A5XX_SP_FS_MRT(i), 1);
      ta5_cs_push(cmd, 0x00000000);

      ta5_pkt4(cmd, REG_A5XX_RB_MRT_FLAG_BUFFER(i), 4);
      for (uint32_t j = 0; j < 4; j++)
         ta5_cs_push(cmd, 0x00000000);
   }

   /*
    * Enable RT0 before the clear.  The component mask tells the RB which
    * colour targets are live; with it still zero from the state restore the
    * CCU has nowhere to write and the clear is silently dropped.
    */
   ta5_emit_msaa(cmd);
   ta5_emit_draw_state(cmd);

   return VK_SUCCESS;
}

/**
 * The draw state fd5_emit_state() emits before a draw.
 *
 * a5xx needs all of this: without RB_FS_OUTPUT_CNTL/SP_FS_OUTPUT_CNTL the
 * shader cores are not told how many colour outputs to expect, and without
 * the RT0 component mask in RB_RENDER_COMPONENTS nothing is written to the
 * colour pipe at all.  Values are the no-blend, no-depth, one-target case,
 * matching a default VkPipelineColorBlendState with an R8G8B8A8_UNORM target.
 */
static void
ta5_emit_draw_state(struct ta5_cmd_buffer *cmd)
{
   /* fd5_emit_render_cntl(blit = false, binning = false) */
   ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   ta5_pkt4(cmd, REG_A5XX_RB_RENDER_COMPONENTS, 1);
   ta5_cs_push(cmd, A5XX_RB_RENDER_COMPONENTS_RT0(0xf));

   /*
    * Clipper.  GL leaves this zero for a pipeline with no user clip
    * distances, so match that rather than setting a bit the field layout
    * does not define.
    */
   ta5_pkt4(cmd, REG_A5XX_GRAS_CL_CNTL, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_RB_ALPHA_CONTROL, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_RB_STENCIL_CONTROL, 1);
   ta5_cs_push(cmd, 0x00000000);

   /* One colour output, no depth, no sample mask. */
   ta5_pkt4(cmd, REG_A5XX_RB_FS_OUTPUT_CNTL, 1);
   ta5_cs_push(cmd, A5XX_RB_FS_OUTPUT_CNTL_MRT(1));

   ta5_pkt4(cmd, REG_A5XX_SP_FS_OUTPUT_CNTL, 1);
   if (getenv("TA5_FS_NO_DEPTH_OUT"))
      ta5_cs_push(cmd, A5XX_SP_FS_OUTPUT_CNTL_MRT(1));
   else
      ta5_cs_push(cmd, A5XX_SP_FS_OUTPUT_CNTL_MRT(1) |
                        A5XX_SP_FS_OUTPUT_CNTL_DEPTH_REGID(regid(63, 0)) |
                        A5XX_SP_FS_OUTPUT_CNTL_SAMPLEMASK_REGID(regid(63, 0)));

   /*
    * No attachment blends, so both enable masks are zero -- but fd5 still
    * carries the driver's UNK8 bit on the shader side and the full default
    * sample mask on the RB side (fd5_blend.c:110-119, fd5_emit.c:848-850).
    */
   ta5_pkt4(cmd, REG_A5XX_SP_BLEND_CNTL, 1);
   ta5_cs_push(cmd, A5XX_SP_BLEND_CNTL_UNK8);

   ta5_pkt4(cmd, REG_A5XX_RB_BLEND_CNTL, 1);
   ta5_cs_push(cmd, A5XX_RB_BLEND_CNTL_SAMPLE_MASK(0xffff));

   /*
    * Per-attachment ROP.  RB_MRT_CONTROL_COMPONENT_ENABLE is a *mask of colour
    * components the RB may write*, not a list of bound attachments, so leaving
    * it at zero makes every fragment write a no-op: the clear still lands
    * (it goes through RB_CLEAR_CNTL and does not consult this register) but
    * a draw changes nothing.  fd5_blend.c builds the same value from
    * ROP_CODE(ROP_COPY) | COMPONENT_ENABLE(colormask); with no blending and a
    * full colormask that is 0x7e0, which is what GL's stream carries.
    *
    * RB_MRT_CONTROL(i) is strided by 7 registers, so each slot is a separate
    * one-dword write -- a multi-dword T4 here would spill into the unrelated
    * registers in between.
    */
   for (int i = 0; i < 8; i++) {
      ta5_pkt4(cmd, REG_A5XX_RB_MRT_CONTROL(i), 1);
      ta5_cs_push(cmd, A5XX_RB_MRT_CONTROL_ROP_CODE(ROP_COPY) |
                        A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(0xf) |
                        (getenv("TA5_ROP_ENABLE")
                             ? A5XX_RB_MRT_CONTROL_ROP_ENABLE
                             : 0));

      ta5_pkt4(cmd, REG_A5XX_RB_MRT_BLEND_CONTROL(i), 1);
      ta5_cs_push(cmd, 0x00000000);
   }

   /* No textures bound in any stage. */
   ta5_pkt4(cmd, REG_A5XX_TPL1_VS_TEX_COUNT, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_TPL1_FS_TEX_COUNT, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_TPL1_CS_TEX_COUNT, 1);
   ta5_cs_push(cmd, 0x00000000);

   /*
    * No depth or stencil attachment.  fd5_emit_zs() programs the depth pipe
    * explicitly rather than leaving it alone, and that distinction matters:
    * clearing RB_DEPTH_CNTL only switches depth testing off, it does not say
    * what the depth format is or where the depth buffer lives.  Both of those
    * are register state that outlives a submission, so leaving them stale lets
    * the CCU keep a Z-fetch live against whatever base the previous client
    * left behind -- which is a pair of unmapped reads, one per fetch, at the
    * moment the first fragments arrive.
    *
    * Transcribed from fd5_gmem.c:198-216, the no-zsbuf branch.
    */
   ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
   ta5_cs_push(cmd, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH5_NONE));
   ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_BUFFER_BASE_LO */
   ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_BUFFER_BASE_HI */
   ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_BUFFER_PITCH */
   ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_BUFFER_ARRAY_PITCH */

   ta5_pkt4(cmd, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
   ta5_cs_push(cmd, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH5_NONE));

   ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
   ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_FLAG_BUFFER_BASE */
   ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_FLAG_BUFFER_BASE_HI */
   ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_FLAG_BUFFER_PITCH */

   ta5_pkt4(cmd, REG_A5XX_RB_STENCIL_INFO, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_CNTL, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_PLANE_CNTL, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_GRAS_SU_DEPTH_PLANE_CNTL, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_RB_STENCILREFMASK, 2);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   /* Rasteriser: filled triangles, width 1, no polygon offset, no culling.
    * FRONT_CW is what GL's rasteriser state carries on this part.
    */
   ta5_pkt4(cmd, REG_A5XX_GRAS_SU_CNTL, 1);
   ta5_cs_push(cmd, A5XX_GRAS_SU_CNTL_FRONT_CW |
                     A5XX_GRAS_SU_CNTL_LINEHALFWIDTH(0.5f));

   ta5_pkt4(cmd, REG_A5XX_GRAS_SU_POINT_MINMAX, 2);
   ta5_cs_push(cmd, A5XX_GRAS_SU_POINT_MINMAX_MIN(1.0f) |
                     A5XX_GRAS_SU_POINT_MINMAX_MAX(1.0f));
   ta5_cs_push(cmd, A5XX_GRAS_SU_POINT_SIZE(1.0f));

   ta5_pkt4(cmd, REG_A5XX_GRAS_SU_POLY_OFFSET_SCALE, 3);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   /* Both PTYPE fields select triangles for a filled primitive; PTYPE 0 is
    * points, so a zero here quietly turns the triangle into three dots. */
   ta5_pkt4(cmd, REG_A5XX_PC_RASTER_CNTL, 1);
   ta5_cs_push(cmd, A5XX_PC_RASTER_CNTL_POLYMODE_FRONT_PTYPE(
                      PC_DRAW_TRIANGLES) |
                     A5XX_PC_RASTER_CNTL_POLYMODE_BACK_PTYPE(
                        PC_DRAW_TRIANGLES));

   /* Depth range: Vulkan's default is 0..1 with clip_halfz off, which makes
    * the clamped range -1..1. */
   ta5_pkt4(cmd, REG_A5XX_GRAS_CL_VIEWPORT_ZCLAMP_NEAR_0, 2);
   ta5_cs_push(cmd, A5XX_GRAS_CL_VIEWPORT_ZCLAMP_NEAR_0(-1.0f));
   ta5_cs_push(cmd, A5XX_GRAS_CL_VIEWPORT_ZCLAMP_FAR_0(1.0f));

   ta5_pkt4(cmd, REG_A5XX_RB_VIEWPORT_ZCLAMP_NEAR, 2);
   ta5_cs_push(cmd, A5XX_RB_VIEWPORT_ZCLAMP_NEAR(-1.0f));
   ta5_cs_push(cmd, A5XX_RB_VIEWPORT_ZCLAMP_FAR(1.0f));

   /* Blend colour, even with blending disabled. */
   ta5_pkt4(cmd, REG_A5XX_RB_BLEND_RED, 8);
   for (int i = 0; i < 8; i++)
      ta5_cs_push(cmd, 0x00000000);
}

/** Clear every colour attachment. */
static VkResult
ta5_emit_clear(struct ta5_cmd_buffer *cmd, struct ta5_device *device,
               const union ta5_color *color)
{
   VkResult ret;

   /*
    * Switch the RB into blit mode for the duration of the clear.  This is
    * fd5_emit_render_cntl(ctx, blit = true, binning = false): only
    * RB_RENDER_CNTL loses its 0x8 bit -- GRAS_SC_CNTL keeps it for a blit,
    * exactly as it does for a normal render pass.  Clearing it here takes the
    * screen controller out of the loop and the CCU never resolves the clear.
    */
   ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   ta5_pkt4(cmd, REG_A5XX_RB_BLIT_CNTL, 1);
   ta5_cs_push(cmd, A5XX_RB_BLIT_CNTL_BUF(BLIT_MRT0));

   ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_CNTL, 1);
   ta5_cs_push(cmd, A5XX_RB_CLEAR_CNTL_FAST_CLEAR | A5XX_RB_CLEAR_CNTL_MASK(0xf));

   /*
    * RB_CLEAR_COLOR_DW0..3 is one packed dword followed by three reserved
    * ones, not four channel dwords.  The byte order is BGRA, which is only
    * visible from GL's stream: its clear of rgba(0,0,0,1) lands as
    * RB_CLEAR_COLOR_DW0 = 0xff000000, i.e. alpha in the high byte.  Writing
    * the channels as separate dwords clears to (r,0,0,0) instead.
    */
   ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
   ta5_cs_push(cmd, (color->u32[3] << 24) | (color->u32[0] << 16) |
                     (color->u32[1] << 8) | color->u32[2]);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   if (device->blit_bo == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   ret = ta5_event_write_ts(cmd, device->blit_bo, CCU_RESOLVE);
   if (ret != VK_SUCCESS)
      return ret;

   ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_CNTL, 1);
   ta5_cs_push(cmd, 0);

   /* Back to rendering, not blitting. */
   ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   return VK_SUCCESS;
}

/** End of the render pass: flush colour out of the CCU. */
static VkResult
ta5_emit_sysmem_fini(struct ta5_cmd_buffer *cmd)
{
   VkResult ret;

   ta5_pkt7(cmd, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
   ta5_cs_push(cmd, 0);

   /* fd5_emit_lrz_flush(): the trailing disable is part of the sequence. */
   ta5_pkt4(cmd, REG_A5XX_GRAS_LRZ_CNTL, 1);
   ta5_cs_push(cmd, A5XX_GRAS_LRZ_CNTL_ENABLE);
   ta5_event_write(cmd, LRZ_FLUSH_INVALIDATE);
   ta5_pkt4(cmd, REG_A5XX_GRAS_LRZ_CNTL, 1);
   ta5_cs_push(cmd, 0x0);

   /*
    * Both CCU flushes are timestamped.  The timestamp destination is the
    * device-wide 16-byte blit scratch buffer, never a render target: freedreno
    * always points fd5_event_write()'s relocation at blit_mem, and the
    * timestamp's job is to make the CP report when the flush has *completed*
    * so the wait is meaningful.  Handing it an attachment instead asks the CCU
    * to flush on behalf of a buffer nothing is waiting on, and the packet
    * faults -- it is the only packet left that does.
    */
   ret = ta5_event_write_ts(cmd, cmd->device->blit_bo,
                            PC_CCU_FLUSH_COLOR_TS);
   if (ret != VK_SUCCESS)
      return ret;

   ret = ta5_event_write_ts(cmd, cmd->device->blit_bo,
                            PC_CCU_FLUSH_DEPTH_TS);
   if (ret != VK_SUCCESS)
      return ret;

   /*
    * CACHE_FLUSH_TS closes the pass, again into blit_mem.  GL's sysmem pass
    * emits this with a data word of 1; emitting it with 0, or aimed at an
    * attachment instead of the scratch buffer, is what made it fault when it
    * was first added.
    */
   ta5_pkt7(cmd, CP_EVENT_WRITE, 4);
   ta5_cs_push(cmd, CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS));
   ret = ta5_cs_reloc(cmd, cmd->device->blit_bo, 0, 0);
   if (ret != VK_SUCCESS)
      return ret;
   ta5_cs_push(cmd, 0x00000001);
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Vulkan command entry points                                         */
/* ------------------------------------------------------------------ */

VKAPI_ATTR void VKAPI_CALL
ta5_CmdBindPipeline(VkCommandBuffer _cmd, VkPipelineBindPoint pipelineBindPoint,
                    VkPipeline _pipeline)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   (void)pipelineBindPoint;

   if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) {
      cmd->bound_pipeline = NULL;
      return;
   }
   cmd->bound_pipeline = ta5_pipeline_from_handle(_pipeline);
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdSetViewport(VkCommandBuffer _cmd, uint32_t firstViewport,
                   uint32_t viewportCount, const VkViewport *pViewports)
{
   (void)_cmd;
   (void)firstViewport;
   (void)viewportCount;
   (void)pViewports;
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdSetScissor(VkCommandBuffer _cmd, uint32_t firstScissor,
                  uint32_t scissorCount, const VkRect2D *pScissors)
{
   (void)_cmd;
   (void)firstScissor;
   (void)scissorCount;
   (void)pScissors;
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdBindDescriptorSets(VkCommandBuffer _cmd,
                          VkPipelineBindPoint pipelineBindPoint,
                          VkPipelineLayout _layout, uint32_t firstSet,
                          uint32_t descriptorSetCount,
                          const VkDescriptorSet *pDescriptorSets,
                          uint32_t dynamicOffsetCount,
                          const uint32_t *pDynamicOffsets)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);
   uint32_t i;

   (void)_layout;
   (void)pipelineBindPoint;
   (void)dynamicOffsetCount;
   (void)pDynamicOffsets;

   if (firstSet + descriptorSetCount > ARRAY_SIZE(cmd->bound_sets))
      return;

   for (i = 0; i < descriptorSetCount; i++) {
      cmd->bound_sets[firstSet + i] =
         ta5_descriptor_set_from_handle(pDescriptorSets[i]);
   }
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdBeginRenderPass(VkCommandBuffer _cmd,
                       const VkRenderPassBeginInfo *pRenderPassBegin,
                       VkSubpassContents contents)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);
   struct ta5_render_pass *pass =
      ta5_render_pass_from_handle(pRenderPassBegin->renderPass);
   const VkClearValue *clear = pRenderPassBegin->pClearValues;
   union ta5_color color;
   VkResult ret;

   (void)contents;

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   cmd->bound_render_pass = pass;
   cmd->bound_framebuffer =
      ta5_framebuffer_from_handle(pRenderPassBegin->framebuffer);

   if (clear)
      color = ta5_color_from_clear(clear[0].color);

   /* Make room for the worst case before emitting, so the grow cannot move
    * the stream underneath a relocation we have already recorded. */
   ret = ta5_cmd_buffer_alloc_cs(cmd, cmd->cs_count + 4096);
   if (ret != VK_SUCCESS)
      return;

   ret = ta5_emit_sysmem_prep(cmd, cmd->bound_framebuffer);
   if (ret != VK_SUCCESS)
      return;

   if (pass->attachments[0].load_op == VK_ATTACHMENT_LOAD_OP_CLEAR &&
       ta5_emit_clear(cmd, cmd->device, &color) != VK_SUCCESS)
      return;
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdEndRenderPass(VkCommandBuffer _cmd)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   /*
    * Grow before emitting.  ta5_cs_push() writes unconditionally, and the
    * only two sites that reserved space were BeginRenderPass and the draw
    * path, so the CCU flushes and the trailing CACHE_FLUSH_TS this emits
    * were being written into whatever the draw path happened to leave
    * spare.  That worked only because BeginRenderPass happens to run first
    * in a frame; it is an accident of ordering, not a bound.
    */
   if (ta5_cmd_buffer_alloc_cs(cmd, cmd->cs_count + 1024) != VK_SUCCESS)
      return;

   if (ta5_emit_sysmem_fini(cmd) != VK_SUCCESS)
      return;
   ta5_wfi(cmd);

   /*
    * Truncate to the first N dwords, for finding which packet stops the CP.
    * Relocations past the cut go with it -- leaving one in would make the
    * kernel patch outside the submitted range and reject the submit.
    */
   const char *maxw = getenv("TA5_MAX_DWORDS");
   if (maxw) {
      uint32_t want = (uint32_t)atoi(maxw);
      uint32_t n = 0, i = 0;

      /*
       * Snap to a packet boundary.  Cutting mid-packet leaves the count field
       * describing more dwords than actually follow, which faults the CP on
       * its own and makes every prefix look broken -- which is exactly what
       * happened the first time this was tried.
       */
      while (i < cmd->cs_count && n < want) {
         uint32_t hdr = cmd->cs[i];
         uint32_t total;

         if ((hdr & 0xc0000000) == 0xc0000000) {
            /* type 3 */
            total = ((hdr >> 16) & 0x3ff) + 2;
         } else if (((hdr >> 30) & 3) == 1 && ((hdr >> 28) & 3) == 3) {
            /* type 7: count field is 15 bits */
            total = 1 + (hdr & 0x7fff);
         } else if (((hdr >> 30) & 3) == 1) {
            /* type 4: count field is 7 bits */
            total = 1 + (hdr & 0x7f);
         } else {
            total = 1;
         }

         if (n + total > want)
            break;
         n += total;
         i += total;
      }

      while (cmd->reloc_count && cmd->relocs[cmd->reloc_count - 1].dst_off >= n * 4)
         cmd->reloc_count--;
      if (n < cmd->cs_count)
         cmd->cs_count = n;
   }

   {
      const char *dump = getenv("TA5_CS_DUMP");
      if (dump) {
         FILE *f = fopen(dump, "wb");
         if (f) {
            fwrite(cmd->cs, 4, cmd->cs_count, f);
            fclose(f);
         }
      }
   }

   if (getenv("TA5_TRACE_NULL")) {
      fprintf(stderr, "ta5[cs] %u dwords:\n", cmd->cs_count);
      for (uint32_t i = 0; i < cmd->cs_count; i++) {
         uint32_t w = cmd->cs[i];
         /* type 7 has bits[31:30]=01 and bits[29:28]=11;
          * type 4 has bits[31:30]=01 with the register index in [27:8]. */
         uint32_t kind = (((w >> 30) & 3) == 1 && ((w >> 28) & 3) == 3) ? 3 : 1;
         const char *k = kind == 3 ? "T7" : kind == 1 ? "T4" : "??";
         if (kind == 3)
            fprintf(stderr, "  [%3u] %08x  T7 op=0x%02x cnt=%u\n", i, w,
                    (w >> 16) & 0x7f, w & 0x7fff);
         else if (kind == 1)
            fprintf(stderr, "  [%3u] %08x  T4 reg=0x%05x cnt=%u\n", i, w,
                    (w >> 8) & 0x3ffff, w & 0x7f);
         else
            fprintf(stderr, "  [%3u] %08x  %s\n", i, w, k);
      }
   }

   cmd->bound_render_pass = NULL;
   cmd->bound_framebuffer = NULL;
}

/*
 * vkCmdPipelineBarrier.
 *
 * This is a Vulkan 1.0 core command with no feature gating it, so its absence
 * is not something the feature table can hide: without it every application
 * that orders a copy against a draw gets an error from the loader's trampoline
 * instead of a barrier, because the entrypoint is declared weak and the
 * dispatch slot is NULL.
 *
 * The implementation is deliberately the blunt one: a full CP_WAIT_FOR_IDLE
 * wherever a real dependency could exist.  That is always legal --
 * over-synchronising is permitted, only under-synchronising is a bug -- and it
 * is the only thing that is unambiguously correct without a CCU/ROP cache
 * invalidation pass, which is what a real implementation would do instead for
 * the shader-write-to-later-read cases this hardware needs it for.
 *
 * The two stage masks are ignored.  Narrowing them means reasoning about
 * which stage each bit corresponds to on a5xx and which of those the CCU and
 * the SP actually order against, and getting that wrong produces a stream that
 * passes validation and corrupts a render.  One WFI cannot.
 */
VKAPI_ATTR void VKAPI_CALL
ta5_CmdPipelineBarrier(VkCommandBuffer _cmd, VkPipelineStageFlags srcStageMask,
                       VkPipelineStageFlags dstStageMask,
                       VkDependencyFlags dependencyFlags,
                       uint32_t memoryBarrierCount,
                       const VkMemoryBarrier *pMemoryBarriers,
                       uint32_t bufferMemoryBarrierCount,
                       const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                       uint32_t imageMemoryBarrierCount,
                       const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);
   VkResult ret;

   /*
    * The barrier ranges describe what the dependency is *for*; the stream
    * does not change with them.  Read them so they cannot be optimised away,
    * and so the unused-parameter warnings stay off.
    */
   (void)dependencyFlags;
   (void)pMemoryBarriers;
   (void)pBufferMemoryBarriers;
   (void)pImageMemoryBarriers;
   (void)memoryBarrierCount;
   (void)bufferMemoryBarrierCount;
   (void)imageMemoryBarrierCount;

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   /*
    * VK_PIPELINE_STAGE_NONE on either side means no dependency is being
    * expressed and no work is required, which is the common case for the
    * layout-transition barriers that surround a copy.
    */
   if (srcStageMask == VK_PIPELINE_STAGE_NONE ||
       dstStageMask == VK_PIPELINE_STAGE_NONE)
      return;

   ret = ta5_cmd_buffer_alloc_cs(cmd, cmd->cs_count + 8);
   if (ret != VK_SUCCESS)
      return;

   ta5_wfi(cmd);
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdDraw(VkCommandBuffer _cmd, uint32_t vertexCount, uint32_t instanceCount,
            uint32_t firstVertex, uint32_t firstInstance)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);
   struct ta5_pipeline *pipe = cmd->bound_pipeline;
   VkResult ret;

   /*
    * Driver parameters are recorded per draw, so remember what this one is
    * before anything emits the shader.
    */
   cmd->draw_first_vertex = firstVertex;
   cmd->draw_first_instance = firstInstance;
   cmd->draw_index_size = 0;   /* AUTO_INDEX; no index buffer is bound */

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING || pipe == NULL)
      return;

   if (getenv("TA5_SKIP_DRAW"))
      return;

   ret = ta5_cmd_buffer_alloc_cs(cmd, cmd->cs_count + 8192);
   if (ret != VK_SUCCESS)
      return;

   /*
    * TA5_CLEAR_IN_DRAW emits a second clear here, at the top of the draw
    * path.  It is a probe, not a feature: if this clear lands then the CP
    * reached the draw and whatever is wrong is in the draw itself, and if it
    * does not then the CP never got this far.
    */
   if (getenv("TA5_CLEAR_IN_DRAW")) {
      union ta5_color probe = {{0}};
      probe.u32[0] = 0xff;
      probe.u32[1] = 0x00;
      probe.u32[2] = 0xff;
      probe.u32[3] = 0xff;
      ta5_emit_clear(cmd, cmd->device, &probe);
   }

   /*
    * Viewport transform.  The vertex shader produces NDC, so this is the
    * width/height-to-half-size mapping *plus* the half-size offset that puts
    * NDC -1 at pixel 0.  Without the offset the whole frame lands off the
    * left and bottom edges and the draw rasterises nothing.  GL's stream
    * carries 32/32 offsets and 0.5/0.5 for z on a 64x64 target.
    */
   {
      uint32_t w = cmd->bound_framebuffer->width;
      uint32_t h = cmd->bound_framebuffer->height;

      ta5_pkt4(cmd, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0, 2);
      ta5_cs_push(cmd, A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(0) |
                        A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(0));
      ta5_cs_push(cmd, A5XX_GRAS_SC_SCREEN_SCISSOR_BR_0_X(w - 1) |
                        A5XX_GRAS_SC_SCREEN_SCISSOR_BR_0_Y(h - 1));

      ta5_pkt4(cmd, REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0, 2);
      ta5_cs_push(cmd, A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(0) |
                        A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(0));
      ta5_cs_push(cmd, A5XX_GRAS_SC_VIEWPORT_SCISSOR_BR_0_X(w - 1) |
                        A5XX_GRAS_SC_VIEWPORT_SCISSOR_BR_0_Y(h - 1));
   }

   ta5_pkt4(cmd, REG_A5XX_GRAS_CL_VPORT_XOFFSET_0, 6);
   ta5_cs_push(cmd, A5XX_GRAS_CL_VPORT_XOFFSET_0(cmd->bound_framebuffer->width
                                                 / 2.0f));
   ta5_cs_push(cmd, A5XX_GRAS_CL_VPORT_XSCALE_0(cmd->bound_framebuffer->width
                                                / 2.0f));
   ta5_cs_push(cmd, A5XX_GRAS_CL_VPORT_YOFFSET_0(cmd->bound_framebuffer->height
                                                 / 2.0f));
   ta5_cs_push(cmd, A5XX_GRAS_CL_VPORT_YSCALE_0(cmd->bound_framebuffer->height
                                                / 2.0f));
   ta5_cs_push(cmd, A5XX_GRAS_CL_VPORT_ZOFFSET_0(0.5f));
   ta5_cs_push(cmd, A5XX_GRAS_CL_VPORT_ZSCALE_0(0.5f));

   ta5_emit_program_state(cmd, pipe);

   /*
    * Both programs are loaded before either stage's constants, which is the
    * order fd5 emits them in (fd5_program.c: the SP_xS_OBJ_START and program
    * loads for both stages come first, and the constant and driver-parameter
    * uploads follow).  Emitting each stage's constants straight after its own
    * program load instead reorders them, and the fragment stage faults on
    * the very first shaded fragment -- see the plan doc.
    */
   ret = ta5_emit_shader(cmd, pipe, &pipe->vertex, SB4_VS_SHADER,
                         REG_A5XX_SP_VS_OBJ_START, pipe->vs_bo);
   if (ret != VK_SUCCESS)
      return;
   if (pipe->has_fragment) {
      ret = ta5_emit_shader(cmd, pipe, &pipe->fragment, SB4_FS_SHADER,
                            REG_A5XX_SP_FS_OBJ_START, pipe->fs_bo);
      if (ret != VK_SUCCESS)
         return;
   }

   ta5_emit_consts(cmd, pipe, &pipe->vertex, SB4_VS_SHADER, pipe->vs_bo);
   if (pipe->has_fragment)
      ta5_emit_consts(cmd, pipe, &pipe->fragment, SB4_FS_SHADER, pipe->fs_bo);

   /*
    * The prep enables LRZ and fd5 leaves it enabled across the draw, turning
    * it off only in the fini.  That has never been varied here, and LRZ gates
    * the depth path, so TA5_LRZ_OFF turns it off before the draw to see
    * whether it is what is swallowing the fragments.
    *
    * The fini disables LRZ only after an LRZ_FLUSH_INVALIDATE; this does not.
    * That is sound here because ta5_emit_draw_state programs DEPTH5_NONE and
    * RB_DEPTH_CNTL = 0, so the LRZ holds nothing to invalidate.  It would not
    * be on a pipeline with a depth attachment, which is why this stays a probe
    * and not a general "disable LRZ" path.
    */
   if (getenv("TA5_LRZ_OFF")) {
      ta5_pkt4(cmd, REG_A5XX_GRAS_LRZ_CNTL, 1);
      ta5_cs_push(cmd, 0x00000000);
   }

   /*
    * GL's sysmem pass emits the draw state, the program state and both
    * shader loads twice: once in the top-level stream and again inside the
    * buffer the draw is reached through.  TA5_DOUBLE_STATE repeats the
    * second emission immediately before the draw, to see whether the draw
    * needs the state re-armed that recently.
    *
    * ta5_emit_shader() is checked here for the same reason it is checked above:
    * it can fail in ta5_cs_reloc(), and continuing past that would emit the
    * draw packet against a half-loaded program.
    */
   if (getenv("TA5_DOUBLE_STATE")) {
      ret = ta5_cmd_buffer_alloc_cs(cmd, cmd->cs_count + 4096);
      if (ret != VK_SUCCESS)
         return;
      ta5_emit_draw_state(cmd);
      ta5_emit_program_state(cmd, pipe);
      ret = ta5_emit_shader(cmd, pipe, &pipe->vertex, SB4_VS_SHADER,
                            REG_A5XX_SP_VS_OBJ_START, pipe->vs_bo);
      if (ret != VK_SUCCESS)
         return;
      if (pipe->has_fragment) {
         ret = ta5_emit_shader(cmd, pipe, &pipe->fragment, SB4_FS_SHADER,
                               REG_A5XX_SP_FS_OBJ_START, pipe->fs_bo);
         if (ret != VK_SUCCESS)
            return;
      }
      ta5_emit_consts(cmd, pipe, &pipe->vertex, SB4_VS_SHADER, pipe->vs_bo);
      if (pipe->has_fragment)
         ta5_emit_consts(cmd, pipe, &pipe->fragment, SB4_FS_SHADER, pipe->fs_bo);
   }

   /*
    * CP_DRAW_INDX_OFFSET, no index buffer.  AUTO_INDEX synthesises vertex ids
    * 0..count-1, which is exactly what gl_VertexIndex reads.  With no index
    * buffer fd5 still programs INDEX_SIZE as 32-bit (fd5_draw.h:114) rather
    * than leaving it at the 8-bit reset value.
    */
   ta5_pkt7(cmd, CP_DRAW_INDX_OFFSET, 3);
   ta5_cs_push(cmd, CP_DRAW_INDX_OFFSET_0_PRIM_TYPE(DI_PT_TRILIST) |
                     CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(DI_SRC_SEL_AUTO_INDEX) |
                     CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(INDEX4_SIZE_32_BIT) |
                     CP_DRAW_INDX_OFFSET_0_VIS_CULL(IGNORE_VISIBILITY));
   ta5_cs_push(cmd, instanceCount);
   ta5_cs_push(cmd, vertexCount);

   /*
    * Stage the draw into its own object and reach it with CP_INDIRECT_BUFFER,
    * which is what fd5 does (freedreno_util.h __OUT_IB5) and the one packet
    * its sysmem stream contains that an inline draw does not.
    *
    * Only the draw packet moves.  The state and the shader loads stay in the
    * main stream, because they carry relocations and the kernel's patching
    * only reaches the submitted command buffer -- so this is not yet the
    * whole of GL's arrangement, but it isolates the one packet.
    */
   if (cmd->device->draw_bo && !getenv("TA5_NO_INDIRECT_DRAW")) {
      uint32_t off = cmd->cs_count - 4;
      void *map = ta5_bo_map(cmd->device->draw_bo);

      if (map != NULL) {
         memcpy(map, &cmd->cs[off], 4 * sizeof(uint32_t));
         __sync_synchronize();

         cmd->cs_count = off;
         ta5_pkt7(cmd, CP_INDIRECT_BUFFER, 3);
         ret = ta5_cs_reloc(cmd, cmd->device->draw_bo, 0, 0);
         if (ret != VK_SUCCESS)
            return;
         ta5_cs_push(cmd, 4); /* IB_SIZE, in dwords */

         ta5_pkt7(cmd, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
         ta5_cs_push(cmd, 0);
      }
   }
}
