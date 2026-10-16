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
#include "ta5_submit.h"
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

static void
ta5_cmd_buffer_clear_ibs(struct ta5_cmd_buffer *cmd)
{
   for (uint32_t i = 0; i < cmd->ib_count; i++) {
      struct ta5_ib *ib = &cmd->ibs[i];

      ta5_bo_unref(ib->bo);
      vk_free(&cmd->device->vk.alloc, ib->relocs);
      vk_free(&cmd->device->vk.alloc, ib->reloc_bos);
      memset(ib, 0, sizeof(*ib));
   }

   cmd->ib_count = 0;
}

/**
 * Stage a recorded command stream as a relocatable MSM IB target.
 *
 * The caller owns src's CPU stream and reloc arrays.  On success the BO and
 * reloc arrays move into cmd->ibs; on failure src remains untouched.
 */
static VkResult
ta5_cmd_buffer_add_ib(struct ta5_cmd_buffer *cmd,
                      struct ta5_cmd_buffer *src)
{
   struct ta5_bo *bo;
   struct ta5_ib *ibs;
   uint32_t bytes = src->cs_count * sizeof(uint32_t);
   uint32_t bo_size = align(bytes, 4096);
   void *map;

   if (cmd->ib_count == cmd->ib_size) {
      uint32_t new_size = cmd->ib_size ? cmd->ib_size * 2 : 2;
      ibs = (struct ta5_ib *)vk_realloc(&cmd->device->vk.alloc, cmd->ibs,
                                         new_size * sizeof(*ibs), 8,
                                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!ibs)
         return vk_error(&cmd->device->instance->vk,
                         VK_ERROR_OUT_OF_HOST_MEMORY);
      cmd->ibs = ibs;
      cmd->ib_size = new_size;
   }

   bo = ta5_bo_alloc(cmd->device, bo_size, MSM_BO_WC);
   if (!bo)
      return vk_error(&cmd->device->instance->vk,
                      VK_ERROR_OUT_OF_DEVICE_MEMORY);

   map = ta5_bo_map(bo);
   if (!map) {
      ta5_bo_unref(bo);
      return vk_error(&cmd->device->instance->vk,
                      VK_ERROR_MEMORY_MAP_FAILED);
   }

   memcpy(map, src->cs, bytes);
   __sync_synchronize();

   struct ta5_ib *ib = &cmd->ibs[cmd->ib_count++];
   memset(ib, 0, sizeof(*ib));
   ib->bo = bo;
   ib->size = src->cs_count;
   ib->relocs = src->relocs;
   ib->reloc_count = src->reloc_count;
   ib->reloc_bos = src->reloc_bos;
   ib->reloc_bo_count = src->reloc_bo_count;

   src->relocs = NULL;
   src->reloc_count = src->reloc_size = 0;
   src->reloc_bos = NULL;
   src->reloc_bo_count = 0;

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
      ta5_cmd_buffer_clear_ibs(cmd);
      vk_free(&cmd->device->vk.alloc, cmd->ibs);
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
   ta5_cmd_buffer_clear_ibs(cmd);
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
   ta5_cmd_buffer_clear_ibs(cmd);
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
#include "fd5_hw.h"          /* approved wrapper for a5xx.xml.h */

#include "ir3/ir3_shader.h"
#include "drm/msm_drm.h"
#include "ta5_render.h"
#include "ta5_pipeline.h"
#include "ta5_descriptor.h"
#include "ta5_sampler.h"
#include "ta5_formats.h"
#include "ta5_texel.h"
#include "ta5_memory.h"
#include "util/format/format_utils.h"

/** Clear colour, packed the way RB_CLEAR_COLOR_DW0..3 wants it. */
union ta5_color
{
   float f32[4];
   uint32_t u32[4];
};

/**
 * Linear -> sRGB, the curve Vulkan's clear color arrives in.  The clear path
 * writes raw dwords through RB_BLIT, which does not run the fixed-function
 * COLOR_SRGB converter, so an sRGB attachment gets the encode applied here --
 * the same thing util_pack_color() does for GL's blit clears on sRGB formats.
 */
static float
ta5_linear_to_srgb(float v)
{
   if (v <= 0.0f)
      return 0.0f;
   if (v >= 1.0f)
      return 1.0f;
   return v < 0.0031308f ? v * 12.92f
                         : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

/** Convert a VkClearColorValue into RB_CLEAR_COLOR_DW0..3 order. */
static union ta5_color
ta5_color_from_clear(VkClearColorValue c, bool srgb)
{
   union ta5_color out;

   /* The RB wants RGBA in dword 0..3 for an RGBA8 target. */
   for (uint32_t i = 0; i < 4; i++) {
      float v = srgb ? ta5_linear_to_srgb(c.float32[i]) : c.float32[i];
      out.u32[i] = _mesa_float_to_unorm(v, 8);
   }
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

   /*
    * The OR mask goes in the *high* dword, not the low one.
    *
    * freedreno's OUT_RELOC takes a 64-bit mask and splits it: it shifts the
    * high half onto the high dword of the relocated pair.  fd5 relies on that
    * for the texel descriptor, where TEX_CONST_5 shares a dword with the top
    * half of the image address:
    *
    *     OUT_RELOC(ring, rsc->bo, view->offset,
    *               (uint64_t)view->texconst5 << 32, 0);
    *
    * Here the same thing is done with a 32-bit mask attached to the shifted
    * relocation instead.  Putting it on the low dword instead would OR
    * TEX_CONST_5 into BASE_LO and leave BASE_HI's own bits empty, which moves
    * the image base by however much DEPTH happens to be -- for a cube, 128 KB
    * -- and the fetch then lands outside the allocation.
    */
   cmd->cs[cmd->cs_count++] = 0;
   cmd->cs[cmd->cs_count++] = or_mask;

   cmd->relocs[cmd->reloc_count].bo_index = i;
   cmd->relocs[cmd->reloc_count].bo_offset = bo_offset;
   cmd->relocs[cmd->reloc_count].dst_off = (cmd->cs_count - 2) * 4;
   cmd->relocs[cmd->reloc_count].or_mask = 0;
   cmd->relocs[cmd->reloc_count].shift = 0;
   cmd->reloc_count++;

   cmd->relocs[cmd->reloc_count].bo_index = i;
   cmd->relocs[cmd->reloc_count].bo_offset = bo_offset;
   cmd->relocs[cmd->reloc_count].dst_off = (cmd->cs_count - 1) * 4;
   cmd->relocs[cmd->reloc_count].or_mask = or_mask;   /* or_mask >> 32 */
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
   /* TA5_TRACE_PKT=reg logs every register write this draw programs, in
    * order.  Two runs of the same program differ in one object; comparing the
    * two logs names what changed without decoding PM4 by hand. */
   const char *tpk = getenv("TA5_TRACE_PKT");

   if (tpk && (!strcmp(tpk, "all") || (uint32_t)strtoul(tpk, NULL, 16) == reg))
      fprintf(stderr, "pkt4 reg=%05x cnt=%u @cs%u\n", reg, cnt, cmd->cs_count);
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
   const char *tpk = getenv("TA5_TRACE_PKT");

   if (tpk && (!strcmp(tpk, "all") || (uint32_t)strtoul(tpk, NULL, 16) == opcode))
      fprintf(stderr, "pkt7 op=%02x cnt=%u @cs%u\n", opcode, cnt,
              cmd->cs_count);
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

   /*
    * TA5_WFI_EVENT waits for idle immediately before the event.  GL puts its
    * fd_wfi() before the CCU reconfiguration instead (TA5_WFI_CCU, measured,
    * no effect), which is the wrong place for the other half of this theory:
    * the CCU is GPU-global, so a resolve issued while the compositor's CCU
    * work is still in flight flushes a cache that is mid-update.  That would
    * also explain why three concurrent *Vulkan* clients never fault and one GL
    * compositor always does.
    */
   if (getenv("TA5_WFI_EVENT"))
      ta5_wfi(cmd);

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
 * fd5_program.c:47 passes v->instrlen straight to CP_LOAD_STATE4_0_NUM_UNIT.
 * That is the length: instrlen, and nothing derived from info.sizedwords.
 *
 * This used to compute
 *
 *     MAX2(instrlen, DIV_ROUND_UP(info.sizedwords, 32))
 *
 * on the belief -- asserted above until now -- that info.sizedwords is always
 * instrlen * 32.  It is not.  ir3.c:400 is
 *
 *     info->size = MAX2(instrlen * instr_align, instr_count + 4) * 8;
 *
 * and the second term is *dump padding*: four NOPs so cffdump does not decode
 * whatever follows the program as instructions.  Whenever instr_count exceeds
 * 16 * instrlen - 4 -- the last four counts of *every* unit -- instr_count + 4
 * wins, sizedwords stops being a multiple of 32, and DIV_ROUND_UP rounds it to
 * one unit more than the program occupies.  For a shader of 13 to 16
 * instructions instrlen is 1, but instr_count + 4 is 17 to 20, so sizedwords
 * comes out 34, 36 or 40 rather than 32, and the program is loaded 2 units long
 * against 1.  The same thing happens at 29 to 32 instructions with 3 units
 * instead of 2, and at 45 to 48 with 4 instead of 3.
 *
 * The instrlen-only formula below is correct at every one of those, so this is
 * not a code change; it is recorded because an earlier version of this comment
 * named only the 13-to-16 window, and a reader taking it at face value would
 * conclude that a 32-instruction shader was immune.  The 88-case matrix happened
 * to contain no shader outside that window, which is why the measured failure
 * set below is all instrlen == 1.
 *
 * That reads past the end of the shader BO, which ta5_variant_size() sizes at
 * info.sizedwords * 4 bytes: 256 bytes asked for against 136 or 160 allocated,
 * so 96 to 120 bytes of whatever follows.  The draw produces no pixels at all
 * -- sometimes with a gpu fault, sometimes silently.
 *
 * Measured over the 88-case matrix, DIV_ROUND_UP(sizedwords, 32) != instrlen
 * for exactly 8 cases, and they are exactly the 8 that produce no output:
 *
 *     case        sizedwords  instrlen  units   result
 *     fs:3           34          1        2    no output
 *     n3to2d:0       34          1        2    no output
 *     grad:27        34          1        2    no output
 *     n3to2d:1       36          1        2    no output
 *     lensplit:0     36          1        2    no output
 *     grad:24        36          1        2    no output
 *     grad:26        36          1        2    no output
 *     lad:3          40          1        2    no output
 *     all 80 others  32 or 64    1 or 2  ==     correct
 *
 * It separates in both directions, which is what makes it the cause and not a
 * correlate.  TA5_FS_UNITS=1 fixes all 8.  TA5_FS_UNITS=2 breaks grad:0,
 * layer:0, cube:0, grad:4 and const:0, which otherwise pass.  All thirteen
 * collapse to the one hash 5f9eb3fedcef8383 -- the attachment still holding the
 * clear colour -- under the default build.
 *
 * It also explains why IR3_SHADER_DEBUG=spillall moved the failure set in both
 * directions.  Spilling rewrites the program, so it moves shaders into and out
 * of the 13-to-16 instruction window: the four it fixed left the window, and
 * grad:25, which has no texture instruction at all, entered it.
 *
 * The TA5_*_UNITS overrides stay.  They are what measured this.
 * TA5_FS_UNITS=2 reproduces the defect on the fixed build -- grad:0, layer:0,
 * cube:0 and const:0 all collapse to 5f9eb3fedcef8383 again -- but it is a
 * blunt instrument, not an exact revert: it overrides the length for every
 * fragment stage, so it also breaks the 72 other one-unit shaders that the old
 * formula happened to get right (73 WRONG against lavapipe, versus 8 before).
 * To re-measure the old formula itself, restore the DIV_ROUND_UP term.
 */
static uint32_t
ta5_shader_units(const struct ta5_compiled_shader *shader)
{
   const struct ir3_shader_variant *v =
      (const struct ir3_shader_variant *)shader->variant;
   uint32_t units = shader->instrlen;
   const char *ov = getenv(v->type == MESA_SHADER_VERTEX ? "TA5_VS_UNITS"
                                                          : "TA5_FS_UNITS");

   if (!ov)
      ov = getenv("TA5_SHADER_UNITS");

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

   /*
    * Rasterizer and RB render mode.  Bit 0 is the gmem/sysmem select on
    * both registers; this is the sysmem path (the stream opens with
    * CP_SET_RENDER_MODE(BYPASS) and there is no binning pass), so it has to
    * be 1.
    *
    * fd5 writes 0 here in both its program state (fd5_program.c:574) and its
    * gmem draw state (fd5_draw.c:173), but neither of those is the path a
    * sysmem draw takes: a capture of a working fd5 sysmem draw
    * (FD_MESA_DEBUG=sysmem) has 0x00000001 in both, and with them at 0 the
    * rasterizer runs in gmem mode -- geometry still reaches memory through
    * the bypass, but the fragment stage is handed one vertex's barycentrics
    * for the whole primitive and every interpolated varying collapses to a
    * constant.
    */
   ta5_pkt4(cmd, REG_A5XX_GRAS_CNTL, 1);
   ta5_cs_push(cmd, 0x00000001);

   ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CONTROL0, 2);
   ta5_cs_push(cmd, 0x00000001);
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

   /*
    * VFD_CONTROL_0.VTXCNT belongs to the vertex-fetch program, not to the draw
    * state: GL emits it last in fd5_emit_vertex_bufs() as the number of fetch
    * instructions it just programmed (fd5_emit.c:501) and never clears it.
    * Zeroing it here instead leaves the VFD fetching nothing, every vertex
    * attribute reads as zero, and all the vertices land on (0,0,0,1) -- a
    * zero-area triangle that rasterises no fragments and raises no fault.
    */

   ta5_pkt4(cmd, REG_A5XX_VFD_INDEX_OFFSET, 2);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   if (getenv("TA5_TRACE_VFD")) {
      fprintf(stderr,
              "ta5[vfd]: REGID4VTX=%u REGID4INST=%u (63,0 == invalid)\n",
              vertex_regid, instance_regid);
      fprintf(stderr, "ta5[vfd]: vs inputs: %u\n", vs->inputs_count);
      for (unsigned i = 0; i < vs->inputs_count; i++) {
         fprintf(stderr, "ta5[vfd]:   in[%u] regid=%u sysval=%d slot=%d\n", i,
                 vs->inputs[i].regid, (int)vs->inputs[i].sysval,
                 (int)vs->inputs[i].slot);
      }
      fprintf(stderr, "ta5[vfd]: linkage max_loc=%u cnt=%u\n", l.max_loc,
              l.cnt);
   }

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
 * Upload the per-stage UBO pointer array.
 *
 * Before a6xx a buffer descriptor is not a descriptor at all: it is a 64-bit
 * address written into the stage's constant file, and IR3's emit_intrinsic_load_ubo
 * reads that slot through uniform(ubo_ptrs_base + block * ptrsz).  The block
 * number came from the shader compile (ta5_lower_buffer_descriptors), so this
 * is where that number finally means something.
 *
 * The slot order is ta5_pipeline_layout_buffer_bindings() again, for the same
 * reason the texture units are: the compiler numbered from that list and the
 * emitter has to fill the same one.
 *
 * Addresses are not known to userspace on this platform, so each slot is
 * emitted as two zero dwords plus a relocation; the kernel patches the real
 * address in at submit time, exactly as it does for a texture descriptor.
 */
static VkResult
ta5_emit_ubo_pointers(struct ta5_cmd_buffer *cmd, struct ta5_pipeline *pipe,
                      const struct ta5_compiled_shader *shader,
                      enum a4xx_state_block state_block,
                      VkShaderStageFlags stage)
{
   const struct ir3_shader_variant *v =
      (const struct ir3_shader_variant *)shader->variant;
   const struct ir3_const_state *const_state = ir3_const_state(v);
   struct ta5_buffer_binding order[TA5_MAX_UBOS];
   unsigned num_ubos, num_order;
   uint32_t regid, sizedwords;
   VkResult ret;

   if (!v || const_state->num_ubos == 0)
      return VK_SUCCESS;

   num_ubos = MIN2(const_state->num_ubos, TA5_MAX_UBOS);
   num_order = ta5_pipeline_layout_buffer_bindings(
      pipe->layout, stage, order, ARRAY_SIZE(order));

   /*
    * The IR3 allocation is rounded up to a vec4, and its size is what the
    * hardware sees: loading fewer dwords than the array occupies leaves the
    * rest of it holding the previous draw's pointers.  Emitting the whole
    * allocation, with a zero slot for any block the layout has no descriptor
    * for, keeps the array and the shader's view of it the same length.
    */
   sizedwords =
      MAX2(const_state->allocs.consts[IR3_CONST_ALLOC_UBO_PTRS].size_vec4 * 4,
           num_ubos * 2);
   sizedwords = DIV_ROUND_UP(sizedwords, 4) * 4;
   regid = ir3_const_reg(const_state, IR3_CONST_ALLOC_UBO_PTRS, 0);

   ret = ta5_cmd_buffer_alloc_cs(cmd, cmd->cs_count + 3 + sizedwords + 16);
   if (ret != VK_SUCCESS)
      return ret;

   ta5_pkt7(cmd, CP_LOAD_STATE4, 3 + sizedwords);
   ta5_cs_push(cmd, CP_LOAD_STATE4_0_DST_OFF(regid / 4) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(state_block) |
                     CP_LOAD_STATE4_0_NUM_UNIT(sizedwords / 4));
   ta5_cs_push(cmd, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) |
                     CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
   ta5_cs_push(cmd, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

   for (unsigned i = 0; i < num_ubos; i++) {
      struct ta5_bo *bo = NULL;
      uint64_t bo_offset = 0;

      if (i < num_order) {
         struct ta5_descriptor_set *set = cmd->bound_sets[order[i].set];
         struct ta5_descriptor_set_layout *sl =
            set ? set->layout : NULL;

         if (sl) {
            for (uint32_t j = 0; j < sl->binding_count; j++) {
               const struct ta5_descriptor_layout_binding *bind =
                  &sl->bindings[j];
               struct ta5_descriptor *desc;
               struct ta5_buffer *buf;

               if (bind->binding != order[i].binding)
                  continue;

               desc = &set->descriptors[bind->offset + order[i].element];
               buf = ta5_buffer_from_handle(desc->data.buffer.buffer);
               if (buf && buf->bound_memory) {
                  bo = buf->bound_memory->bo;
                  bo_offset = desc->data.buffer.offset;
               }
               break;
            }
         }
      }

      if (getenv("TA5_TRACE_UBO")) {
         fprintf(stderr,
                 "ta5[ubo] %s block %u set=%u binding=%u elem=%u bo=%p\n",
                 stage == VK_SHADER_STAGE_VERTEX_BIT ? "vs" : "fs", i,
                 i < num_order ? order[i].set : 0,
                 i < num_order ? order[i].binding : 0,
                 i < num_order ? order[i].element : 0, (void *)bo);

         /*
          * The first 64 bytes as the CPU sees them.  This is the question
          * that decides where a blank frame comes from: if the buffer is
          * already empty here, the application never wrote it, and if it is
          * full here but the shader reads zeros, the pointer that goes to the
          * hardware is wrong.
          */
         if (getenv("TA5_TRACE_UBO_DATA") && bo) {
            uint8_t *map = (uint8_t *)ta5_bo_map(bo);

            if (!map) {
               fprintf(stderr, "ta5[ubo] block %u: map failed\n", i);
            } else {
               /*
                * The whole block, 16 bytes to a line, capped at the buffer.
                * 16 bytes was enough to answer "did the application write
                * anything", which is what this was for, and useless for
                * anything else: vkcube's block is 1216 bytes and 600 of them
                * are the cube's vertex positions, so the cube cannot be fed
                * back through the harness from a 16-byte sample.
                */
               uint64_t avail = bo->size > bo_offset ? bo->size - bo_offset : 0;
               int n = (int)MIN2(avail, 2048);

               fprintf(stderr, "ta5[ubo] block %u data (%d bytes):", i, n);
               for (int k = 0; k < n; k++) {
                  if (k && (k % 16) == 0)
                     fprintf(stderr, "\nta5[ubo]   ");
                  fprintf(stderr, " %02x", map[bo_offset + k]);
               }
               fprintf(stderr, "\n");
            }
         }
      }

      if (bo) {
         ret = ta5_cs_reloc(cmd, bo, bo_offset, 0);
         if (ret != VK_SUCCESS)
            return ret;
      } else {
         /* An unbound block reads as a zero address rather than whatever the
          * previous draw left in the slot. */
         ta5_cs_push(cmd, 0);
         ta5_cs_push(cmd, 0);
      }
   }

   for (uint32_t w = num_ubos * 2; w < sizedwords; w++)
      ta5_cs_push(cmd, 0);

   return VK_SUCCESS;
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

   /*
    * fd5_emit_shader_obj() programs a real private-memory window when the
    * shader asks for one (ir3_get_private_mem(): per_sp_size =
    * align(pvtmem_size * fibers_per_sp, 4K), a real BO for the address) and
    * all zeros when it does not -- so these zeros are right *only* if the
    * variant really has no private memory.  A shader that needs it gets a
    * window of size zero here, and private accesses then land at address 0,
    * which is the null-IOVA fault the stripped variants produce.
    */
   if (getenv("TA5_TRACE_PVT")) {
      const struct ir3_shader_variant *v =
         (const struct ir3_shader_variant *)pipe->has_vertex
            ? (const struct ir3_shader_variant *)pipe->vertex.variant
            : (const struct ir3_shader_variant *)pipe->fragment.variant;
      fprintf(stderr, "ta5[pvt]: obj reg=0x%03x pvtmem_size=%u per_wave=%u\n",
              reg, v->pvtmem_size, v->pvtmem_per_wave);
   }
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
 *   TA5_EMPTY_IB          record no packets at all in the render pass
 *   TA5_SKIP_PREP         record no sysmem prep
 *   TA5_PADEND=N          emit N NOP dwords at the *end* of the stream
 *                          instead of the start
 *   TA5_PAD=N             emit N NOP dwords into the stream, to move the
 *                          stream size without changing which phases
 *                          run
 *   TA5_SKIP_CLEAR        record no attachment clear
 *   TA5_SKIP_CLEAR_COLOR  record no colour clear
 *   TA5_SKIP_CLEAR_DEPTH  record no depth clear
 *   TA5_WFI_CCU           emit the wait-for-idle GL puts immediately before
 *                          the prep's CCU reconfiguration
 *   TA5_DEPTH_NOTS        emit the depth clear's CCU resolve as a bare
 *                          event, with no timestamp and no address, to
 *                          separate the resolve itself from the address
 *                          handed to it
 *   TA5_SKIP_FINI         record no sysmem fini
 *                          (Begin/End), so with TA5_SKIP_DRAW the whole
 *                          frame submits an empty command stream.  This
 *                          separates our *packets* from our *submit
 *                          mechanics* -- the fence, the BO set, the
 *                          relocs and the IB address are all still
 *                          there; only the contents go away.
 *   TA5_SKIP_FS            load no fragment program at all
 *   TA5_FS_NO_DEPTH_OUT    leave DEPTH_REGID out of SP_FS_OUTPUT_CNTL
 *   TA5_CLEAR_IN_DRAW      emit a magenta clear at the top of the draw path,
 *                          as a reachability probe (if it lands, the CP got
 *                          that far; if not, it never did)
 *   TA5_SHADER_UNITS=N     raise the CP_LOAD_STATE4 program-load length
 *   TA5_INSTRLEN=N         raise HLSQ_xS_CNTL.INSTRLEN
 *   TA5_PREAMBLE_ROWS=N    emit only the first N state-restore register rows
 *   TA5_SKIP_VSPROG       drop the vertex program load from the indirect buffer
 *   TA5_SKIP_FSPROG       drop the fragment program load (ours; GL's TA5_SKIP_FS
 *                          also drops SP_FS_OBJ_START, which is different)
 *   TA5_SKIP_CONSTS        drop the constant uploads and UBO pointers
 *   TA5_SKIP_DRAWPKT       drop CP_DRAW_INDX_OFFSET, keeping the rest of the IB
 *   TA5_SKIP_DRAWSTATE     drop ta5_emit_draw_state() from the draw
 *   TA5_SKIP_PROGSTATE     drop ta5_emit_program_state() from the draw
 *   TA5_SKIP_TEX           drop both texture-descriptor emissions
 *   TA5_SKIP_VPORT         drop the viewport scale/offset
 *   TA5_SKIP_VFD           drop VFD_CONTROL_0
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

/**
 * Emit the texture and sampler descriptor state for one stage.
 *
 * Transcribed from fd5_emit_textures() (fd5_emit.c:315).  a5xx has no
 * descriptor heap: the descriptors the shader can reach are written straight
 * into the command stream with CP_LOAD_STATE4/SS4_DIRECT, so there is nothing
 * to bind and nothing that has to outlive the submission.  Only the image
 * address inside each texture descriptor is a relocation.
 *
 * The two halves go into the same state block at the same offset and are told
 * apart by STATE_TYPE -- the sampler words are ST4_SHADER, the texture words
 * ST4_CONSTANTS.
 *
 * Which descriptor lands in which slot is not decided here: the unit order
 * comes from ta5_pipeline_layout_tex_bindings(), the same function the shader
 * compiler numbers its texture instructions with.  Deciding it twice is how
 * the two halves drift apart.
 *
 * `sb` is SB4_VS_TEX or SB4_FS_TEX.  Returns the number of texture units bound.
 */
static unsigned
ta5_emit_textures(struct ta5_cmd_buffer *cmd, enum a4xx_state_block sb,
                  struct ta5_pipeline *pipe, VkShaderStageFlags stage)
{
   struct ta5_descriptor *tex_desc[TA5_MAX_TEX_UNITS] = {0};
   struct ta5_descriptor *samp_desc[TA5_MAX_TEX_UNITS] = {0};
   struct ta5_tex_binding order[TA5_MAX_TEX_UNITS * 2];
   uint32_t num_textures = 0, num_samplers = 0;
   unsigned num_order;

   if (!pipe || !pipe->layout)
      return 0;

   num_order = ta5_pipeline_layout_tex_bindings(
      pipe->layout, stage, order, ARRAY_SIZE(order));

   /*
    * A shader names a texel unit by number and the hardware fetches unit N
    * from the Nth descriptor written above, so the two counts have to agree
    * as well as the order does.  Past 16 units the hardware cannot address
    * them, so the extra units are dropped rather than silently aliased onto
    * a unit that is in use.
    */
   for (unsigned i = 0; i < num_order; i++) {
      struct ta5_descriptor_set *set = cmd->bound_sets[order[i].set];
      struct ta5_descriptor_set_layout *sl = set ? set->layout : NULL;
      struct ta5_descriptor *desc = NULL;

      if (!sl)
         continue;

      for (uint32_t j = 0; j < sl->binding_count; j++) {
         const struct ta5_descriptor_layout_binding *b = &sl->bindings[j];

         if (b->binding == order[i].binding) {
            desc = &set->descriptors[b->offset];
            break;
         }
      }

      if (!desc)
         continue;

      /*
       * Slot i, not "the next free one".
       *
       * ta5_lower_tex_units() numbers a shader's texture_index and
       * sampler_index with one global index into the same combined order this
       * loop walks, so slot i here is the slot unit i fetches.  Packing each
       * array with its own running counter instead is invisible for
       * COMBINED_IMAGE_SAMPLER-only layouts -- both counters advance together
       * -- and wrong the moment a layout separates them, because a SAMPLER
       * occupying order index 0 leaves tex_desc[0] holding the descriptor from
       * index 1, and the shader asking for unit 1 reads tex_desc[1], which was
       * never written.
       *
       * So the index is the order index, a descriptor of the other kind leaves
       * its slot NULL, and the write loops below emit zeros for those.  The
       * counts are the highest index written plus one, because the load is
       * contiguous from slot 0.
       *
       * Past TA5_MAX_TEX_UNITS the hardware cannot address the unit at all,
       * so the descriptor is dropped rather than silently aliased onto a unit
       * that is in use.
       */
      if (i >= TA5_MAX_TEX_UNITS)
         break;

      if (order[i].type == VK_DESCRIPTOR_TYPE_SAMPLER) {
         samp_desc[i] = desc;
         if (num_samplers < i + 1)
            num_samplers = i + 1;
         continue;
      }

      tex_desc[i] = desc;
      if (num_textures < i + 1)
         num_textures = i + 1;

      if (order[i].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
         samp_desc[i] = desc;
         if (num_samplers < i + 1)
            num_samplers = i + 1;
      }
   }

   if (getenv("TA5_TRACE_TEX"))
      fprintf(stderr,
              "ta5[tex] sb=%u order=%u tex=%u samp=%u bound_sets=%u\n",
              sb, num_order, num_textures, num_samplers,
              (unsigned)ARRAY_SIZE(cmd->bound_sets));

   if (num_textures == 0)
      return 0;

   if (ta5_cmd_buffer_alloc_cs(cmd, cmd->cs_count +
                                       4 * num_samplers +
                                       TA5_TEXEL_DESC_DWORDS * num_textures +
                                       32) != VK_SUCCESS) {
      if (getenv("TA5_TRACE_TEX"))
         fprintf(stderr, "ta5[tex] alloc_cs failed\n");
      return 0;
   }

   /* Sampler words: 4 dwords per unit, ST4_SHADER. */
   if (num_samplers) {
      ta5_pkt7(cmd, CP_LOAD_STATE4, 3 + 4 * num_samplers);
      ta5_cs_push(cmd, CP_LOAD_STATE4_0_DST_OFF(0) |
                        CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                        CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                        CP_LOAD_STATE4_0_NUM_UNIT(num_samplers));
      ta5_cs_push(cmd, CP_LOAD_STATE4_1_STATE_TYPE(ST4_SHADER) |
                        CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
      ta5_cs_push(cmd, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

      for (uint32_t i = 0; i < num_samplers; i++) {
         /* A NULL slot is a unit whose descriptor is a texture and not a
          * sampler, and is written as zeros so the units above it keep their
          * places.  Without this the deref is of NULL + the sampler offset:
          * measured as a segfault at address 0x10 on MODE=septex, the first
          * layout with a SAMPLED_IMAGE at a lower order index than a
          * SAMPLER. */
         struct ta5_sampler *samp =
            samp_desc[i] ? ta5_sampler_from_handle(samp_desc[i]->data.image.sampler)
                         : NULL;

         for (uint32_t w = 0; w < 4; w++)
            ta5_cs_push(cmd, samp ? samp->desc[w] : 0);
      }
   }

   /* Texture words: 12 dwords per unit, ST4_CONSTANTS. */
   ta5_pkt7(cmd, CP_LOAD_STATE4, 3 + TA5_TEXEL_DESC_DWORDS * num_textures);
   ta5_cs_push(cmd, CP_LOAD_STATE4_0_DST_OFF(0) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                     CP_LOAD_STATE4_0_NUM_UNIT(num_textures));
   ta5_cs_push(cmd, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                     CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
   ta5_cs_push(cmd, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

   for (uint32_t i = 0; i < num_textures; i++) {
      struct ta5_descriptor *d = tex_desc[i];
      struct ta5_image_view *view =
         d ? ta5_image_view_from_handle(d->data.image.image_view) : NULL;
      uint32_t words[TA5_TEXEL_DESC_DWORDS];
      uint64_t address = 0;

      /* A NULL slot is a unit whose descriptor is of the other kind -- a
       * SAMPLER in a layout that also separates the two -- and it is written as
       * zeros so that the units above it keep their places. */
      if (!d || !view || !view->image->bound_memory ||
          !view->image->bound_memory->bo) {
         for (uint32_t w = 0; w < TA5_TEXEL_DESC_DWORDS; w++)
            ta5_cs_push(cmd, 0);
         continue;
      }

      ta5_texel_descriptor(view, words, &address);

      ta5_cs_push(cmd, words[0]);
      ta5_cs_push(cmd, words[1]);
      ta5_cs_push(cmd, words[2]);
      ta5_cs_push(cmd, words[3]);

      /*
       * Words 4 and 5 are the image address, and word 5 -- TEX_CONST_5 --
       * rides in the high half of the high address dword as the relocation's
       * OR mask.  fd5 does exactly this
       *
       *     OUT_RELOC(ring, rsc->bo, view->offset,
       *               (uint64_t)view->texconst5 << 32, 0);
       *
       * so there is no separate TEX_CONST_5 word to push.  Passing 0 for the
       * mask silently drops it, and TEX_CONST_5 is where DEPTH lives -- the
       * cube's face count and a 3D image's depth.  Without it the hardware
       * fetches a single layer, which is not an error, just the wrong pixels.
       */
      if (getenv("TA5_TRACE_TEX"))
         fprintf(stderr,
                 "ta5[tex] desc: %08x %08x %08x %08x %08x addr=%llx bo=%p\n",
                 words[0], words[1], words[2], words[3], words[5],
                 (unsigned long long)address,
                 (void *)view->image->bound_memory->bo);

      ta5_cs_reloc(cmd, view->image->bound_memory->bo, address, words[5]);

      for (uint32_t w = 6; w < TA5_TEXEL_DESC_DWORDS; w++)
         ta5_cs_push(cmd, words[w]);
   }

   return num_textures;
}

static void ta5_emit_draw_state(struct ta5_cmd_buffer *cmd,
                               unsigned vs_tex_count, unsigned fs_tex_count,
                               uint32_t vertex_count,
                               const struct ta5_pipeline *pipe);

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
      /*
       * HLSQ_UPDATE_CNTL is deliberately absent.
       *
       * It used to lead the table as 0x000fffff, which no fd5 restore carries
       * -- decoding GL's own stream (glsys.cs.0.bo5) shows exactly one write to
       * this register, the 0x020fffff that fd5_program_emit() emits just
       * before the program load.  Ours had two: 0x000fffff here and 0x020fffff
       * in the program state.
       *
       * This register tells the HLSQ what changed so it knows what to re-fetch,
       * so the extra write is not a harmless duplicate -- and it is the only
       * register in the draw path that GL and this driver disagreed on before
       * the vertex stage was found not to run at all.  If it is a sticky
       * invalidate set, bits 0-19 survive the later 0x020fffff (which only adds
       * bit 25) and the HLSQ never considers its state valid, which would
       * explain a draw packet that the CP accepts and never dispatches.
       */
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
      /*
       * HLSQ_DBG_ECO_CNTL and HLSQ_UPDATE_CNTL are in the kernel's
       * save/restore blind spot: the kernel programs PC_DBG_ECO_CNTL and
       * RB_DBG_ECO_CNTL once at init and restores neither of these per
       * context, so they hold whatever ran last.  GL writes both on every
       * restore (fd5_emit.c: HLSQ_DBG_ECO_CNTL = 0, HLSQ_UPDATE_CNTL =
       * 0xfffff), so a GL client always leaves them sane for the next one.
       * This driver never wrote them, and every submission left them for the
       * compositor's context -- the one difference between "our submits are
       * fine alone" (the harness, one client) and "our submits take the
       * compositor down" (two clients).
       */
      { REG_A5XX_HLSQ_DBG_ECO_CNTL, 1, { 0x00000000 } },
      { REG_A5XX_HLSQ_UPDATE_CNTL, 1, { 0x000fffff } },
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

   /*
    * The LRZ flush, and with it whether the LRZ is on at all.
    *
    * fd5_emit_lrz_flush() writes LRZ_CNTL_ENABLE because by then GL has an LRZ
    * buffer behind it -- fd5_gmem.c programs GRAS_LRZ_BUFFER_BASE,
    * GRAS_LRZ_BUFFER_PITCH and GRAS_LRZ_FAST_CLEAR_BUFFER_BASE for the
    * attachment's rsc->lrz before any draw runs.  This driver has no LRZ
    * buffer and never programs those three registers, so enabling the unit
    * hands it a base left behind by whichever client ran on this GPU last.
    *
    * That is not hypothetical: with a depth attachment the LRZ is live for the
    * whole pass, and the unit dereferences the stale base.  Measured per frame,
    * D32 depth, faults from the kernel's IOMMU report:
    *
    *     64x64, 256x256, 512x512, 1024x1024          0 faults
    *     1023x1023, 1025x1025, 1080x1080, 1080x2280 10 faults
    *
    * and the fault addresses are outside every buffer this process allocates,
    * advancing 64 bytes at a time.  The sizes that fault are exactly the ones
    * whose height is not a whole number of 64-pixel tiles, which is the shape
    * of the LRZ's fast-clear path.
    *
    * TA5_LRZ_ENABLE=1 puts the enable back, for the A/B.
    */
   ta5_pkt4(cmd, REG_A5XX_GRAS_LRZ_CNTL, 1);
   ta5_cs_push(cmd, getenv("TA5_LRZ_ENABLE")
                       ? A5XX_GRAS_LRZ_CNTL_ENABLE
                       : 0x00000000);
   if (!getenv("TA5_NO_LRZ_FLUSH"))
      ta5_event_write(cmd, LRZ_FLUSH_INVALIDATE);

   ta5_pkt7(cmd, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
   ta5_cs_push(cmd, 0);

   if (!getenv("TA5_NO_CCU_INVALIDATE_COLOR"))
      ta5_event_write(cmd, PC_CCU_INVALIDATE_COLOR);

   /*
    * PC_CCU_INVALIDATE_DEPTH belongs here too, and is deliberately absent.
    * The storm below is the depth clear's CCU resolve walking the CCU against
    * state this prep never invalidated, so invalidating the depth side before
    * it is the obvious move -- and it was measured and it does not help.  With
    * it: 20 presents, 10 faults.  Without it: the same 10 faults.  Kept out
    * rather than deleted from this comment's history so the next attempt
    * starts from the measurement instead of re-trying it.
    */

   uint32_t num_sp = cmd->device->physical_device->gpu_info.num_sp_cores;

   ta5_pkt4(cmd, REG_A5XX_PC_POWER_CNTL, 1);
   ta5_cs_push(cmd, num_sp - 1);

   ta5_pkt4(cmd, REG_A5XX_VFD_POWER_CNTL, 1);
   ta5_cs_push(cmd, num_sp - 1);

   /*
    * TA5_WFI_CCU emits the wait-for-idle GL puts immediately before this
    * register write -- fd5_emit_gmem() does fd_wfi(batch, ring) and only then
    * RB_CCU_CNTL, with the comment "0x10000000 for BYPASS.. 0x7c13c080 for
    * GMEM".  The note below used to argue against emitting it; that argument
    * assumed the CCU is already quiet, which holds when this is the only
    * client on the GPU and not when a compositor is there too.
    */
   if (getenv("TA5_WFI_CCU"))
      ta5_wfi(cmd);

   /*
    * No wait-for-idle here by default.  GL calls fd_wfi() at this point, but
    * that is guarded on batch->needs_wfi, and the cache flush in
    * ta5_emit_restore() has already reset and consumed the flag -- so GL emits
    * exactly one WFI per stream, in the cache flush.  See TA5_WFI_CCU above.
    */

   /*
    * 0x10000000 is GL's BYPASS value ("0x10000000 for BYPASS.. 0x7c13c080 for
    * GMEM"), and TA5_CCU_CNTL=<hex> overrides it -- because the resolve after
    * a depth clear faults, and this register is the CCU's configuration, so it
    * is the next thing to be wrong in.
    */
   const char *ccucntl = getenv("TA5_CCU_CNTL");

   if (!getenv("TA5_NO_CCU_CNTL")) {
      ta5_pkt4(cmd, REG_A5XX_RB_CCU_CNTL, 1);
      ta5_cs_push(cmd, ccucntl ? (uint32_t)strtoul(ccucntl, NULL, 0)
                               : 0x10000000);
   }

   /*
    * TA5_PREP_RESOLVE emits a CCU resolve here, in the prep, with no clear
    * anywhere in the stream.  It separates "the depth clear's resolve" from
    * "any resolve after this prep": the clear is a red herring if a bare
    * resolve faults and the real fault is in what the prep leaves behind.
    */
   if (getenv("TA5_PREP_RESOLVE"))
      ta5_event_write_ts(cmd, cmd->device->blit_bo, CCU_RESOLVE);

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

   /*
    * Colour format plumbing.  Every colour format this driver accepts is an
    * 8_8_8_8 with a different channel order or gamma, and the hardware
    * expresses both here: COLOR_SWAP rotates the channels (fd5_format.c
    * gives B8G8R8A8_* WXYZ and R8G8B8A8_* WZYX) and COLOR_SRGB turns on the
    * fixed-function sRGB encode, which fd5_gmem.c mirrors on SP_FS_MRT_REG
    * so the fragment output is converted too.  The hardware format enum
    * itself has no sRGB variant; it stays 8_8_8_8_UNORM.
    */
   enum a3xx_color_swap swap = (img->format == VK_FORMAT_B8G8R8A8_UNORM ||
                                img->format == VK_FORMAT_B8G8R8A8_SRGB)
                                  ? WXYZ : WZYX;
   bool srgb = (img->format == VK_FORMAT_B8G8R8A8_SRGB ||
                img->format == VK_FORMAT_R8G8B8A8_SRGB);

   /* Color attachment 0.  BASE_LO/BASE_HI is a 64 bit reloc. */
   ta5_pkt4(cmd, REG_A5XX_RB_MRT_BUF_INFO(0), 5);
   ta5_cs_push(cmd, A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(RB5_R8G8B8A8_UNORM) |
                     A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(TILE5_LINEAR) |
                     A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(swap) |
                     (srgb ? A5XX_RB_MRT_BUF_INFO_COLOR_SRGB : 0));
   ta5_cs_push(cmd, A5XX_RB_MRT_PITCH(stride));
   ta5_cs_push(cmd, A5XX_RB_MRT_ARRAY_PITCH(stride * h));
   ret = ta5_cs_reloc(cmd, bo, img->offset, 0);
   if (ret != VK_SUCCESS)
      return ret;

   ta5_pkt4(cmd, REG_A5XX_SP_FS_MRT_REG(0), 1);
   ta5_cs_push(cmd, A5XX_SP_FS_MRT_REG_COLOR_FORMAT(RB5_R8G8B8A8_UNORM) |
                     (srgb ? A5XX_SP_FS_MRT_REG_COLOR_SRGB : 0));

   ta5_pkt4(cmd, REG_A5XX_RB_MRT_FLAG_BUFFER(0), 4);
   ta5_cs_push(cmd, 0);
   ta5_cs_push(cmd, 0);
   ta5_cs_push(cmd, 0);
   ta5_cs_push(cmd, 0);

   /*
    * The depth attachment, the way fd5_emit_sysmem_prep() ends with
    * emit_zs(): RB_DEPTH_BUFFER_INFO with the format and the buffer, then the
    * rasteriser's view of the same format, then the flag buffer, the LRZ pair
    * and the stencil info at zero.
    *
    * This is in the *prep* on purpose.  The depth clear runs between here and
    * the first draw, and until now nothing had described the depth buffer to
    * the units that the clear uses: the clear programmed RB_DEPTH_BUFFER_INFO
    * and nothing else, so GRAS_SU_DEPTH_BUFFER_INFO, RB_STENCIL_INFO,
    * GRAS_LRZ_BUFFER_BASE and GRAS_LRZ_FAST_CLEAR_BUFFER_BASE carried whatever
    * the last client on this GPU left in them.  Those are register state that
    * outlives a submission.
    *
    * Measured per frame, D32 depth attachment, faults from the kernel's IOMMU
    * report, two runs each:
    *
    *     64x64, 256x256, 512x512, 1024x1024          0 faults
    *     1023x1023, 1025x1025, 1080x1080, 1080x2280 10 faults
    *
    * -- every size whose height is not a whole number of 64-pixel tiles, which
    * is where a stale depth description stops matching the buffer.  Fault
    * addresses are outside every buffer this process allocates, 64 bytes apart.
    */
   if (!getenv("TA5_NO_DEPTH_TARGET")) {
      int didx = ta5_render_pass_depth_attachment(fb->render_pass);
      const struct ta5_image_view *dview =
         didx >= 0 ? fb->attachments[didx] : NULL;
      uint32_t hwfmt =
         didx >= 0
            ? ta5_format_depth_hw(fb->render_pass->attachments[didx].format)
            : DEPTH5_NONE;

      if (dview) {
         const struct ta5_image_level *dlvl =
            &dview->image->levels[MIN2(dview->base_mip_level,
                                        dview->image->mip_levels - 1)];

         ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
         ta5_cs_push(cmd, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(
                            (enum a5xx_depth_format)hwfmt));
         ret = ta5_cs_reloc(cmd, dview->image->bound_memory->bo, 0, 0);
         if (ret != VK_SUCCESS)
            return ret;
         ta5_cs_push(cmd, A5XX_RB_DEPTH_BUFFER_PITCH((uint32_t)dview->row_pitch));
         ta5_cs_push(cmd, A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH(
                            (uint32_t)dlvl->layer_pitch));

         ta5_pkt4(cmd, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
         ta5_cs_push(cmd, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(
                            (enum a5xx_depth_format)hwfmt));
      } else {
         ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
         ta5_cs_push(cmd, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH5_NONE));
         ta5_cs_push(cmd, 0x00000000); /* BASE_LO */
         ta5_cs_push(cmd, 0x00000000); /* BASE_HI */
         ta5_cs_push(cmd, 0x00000000); /* PITCH */
         ta5_cs_push(cmd, 0x00000000); /* ARRAY_PITCH */

         ta5_pkt4(cmd, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
         ta5_cs_push(cmd, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(
                            DEPTH5_NONE));
      }

      /* No flag buffer, no LRZ, no separate stencil: say so, every pass. */
      ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
      ta5_cs_push(cmd, 0x00000000);
      ta5_cs_push(cmd, 0x00000000);
      ta5_cs_push(cmd, 0x00000000);

      ta5_pkt4(cmd, REG_A5XX_GRAS_LRZ_BUFFER_BASE, 3);
      ta5_cs_push(cmd, 0x00000000);
      ta5_cs_push(cmd, 0x00000000);
      ta5_cs_push(cmd, 0x00000000);

      ta5_pkt4(cmd, REG_A5XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE, 2);
      ta5_cs_push(cmd, 0x00000000);
      ta5_cs_push(cmd, 0x00000000);

      ta5_pkt4(cmd, REG_A5XX_RB_STENCIL_INFO, 1);
      ta5_cs_push(cmd, 0x00000000);
   }

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
   /* No pipeline is bound yet: this arms the frame, and every draw
    * re-emits this state with the pipeline that owns it. */
   ta5_emit_draw_state(cmd, 0, 0, 0, NULL);

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
ta5_emit_draw_state(struct ta5_cmd_buffer *cmd, unsigned vs_tex_count,
                     unsigned fs_tex_count, uint32_t vertex_count,
                     const struct ta5_pipeline *pipe)
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

   /*
    * Low-resolution Z.  GL leaves LRZ off for the whole draw
    * (fd5_draw.c:179), and enabling it without an LRZ clear discards every
    * fragment before the shader sees it.  There is no depth attachment here
    * for the fast path to depend on, so this is unconditional and matches the
    * GL stream; a pipeline that does have depth/stencil state to preserve has
    * to restore LRZ_CNTL from it instead (see fd5_zsa.c:32).
    */
   ta5_pkt4(cmd, REG_A5XX_GRAS_LRZ_CNTL, 1);
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

   /*
    * Texture counts.  fd5 sets these after emitting the texture state, and
    * they are what tells the fetch unit how many of the descriptors just
    * written are live.  Zero here means "no descriptors", which is what the
    * invalidate-marker texel units further down then have to agree with.
    */
   ta5_pkt4(cmd, REG_A5XX_TPL1_VS_TEX_COUNT, 1);
   ta5_cs_push(cmd, (uint32_t)vs_tex_count);

   ta5_pkt4(cmd, REG_A5XX_TPL1_FS_TEX_COUNT, 1);
   ta5_cs_push(cmd, (uint32_t)fs_tex_count);

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

   /* Rasteriser: filled triangles, width 1, no polygon offset.
    *
    * GRAS_SU_CNTL also carries the face cull bits and the front-facing
    * winding, which GL's rasteriser state holds on this same register
    * (fd5_rasterizer.c: A5XX_GRAS_SU_CNTL_CULL_FRONT / _CULL_BACK, with
    * FRONT_CW next to them).  Both used to be dropped, so every triangle was
    * rasterised in both windings; see ta5_pipeline.h for what that did to
    * vkcube's cube.
    */
   ta5_pkt4(cmd, REG_A5XX_GRAS_SU_CNTL, 1);
   ta5_cs_push(cmd, (pipe && pipe->cull_front
                        ? A5XX_GRAS_SU_CNTL_CULL_FRONT
                        : 0) |
                     (pipe && pipe->cull_back
                         ? A5XX_GRAS_SU_CNTL_CULL_BACK
                         : 0) |
                     (pipe && pipe->front_cw ? A5XX_GRAS_SU_CNTL_FRONT_CW
                                             : 0) |
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
   enum adreno_pa_su_sc_draw probe_ptype = getenv("TA5_POINTS")
                                               ? PC_DRAW_POINTS
                                               : PC_DRAW_TRIANGLES;
   ta5_cs_push(cmd, A5XX_PC_RASTER_CNTL_POLYMODE_FRONT_PTYPE(probe_ptype) |
                      A5XX_PC_RASTER_CNTL_POLYMODE_BACK_PTYPE(probe_ptype));

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
    * ones, not four channel dwords, and the byte order is ABGR counting down
    * from the most significant byte -- equivalently R in the low byte.
    *
    * The previous comment here claimed BGRA.  That was right about alpha being
    * in the high byte and wrong about R and B, and it stayed invisible for as
    * long as every test drew a full-screen triangle over the whole
    * attachment: black and green hide the transposition, and only a
    * distinctly coloured clear exposes it.  Measured on msm8953, clearing the
    * attachment and then leaving it uncovered:
    *
    *     requested      rendered
    *     (255,  0,  0)   (  0,  0,255)     red came out blue
    *     (  0,  0,255)   (255,  0,  0)     blue came out red
    *     ( 17, 34, 51)   ( 51, 34, 17)
    *     (  0,255,  0)   (  0,255,  0)     green is the middle byte
    *     (  0,  0,  0)   (  0,  0,  0)     all zero
    */
   {
      uint32_t r = color->u32[0], g = color->u32[1], b = color->u32[2];

      /*
       * TA5_FORCE_CLEAR=<r>,<g>,<b>: replace the application's clear colour.
       *
       * "Is the cube missing, or is it drawn in the colour of the clear?"
       * cannot be answered from the pixels when the two are close, because
       * both count as background.  vkcube clears to 0x333333 and its cube
       * covers 30% of what it should, so this is the question that decides
       * whether the remaining fault is geometry or colour.
       */
      const char *fc = getenv("TA5_FORCE_CLEAR");
      if (fc) {
         unsigned fr = 0, fg = 0, fb = 0;
         if (sscanf(fc, "%u,%u,%u", &fr, &fg, &fb) == 3) {
            r = fr & 0xff;
            g = fg & 0xff;
            b = fb & 0xff;
         }
      }

      ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
      ta5_cs_push(cmd, (color->u32[3] << 24) | (b << 16) | (g << 8) | r);
      ta5_cs_push(cmd, 0x00000000);
      ta5_cs_push(cmd, 0x00000000);
      ta5_cs_push(cmd, 0x00000000);
   }

   if (device->blit_bo == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   ret = ta5_event_write_ts(cmd, device->blit_bo, CCU_RESOLVE);
   ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_CNTL, 1);
   ta5_cs_push(cmd, 0);

   /* Back to rendering, not blitting. */
   ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   /*
    * The timestamp is a relocation and the relocation can fail, so its result
    * is the result.  Returning VK_SUCCESS over it reported a clear whose
    * completion was never going to be waited for as a clear that had
    * happened.
    */
   return ret;
}

/**
 * Clear the depth/stencil attachment.
 *
 * The same shape as ta5_emit_clear() with two differences, both from
 * fd5_clear() (fd5_draw.c:291-312): the blit source is BLIT_ZS rather than
 * BLIT_MRT0, and the clear mask is 0x1 for depth alone.  The packed value
 * still goes in RB_CLEAR_COLOR_DW0 -- despite the name, that register is the
 * blit's data source for either kind of buffer.
 *
 * Returns VK_ERROR_FORMAT_NOT_SUPPORTED for a depth format with no hardware
 * code rather than clearing it as DEPTH5_NONE.  ta5_CreateGraphicsPipelines
 * rejects the same formats, so this is a second line, not the only one.
 */
static VkResult
ta5_emit_clear_depth(struct ta5_cmd_buffer *cmd, struct ta5_device *device,
                     const struct ta5_render_pass_attachment *att,
                     const struct ta5_image_view *view, float depth)
{
   uint32_t hwfmt = ta5_format_depth_hw(att->format);
   uint32_t packed;
   VkResult ret;

   if (hwfmt == 0)
      return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);

   /*
    * TA5_NO_DEPTH_CLEAR, TA5_NO_DEPTH_STATE and TA5_NO_FASTCLEAR are bisect
    * switches for the fault that only appears with a depth attachment at the
    * panel's size: they turn off, one at a time, the depth clear, the depth
    * test state and the fast-clear bit, so the fault can be attributed to a
    * register rather than to "the depth path".
    */
   if (getenv("TA5_NO_DEPTH_CLEAR"))
      return VK_SUCCESS;
   const int no_bufinfo = getenv("TA5_NO_DEPTH_BUFINFO") != NULL;
   const int no_resolve = getenv("TA5_NO_DEPTH_RESOLVE") != NULL;

   /*
    * The clear value has to be in the *buffer's* encoding, not the float's bit
    * pattern.  util_pack_z()/util_pack_z_stencil() do exactly this:
    *
    *    Z16_UNORM          float -> 16-bit unorm    (1.0 -> 0xffff)
    *    Z24_UNORM_S8_UINT  float -> 24-bit unorm in the low bits, stencil
    *                       in the top 8            (1.0 -> 0x00ffffff)
    *    Z32_FLOAT          the float's own bits    (1.0 -> 0x3f800000)
    *
    * Taking the top 16 bits of the float instead -- which is what this did --
    * puts 0x3f80, i.e. 0.248, into a depth buffer being cleared to 1.0.  Every
    * fragment then fails a LESS depth test against it, and the frame comes out
    * as the clear colour with nothing drawn and *no* faults to explain it.
    *
    * That is exactly what vkcube did: it takes a D16 depth buffer, and every
    * one of the 115 matrix cases asks for D32, where the float's bits are the
    * encoding and the mistake cannot show.
    */
   {
      float z = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);

      /*
       * In double, and that is the whole fix rather than a style choice.
       *
       * A float has 24 bits of significand, so at the magnitude of
       * 16777215 there is no fractional part left to represent: adding the
       * half-unit used for rounding takes 16777215.0f + 0.5f to
       * 16777216.0f.  The truncation then produced 0x1000000, and the
       * 24-bit mask reduced that to zero -- so a D24_UNORM_S8_UINT
       * attachment was cleared to 0.0 rather than to 1.0.
       *
       * Measured, one depth clear to 1.0 per run, the packed dword and the
       * attachment's own bytes afterwards:
       *
       *     D16_UNORM          packed 0x0000ffff   ff ff ff ff ...  cleared
       *     D32_SFLOAT         packed 0x3f800000   00 00 80 3f ...  cleared
       *     D24_UNORM_S8_UINT  packed 0x00000000   00 00 00 00 ...  all zero
       *
       * Sixteen bits does not overflow -- 65535.5f is exactly representable --
       * which is why only the 24-bit format lost its clear, and why the 115
       * matrix cases never saw it: they ask for D32, where the float's own bit
       * pattern is the encoding and this arithmetic is not involved.
       *
       * Consequence beyond the clear: vkcube asks for D24_UNORM_S8_UINT on
       * this device, so its depth buffer was zero, every fragment failed its
       * LESS test, and the frame came out as the clear colour with nothing
       * drawn -- no fault, nothing in dmesg, nothing to attribute it to.
       */
      switch (att->format) {
      case VK_FORMAT_D16_UNORM:
         packed = (uint32_t)((double)z * 65535.0 + 0.5);
         break;
      case VK_FORMAT_D24_UNORM_S8_UINT:
         /* stencil byte stays 0: this driver has no stencil state */
         packed = (uint32_t)((double)z * 16777215.0 + 0.5) & 0x00ffffffu;
         break;
      default: /* VK_FORMAT_D32_SFLOAT */
         memcpy(&packed, &depth, sizeof(packed));
         break;
      }
   }

   ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
   ta5_cs_push(cmd, 0x00000000);

   ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   ta5_pkt4(cmd, REG_A5XX_RB_BLIT_CNTL, 1);
   ta5_cs_push(cmd, A5XX_RB_BLIT_CNTL_BUF(BLIT_ZS));

   /*
    * TA5_SLOW_CLEAR does this clear as a real blit instead of a CCU fast
    * clear: the destination is described through the blit's own registers,
    * RB_CLEAR_CNTL loses FAST_CLEAR, and no CCU resolve follows.
    *
    * This is the one route that does not go through the CCU at all, and it is
    * the only one that does not cost the clear.  Measured, depth readout of a
    * 360x713 D16 attachment (want 0 of 4096 below 0.999, first = 1):
    *
    *    fast clear + CCU resolve          0 of 4096, first = 1   correct
    *    TA5_NO_DEPTH_RESOLVE=1          4096 of 4096, first = 0   no clear
    *    TA5_DEPTH_EVENT=d               4096 of 4096, first = 0   no clear
    *    TA5_RESOLVE_MRT=1               4096 of 4096, first = 0   no clear
    *    TA5_NO_FASTCLEAR=1              4096 of 4096, first = 0   no clear
    *
    * Every resolve-based silencer leaves depth at 0 because the resolve is
    * what applies the fast clear.  The blit destination is written the way
    * fd5_gmem.c:emit_gmem2mem_surf() writes it.
    */
   const bool slow_clear = getenv("TA5_SLOW_CLEAR") != NULL;

   const uint32_t clr_mask = 0x1; /* depth only; this driver has no stencil */

   if (slow_clear) {
      const struct ta5_image_level *slvl =
         &view->image->levels[MIN2(view->base_mip_level,
                                  view->image->mip_levels - 1)];

      ta5_pkt4(cmd, REG_A5XX_RB_BLIT_DST, 4);
      ret = ta5_cs_reloc(cmd, view->image->bound_memory->bo, 0, 0);
      if (ret != VK_SUCCESS)
         return ret;
      ta5_cs_push(cmd, A5XX_RB_BLIT_DST_PITCH((uint32_t)view->row_pitch));
      ta5_cs_push(cmd, A5XX_RB_BLIT_DST_ARRAY_PITCH((uint32_t)slvl->layer_pitch));
   }

   ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_CNTL, 1);
   ta5_cs_push(cmd, ((getenv("TA5_NO_FASTCLEAR") || slow_clear)
                        ? 0
                        : A5XX_RB_CLEAR_CNTL_FAST_CLEAR) |
                     A5XX_RB_CLEAR_CNTL_MASK(clr_mask));

   ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
   ta5_cs_push(cmd, packed);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);
   ta5_cs_push(cmd, 0x00000000);

   /*
    * The blit reaches the buffer through DEPTH_BUFFER_BASE, so a clear with no
    * base programmed clears address 0.  This is the one place that mattered
    * most to get right: a clear to the wrong address still leaves the CCU a
    * timestamp to wait for, so it reports success while doing nothing.
    */
   /*
    * The same five registers as ta5_emit_depth_state() writes, and for the same
    * reason: a blit that does not know the pitch clears a buffer of zeroes.
    * A single-layer attachment makes ARRAY_PITCH irrelevant, but PITCH is the
    * one that decides which rows get cleared.
    */
   {
      const struct ta5_image_level *lvl =
         &view->image->levels[MIN2(view->base_mip_level,
                                    view->image->mip_levels - 1)];

      /*
       * TA5_TRACE_DEPTH prints what the depth buffer is programmed with, next
       * to the size of the allocation it points at.  A pitch or an array pitch
       * that does not match the buffer is an out-of-bounds access, and the
       * kernel reports it as a translation fault at an address in no buffer at
       * all -- so this is what says whether the registers describe the
       * attachment or something else.
       */
      if (getenv("TA5_TRACE_DEPTH"))
         fprintf(stderr,
                 "ta5[depth] clear: fmt=%u attfmt=%u depth=%g packed=0x%08x "
                 "pitch=%llu array_pitch=%llu "
                 "bo_size=%u extent=%ux%u levels=%u\\n",
                 hwfmt, (unsigned)att->format, depth, packed,
                 (unsigned long long)view->row_pitch,
                 (unsigned long long)lvl->layer_pitch,
                 view->image->bound_memory->bo->size, view->image->extent.width,
                 view->image->extent.height, view->image->mip_levels);

      if (!no_bufinfo) {
         ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
         ta5_cs_push(cmd, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(
                            (enum a5xx_depth_format)hwfmt));
         ret = ta5_cs_reloc(cmd, view->image->bound_memory->bo, 0, 0);
         if (ret != VK_SUCCESS)
            return ret;
         ta5_cs_push(cmd,
                     A5XX_RB_DEPTH_BUFFER_PITCH((uint32_t)view->row_pitch));
         ta5_cs_push(cmd,
                     A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH(
                        (uint32_t)lvl->layer_pitch));

         /*
          * The rest of the descriptor, as fd5_gmem.c:emit_zs() writes it.
          *
          * The rasteriser's copy of the format ...
          */
         ta5_pkt4(cmd, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
         ta5_cs_push(cmd, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(
                            (enum a5xx_depth_format)hwfmt));

         /*
          * ... and the stencil side.  These three registers are what makes a
          * D24_UNORM_S8_UINT clear work, and their absence is why the cube
          * never appeared.
          *
          * DEPTH5_24_8 is the only one of this driver's three depth formats
          * that has a stencil aspect, so it is the only one whose CCU fast
          * clear needs the flag buffer described.  emit_zs() writes it for
          * every attachment; this driver wrote it only on detach, leaving it
          * at whatever the last context left behind, and the clear over
          * DEPTH5_24_8 was then dropped on the floor.
          *
          * Measured, 64x64, one depth clear per run, raw bytes of the
          * attachment afterwards:
          *
          *     D32_SFLOAT          0 of 4096 below 0.999  00 00 80 3f ...
          *     D16_UNORM           0 of 4096 below 0.999  ff ff ff ff ...
          *     D24_UNORM_S8_UINT   4096 of 4096 below 0.999  00 00 00 00 ...
          *
          * The last line is all zeroes and *no fault*: the clear was dropped,
          * not misdirected.  vkcube asks for D24_UNORM_S8_UINT on this
          * device, so the depth buffer stayed at zero, every fragment failed
          * its LESS test, and the frame came out as the clear colour with
          * nothing drawn -- which is exactly what the display showed, with
          * nothing in dmesg to explain it.
          *
          * There is no stencil buffer here, so this is zeroed the way GL
          * zeroes it, not pointed at one.
          */
         ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
         ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_FLAG_BUFFER_BASE */
         ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_FLAG_BUFFER_BASE_HI */
         ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_FLAG_BUFFER_PITCH */

         /*
          * ... and RB_STENCIL_INFO, which says whether the stencil plane
          * lives in its own buffer or in the depth buffer's own pixels.
          * fd5_gmem.c:emit_zs() closes with it: A5XX_RB_STENCIL_INFO
          * _SEPARATE_STENCIL plus a base and a pitch when there is a separate
          * stencil resource, and a plain zero when there is not.  This driver
          * has no separate stencil buffer, so the answer is zero -- and it was
          * never written at all, so the register kept whatever the last
          * context left in it.
          *
          * That is the register the D24_UNORM_S8_UINT clear was failing on,
          * and it is why only that format failed.  DEPTH5_24_8 is the only
          * one of the three depth formats with a stencil plane, so it is the
          * only one for which this bit means anything; with it stale, the CCU
          * would look for the stencil in a buffer that was never described,
          * find nothing it could resolve, and drop the whole clear.
          *
          * Measured on the attachment's raw bytes after one clear, 64x64:
          *
          *     D16_UNORM          ff ff ff ff ...   cleared
          *     D32_SFLOAT         00 00 80 3f ...   cleared
          *     D24_UNORM_S8_UINT  00 00 00 00 ...   all zero, no fault
          *
          * and vkcube asks for D24_UNORM_S8_UINT on this device.
          */
         ta5_pkt4(cmd, REG_A5XX_RB_STENCIL_INFO, 1);
         ta5_cs_push(cmd, 0x00000000); /* RB_STENCIL_INFO */
      }
   }

   if (device->blit_bo == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   if (!no_resolve) {
      /*
       * CCU_RESOLVE is the right event here, and this is where that was
       * measured rather than assumed: switching it for
       * PC_CCU_FLUSH_DEPTH_TS silences the faults described below, and costs
       * the depth writes -- four matrix cases (depth 0 and the three two-draw
       * cases) go from matching lavapipe to differing in 4096 of 16384 bytes,
       * because the depth the first draw wrote never reaches the buffer the
       * second draw tests against.  fd5_emit_blit() ends GL's depth clear the
       * same way (fd5_emit.h:139), so the event is right and the fault is
       * something else in this sequence.
       *
       * What is wrong: the depth clear faults the GPU at most sizes.  Measured
       * per frame, D32 depth, CCU_RESOLVE, two runs each:
       *
       *     64x64, 256x256, 512x512, 1024x1024   0 faults
       *     1023x1023, 1025x1025, 1080x1080      10 faults
       *     1080x2280 (the panel)                10 faults
       *
       * The fault addresses are outside every buffer the process allocates and
       * advance by 64 bytes, which is the CCU walking a target it has been
       * given the wrong address for.  GL refuses this clear outright for Z32
       * (fd5_draw.c: fd5_clear returns false when is_z32()), so the fast
       * clear of a Z32 attachment is not a thing this hardware does; what is
       * left to establish is what the attachment needs instead.
       */
      const char *ev = getenv("TA5_DEPTH_EVENT");
      uint32_t which = CCU_RESOLVE;

      if (ev && ev[0] == 'd')
         which = PC_CCU_FLUSH_DEPTH_TS;

      if (getenv("TA5_DEPTH_NOTS")) {
         ta5_pkt7(cmd, CP_EVENT_WRITE, 1);
         ta5_cs_push(cmd,
                      CP_EVENT_WRITE_0_EVENT((enum vgt_event_type)which));
      } else {
         /*
       * TA5_RESOLVE_MRT points the blit at a real target before the resolve.
       *
       * The resolve needs somewhere valid to resolve *into*.  Measured on the
       * fault storm, vkcube --wsi wayland against a compositor, everything
       * else held equal:
       *
       *    resolve with RB_BLIT_CNTL = BLIT_ZS      10 faults
       *    resolve after RB_BLIT_CNTL = BLIT_MRT0     0 faults
       *
       * and the protective pair is the blit target plus the clear data word,
       * not RB_CLEAR_CNTL: CLEAR_CNTL alone still gives 30, while
       * RB_BLIT_CNTL + RB_CLEAR_COLOR_DW0 without it gives 0.  A clear data
       * word of zero is fine; what matters is that the resolve has a target.
       */
      if (getenv("TA5_RESOLVE_MRT")) {
         /*
          * =2 waits for idle first.  The clear is asynchronous: moving the
          * blit target before the RB has consumed the ZS clear moves the
          * clear with it, which is how =1 silences the storm and still clears
          * depth to 0.
          */
         if (atoi(getenv("TA5_RESOLVE_MRT")) >= 2)
            ta5_wfi(cmd);

         ta5_pkt4(cmd, REG_A5XX_RB_BLIT_CNTL, 1);
         ta5_cs_push(cmd, A5XX_RB_BLIT_CNTL_BUF(BLIT_MRT0));
         ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
         ta5_cs_push(cmd, 0x00000000);
         ta5_cs_push(cmd, 0x00000000);
         ta5_cs_push(cmd, 0x00000000);
         ta5_cs_push(cmd, 0x00000000);
      }

      /*
       * TA5_RESOLVE_BLITDST describes the depth attachment through the blit's
       * own destination registers, which is what a resolve resolves into and
       * what fd5_gmem.c:emit_gmem2mem_surf() writes: RB_BLIT_DST,
       * RB_BLIT_DST_PITCH, RB_BLIT_DST_ARRAY_PITCH.  The clear has so far only
       * ever described the attachment through RB_DEPTH_BUFFER_INFO, which is
       * the render target, not the blit.
       *
       * This is the version that does not move the clear: TA5_RESOLVE_MRT
       * silences the storm by repointing RB_BLIT_CNTL away from BLIT_ZS, and
       * the clear then lands on the colour target -- depth comes out 0.0
       * instead of 1.0, with or without a wait-for-idle first.
       */
      if (getenv("TA5_RESOLVE_BLITDST")) {
         const struct ta5_image_level *lvl =
            &view->image->levels[MIN2(view->base_mip_level,
                                     view->image->mip_levels - 1)];

         ta5_pkt4(cmd, REG_A5XX_RB_BLIT_DST, 4);
         ret = ta5_cs_reloc(cmd, view->image->bound_memory->bo, 0, 0);
         if (ret != VK_SUCCESS)
            return ret;
         ta5_cs_push(cmd, A5XX_RB_BLIT_DST_PITCH((uint32_t)view->row_pitch));
         ta5_cs_push(cmd,
                     A5XX_RB_BLIT_DST_ARRAY_PITCH((uint32_t)lvl->layer_pitch));
      }

      if (!slow_clear) {
         ret = ta5_event_write_ts(cmd, device->blit_bo, which);
         if (ret != VK_SUCCESS)
            return ret;
      }
      }
   }

   ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_CNTL, 1);
   ta5_cs_push(cmd, 0);

   /* Back to rendering, not blitting. */
   ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
   ta5_cs_push(cmd, 0x00000008);

   return VK_SUCCESS;
}

/**
 * Depth test, depth write, and the depth buffer's address.
 *
 * Three registers:
 *
 *   RB_DEPTH_BUFFER_INFO   the attachment's hardware format
 *   RB_DEPTH_BUFFER_BASE   its address, as a relocation
 *   RB_DEPTH_CNTL          Z_TEST_ENABLE, Z_WRITE_ENABLE, ZFUNC
 *
 * RB_DEPTH_CNTL is a5xx's whole depth-test state -- there is no separate
 * compare register -- and ZFUNC takes enum adreno_compare_func values, which
 * line up with the Vulkan compare ops one for one.  The two enable bits are
 * independent on both sides: Vulkan has depthTestEnable and depthWriteEnable
 * as separate booleans, and the hardware has separate bits for the same reason.
 *
 * The base is the attachment image's level 0 with no offset, which is the only
 * case the render pass can currently produce.  A framebuffer may name any
 * level and the driver does not reject it, so this is correct for the level-0
 * case an application actually uses and is recorded here as the boundary.
 */
static VkResult
ta5_emit_depth_state(struct ta5_cmd_buffer *cmd,
                     const struct ta5_pipeline *pipe)
{
   const struct ta5_framebuffer *fb = cmd->bound_framebuffer;
   const struct ta5_render_pass *pass = fb->render_pass;
   /*
    * graphics_state.ds is the runtime's own copy: a struct vk_depth_stencil_state
    * with the flags unpacked into .depth and .stencil, not the
    * VkPipelineDepthStencilStateCreateInfo the application passed.  Reading it
    * through the create-info names does not compile, which is a kinder failure
    * than the alternative -- this state was simply never read before, so there
    * was nothing to keep in step with.
    */
   const struct vk_depth_stencil_state *ds = pipe->graphics_state.ds;
   bool test = ds && ds->depth.test_enable;
   bool write = ds && ds->depth.write_enable;
   enum adreno_compare_func func;
   struct ta5_image_view *view;
   uint32_t hwfmt;
   int idx;
   VkResult ret;

   idx = ta5_render_pass_depth_attachment(pass);
   if (getenv("TA5_TRACE_DS"))
      fprintf(stderr,
              "ta5[ds] emit: ds=%p test=%d write=%d op=%d idx=%d pipe=%p\n",
              (const void *)ds, (int)test, (int)write,
              ds ? (int)ds->depth.compare_op : -1, idx, (const void *)pipe);
   if (getenv("TA5_NO_DEPTH_STATE")) {
      ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_CNTL, 1);
      ta5_cs_push(cmd, 0x00000000);
      return VK_SUCCESS;
   }

   if (idx < 0) {
      /*
       * No depth attachment.  Both bits still have to be written as clear: a
       * draw that inherited the previous draw's register would depth-test
       * against a buffer it never declared.
       */
      ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_CNTL, 1);
      ta5_cs_push(cmd, 0x00000000);
      return VK_SUCCESS;
   }

   hwfmt = ta5_format_depth_hw(pass->attachments[idx].format);
   if (hwfmt == 0)
      return vk_error(&cmd->device->instance->vk,
                      VK_ERROR_FORMAT_NOT_SUPPORTED);

   switch (ds->depth.compare_op) {
   case VK_COMPARE_OP_NEVER:
      func = FUNC_NEVER;
      break;
   case VK_COMPARE_OP_LESS:
      func = FUNC_LESS;
      break;
   case VK_COMPARE_OP_EQUAL:
      func = FUNC_EQUAL;
      break;
   case VK_COMPARE_OP_LESS_OR_EQUAL:
      func = FUNC_LEQUAL;
      break;
   case VK_COMPARE_OP_GREATER:
      func = FUNC_GREATER;
      break;
   case VK_COMPARE_OP_NOT_EQUAL:
      func = FUNC_NOTEQUAL;
      break;
   case VK_COMPARE_OP_GREATER_OR_EQUAL:
      func = FUNC_GEQUAL;
      break;
   case VK_COMPARE_OP_ALWAYS:
      func = FUNC_ALWAYS;
      break;
   default:
      return vk_error(&cmd->device->instance->vk,
                      VK_ERROR_FORMAT_NOT_SUPPORTED);
   }

   view = fb->attachments[idx];

   /*
    * The whole depth-buffer block, as fd5_gmem.c:134 writes it:
    *
    *     RB_DEPTH_BUFFER_INFO        the format
    *     RB_DEPTH_BUFFER_BASE        the address, LO/HI
    *     RB_DEPTH_BUFFER_PITCH       bytes per row
    *     RB_DEPTH_BUFFER_ARRAY_PITCH bytes per layer
    *
    * The first version wrote only the first two, on the reasoning that the
    * other two are addressing details the unit could work out.  It cannot, and
    * with both left at zero the unit read every fragment's depth from the same
    * place: the depth *write* worked, the two-draw cases passed, and every case
    * whose fragment should have been rejected let it through anyway --
    * including VK_COMPARE_OP_NEVER, which rejects unconditionally.  That is the
    * signature of a depth test that is not reading the buffer it was pointed
    * at, not of a bad compare op.
    */
   {
      const struct ta5_image_level *lvl =
         &view->image->levels[MIN2(view->base_mip_level,
                                    view->image->mip_levels - 1)];

      ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
      ta5_cs_push(cmd, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(
                         (enum a5xx_depth_format)hwfmt));
      ret = ta5_cs_reloc(cmd, view->image->bound_memory->bo, 0, 0);
      if (ret != VK_SUCCESS)
         return ret;
      ta5_cs_push(cmd, A5XX_RB_DEPTH_BUFFER_PITCH((uint32_t)view->row_pitch));
      ta5_cs_push(cmd, A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH(
                         (uint32_t)lvl->layer_pitch));

      /*
       * The rasteriser side of the same format.  fd5_gmem.c:147 writes this
       * alongside RB_DEPTH_BUFFER_INFO, and without it the unit never reads
       * the buffer the RB pointed at.
       */
      ta5_pkt4(cmd, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
      ta5_cs_push(cmd, A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(
                         (enum a5xx_depth_format)hwfmt));

      /*
       * And the stencil side, which fd5_gmem.c:150 writes immediately after.
       * DEPTH5_24_8 has a stencil aspect and the other two do not, so this is
       * the register that decides whether a D24_UNORM_S8_UINT attachment is
       * described completely or only halfway -- see ta5_emit_clear_depth().
       * Zeroed, because this driver allocates no separate stencil buffer.
       */
      ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
      ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_FLAG_BUFFER_BASE */
      ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_FLAG_BUFFER_BASE_HI */
      ta5_cs_push(cmd, 0x00000000); /* RB_DEPTH_FLAG_BUFFER_PITCH */

      /*
       * And whether the stencil plane is packed into the depth buffer, which
       * here it is: fd5_gmem.c:emit_zs() writes a plain zero for that case and
       * SEPARATE_STENCIL when the stencil has a buffer of its own.  Only
       * DEPTH5_24_8 has a stencil plane, so only that format is affected --
       * see ta5_emit_clear_depth() for what it cost.
       */
      ta5_pkt4(cmd, REG_A5XX_RB_STENCIL_INFO, 1);
      ta5_cs_push(cmd, 0x00000000); /* RB_STENCIL_INFO */
   }

   /*
    * Z_READ_ENABLE is what makes the unit fetch and compare rather than write
    * unconditionally.  It goes in only with Z_TEST_ENABLE: with the test off,
    * leaving it set is a state fd5 never emits.
    */
   uint32_t dcntl =
      (test ? A5XX_RB_DEPTH_CNTL_Z_TEST_ENABLE : 0) |
      (write ? A5XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE : 0) |
      A5XX_RB_DEPTH_CNTL_ZFUNC(func) |
      (test ? A5XX_RB_DEPTH_CNTL_Z_READ_ENABLE : 0);
   ta5_pkt4(cmd, REG_A5XX_RB_DEPTH_CNTL, 1);
   ta5_cs_push(cmd, dcntl);

   if (getenv("TA5_TRACE_DEPTH"))
      fprintf(stderr,
              "ta5[depth] idx=%d fmt=%d hwfmt=%u test=%d write=%d op=%d "
              "cntl=%08x bo=%p\n",
              idx, pass->attachments[idx].format, hwfmt, (int)test, (int)write,
              (int)ds->depth.compare_op, dcntl,
              (void *)view->image->bound_memory->bo);

   return VK_SUCCESS;
}

/** End of the render pass: flush colour out of the CCU. */
static VkResult
ta5_emit_sysmem_fini(struct ta5_cmd_buffer *cmd)
{
   VkResult ret;

   /*
    * TA5_TRACE_ZBUF=<path> writes the depth attachment at the end of the
    * render pass, as a binary PGM of the raw depth words.
    *
    * This is the one measurement that separates "the fragments were never
    * rasterised" from "they were rasterised and then thrown away".  Colour
    * cannot answer it: a fragment killed by the depth test and a fragment
    * that never existed leave identical pixels.  Depth can, because a
    * fragment that passes the test writes its depth -- the test and the write
    * are one register, Z_TEST_ENABLE and Z_WRITE_ENABLE together in
    * RB_DEPTH_CNTL -- so whatever is in here is what the test accepted.
    */
   {
      const char *zpath = getenv("TA5_TRACE_ZBUF");
      const struct ta5_framebuffer *fb = cmd->bound_framebuffer;

      if (zpath && fb) {
         const struct ta5_render_pass *pass = fb->render_pass;
         int idx = ta5_render_pass_depth_attachment(pass);

         if (idx >= 0) {
            struct ta5_image_view *view = fb->attachments[idx];
            uint32_t fmt = (uint32_t)ta5_format_depth_hw(pass->attachments[idx].format);
            uint8_t *map = NULL;

            if (view->image->bound_memory)
               map = (uint8_t *)ta5_bo_map(view->image->bound_memory->bo);
            FILE *f = map ? fopen(zpath, "wb") : NULL;

            if (!f) {
               fprintf(stderr, "ta5[zbuf] %s\n", map ? "cannot open" : "map failed");
            } else {
               uint32_t w = view->image->extent.width;
               uint32_t h = view->image->extent.height;
               uint32_t pitch = view->row_pitch;

               fprintf(f, "P5\n%u %u\n%d\n", w, h,
                       (fmt == 1) ? 65535 : (fmt == 2 ? 16777215 : 255));
               for (uint32_t y = 0; y < h; y++) {
                  const uint8_t *row = map + (size_t)y * pitch;

                  for (uint32_t x = 0; x < w; x++) {
                     uint32_t v;
                     if (fmt == 1) {
                        uint16_t t;
                        memcpy(&t, row + x * 2, 2);
                        v = t;
                     } else if (fmt == 2) {
                        uint32_t t;
                        memcpy(&t, row + x * 4, 4);
                        v = t & 0x00ffffffu;
                     } else {
                        memcpy(&v, row + x * 4, 4);
                        v = v >> 16;   /* float depth, top 8 bits */
                     }
                     fputc((int)(v >> ((fmt == 2) ? 16 : 8)) & 0xff, f);
                  }
               }
               fclose(f);
               fprintf(stderr,
                       "ta5[zbuf] wrote %s %ux%u fmt=%u pitch=%u\n", zpath, w, h,
                       fmt, pitch);
            }
         }
      }
   }

   ta5_pkt7(cmd, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
   ta5_cs_push(cmd, 0);

   /*
    * fd5_emit_lrz_flush()'s sequence, MINUS the enable: the trailing disable is
    * part of that sequence, but the enable cannot go out.
    *
    * LRZ needs a flag buffer.  freedreno allocates one per depth resource and
    * points RB_DEPTH_FLAG_BUFFER_BASE at it; this driver allocates none, and
    * the register is left at zero -- written as a literal 0 in
    * ta5_emit_sysmem_prep's depth block and never given a real address.  With
    * the enable set, the LRZ unit walks the flag buffer from address 0 in
    * 64-byte strides: thousands of translation faults from iova 0, the
    * "null-IOVA" signature, and the walk is long enough to leave the first
    * unmapped page whenever the framebuffer's height is not a multiple of 8.
    *
    * Measured at 360x713 with a depth attachment, 120 frames, no wayland:
    * depth test off, 10 fault bursts; depth test on, 0; the walk scales with
    * the framebuffer, which is why 1080x2280 (both multiples of 8) was clean
    * and 1023x1023 and 1025x1025 were not -- the same sizes that faulted in
    * the original depth-clear investigation.
    *
    * LRZ is an optimisation.  Until a flag buffer exists, the enable stays off;
    * the invalidate and the trailing zero still go out, so the sequence the
    * hardware sees is well-formed.
    */
   ta5_pkt4(cmd, REG_A5XX_GRAS_LRZ_CNTL, 1);
   ta5_cs_push(cmd, getenv("TA5_LRZ_ENABLE") ? A5XX_GRAS_LRZ_CNTL_ENABLE
                                             : 0x00000000);
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
   /*
    * TA5_NO_CCU_FLUSH: emit neither timestamped CCU flush.  This is the A/B
    * for the compositor-killing fault storm, which fires with a content-free
    * submission -- TA5_SKIP_DRAW=1, no draw state, no shaders, no
    * CP_DRAW_INDX_OFFSET -- and so cannot be about anything our GPU renders.
    * What every submission carries regardless of content is this CCU flush
    * pair, and the CCU is GPU-global: invalidating it mid-walk is exactly the
    * shape of another context's GPU work faulting from iova 0 in 64-byte
    * strides.  Rendering without a flush produces stale output, not wrong
    * output; that trade is what this knob is for.
    */
   if (!getenv("TA5_NO_CCU_FLUSH")) {
      ret = ta5_event_write_ts(cmd, cmd->device->blit_bo,
                               PC_CCU_FLUSH_COLOR_TS);
      if (ret != VK_SUCCESS)
         return ret;

      ret = ta5_event_write_ts(cmd, cmd->device->blit_bo,
                               PC_CCU_FLUSH_DEPTH_TS);
      if (ret != VK_SUCCESS)
         return ret;
   }

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

/**
 * Record the dynamic viewport and scissor.
 *
 * a5xx has one viewport and one scissor rectangle.  Vulkan says a call naming
 * an index or count the bound pipeline does not declare is undefined; this
 * ignores it, which is what the spec permits, rather than corrupting the
 * rectangle for viewport 0.
 *
 * The viewport itself is still derived from the framebuffer at draw time --
 * see the note on ta5_emit_draw() -- so this only records it for the day that
 * is fixed.  It is validated and stored now so that fixing it later cannot
 * turn a rejected call into a silently accepted one.
 */
VKAPI_ATTR void VKAPI_CALL
ta5_CmdSetViewport(VkCommandBuffer _cmd, uint32_t firstViewport,
                   uint32_t viewportCount, const VkViewport *pViewports)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   if (firstViewport != 0 || viewportCount != 1 || pViewports == NULL)
      return;

   const VkViewport *vp = &pViewports[0];

   /* Vulkan VUID-vkCmdSetViewport-pViewports-parameter: the width and height
    * must be positive or both negative (zero is neither), and minDepth must not
    * exceed maxDepth.  Either is undefined behaviour, so it is dropped rather
    * than guessed at. */
   if (!((vp->width > 0.0f && vp->height > 0.0f) ||
         (vp->width < 0.0f && vp->height < 0.0f)))
      return;
   if (vp->minDepth > vp->maxDepth)
      return;

   cmd->viewport = *vp;
   cmd->viewport_set = true;
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdSetScissor(VkCommandBuffer _cmd, uint32_t firstScissor,
                  uint32_t scissorCount, const VkRect2D *pScissors)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   if (firstScissor != 0 || scissorCount != 1 || pScissors == NULL)
      return;

   /* Vulkan VUID-vkCmdSetScissor-pScissors-parameter: offset must be
    * non-negative and the extents must not be negative. */
   if (pScissors->offset.x < 0 || pScissors->offset.y < 0 ||
       pScissors->extent.width < 0 || pScissors->extent.height < 0)
      return;

   cmd->scissor = pScissors[0];
   cmd->scissor_set = true;
}

/**
 * Vertex attribute format as a VFMT5 value, or UINT32_MAX if unsupported.
 *
 * Only the 32-bit float forms the fetch unit decodes natively are mapped.
 * Anything else is reported as unsupported so the attribute is skipped rather
 * than silently decoded as something else, which would feed the shader
 * garbage that looks like a pipeline bug rather than a format one.
 */
static uint32_t
ta5_vfd_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R32_SFLOAT:
      return VFMT5_32_FLOAT;
   case VK_FORMAT_R32G32_SFLOAT:
      return VFMT5_32_32_FLOAT;
   case VK_FORMAT_R32G32B32_SFLOAT:
      return VFMT5_32_32_32_FLOAT;
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return VFMT5_32_32_32_32_FLOAT;
   default:
      return UINT32_MAX;
   }
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdBindVertexBuffers(VkCommandBuffer _cmd, uint32_t firstBinding,
                         uint32_t bindingCount,
                         const VkBuffer *pBuffers,
                         const VkDeviceSize *pOffsets)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   /*
    * Binding 0 rebinds the whole array, so the stale slots below it are
    * cleared first; anything above firstBinding + bindingCount keeps the
    * binding it already had, exactly as the spec requires.  A slot past the
    * end of the array is dropped rather than silently landing somewhere else.
    */
   if (firstBinding == 0) {
      memset(cmd->bound_vb, 0, sizeof(cmd->bound_vb));
      memset(cmd->bound_vb_offset, 0, sizeof(cmd->bound_vb_offset));
   }

   for (uint32_t i = 0; i < bindingCount; i++) {
      uint32_t slot = firstBinding + i;

      if (slot >= MESA_VK_MAX_VERTEX_BINDINGS)
         break;

      cmd->bound_vb[slot] = pBuffers ? ta5_buffer_from_handle(pBuffers[i])
                                     : NULL;
      cmd->bound_vb_offset[slot] = pOffsets ? pOffsets[i] : 0;
   }
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
   uint32_t clear_count = pRenderPassBegin->clearValueCount;
   VkResult ret;

   (void)contents;

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   cmd->bound_render_pass = pass;
   cmd->bound_framebuffer =
      ta5_framebuffer_from_handle(pRenderPassBegin->framebuffer);

   /* The scissor is clipped to the render area, so the area has to be known
    * before any draw is recorded.  Stored rather than emitted: it only affects
    * the scissor the draw computes. */
   cmd->render_area = pRenderPassBegin->renderArea;

   /*
    * pClearValues is NULL whenever no attachment asks to be cleared, so it is
    * indexed only under `clear`, not under `clear_count`: clearValueCount is
    * then 0 and every loadOp below is LOAD or DONT_CARE, so the loop never
    * reaches the array.  Reading clear[i] on the strength of clear_count alone
    * would be the bug, not the guard.
    */

   /* Make room for the worst case before emitting, so the grow cannot move
    * the stream underneath a relocation we have already recorded. */
   ret = ta5_cmd_buffer_alloc_cs(cmd, cmd->cs_count + 4096);
   if (ret != VK_SUCCESS)
      return;

   if (getenv("TA5_PREP_BLIT")) {
      /* The colour clear's blit setup and nothing else: no clear, no colour
       * write, just the mode and the target, so that the resolve that follows
       * has the same blit context the safe case has. */
      ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
      ta5_cs_push(cmd, 0x00000000);
      ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
      ta5_cs_push(cmd, 0x00000008);
      ta5_pkt4(cmd, REG_A5XX_RB_BLIT_CNTL, 1);
      ta5_cs_push(cmd, A5XX_RB_BLIT_CNTL_BUF(BLIT_MRT0));

      /* =2 adds the clear's own registers, so this becomes the colour clear
       * with its trailing restores left off. */
      if (atoi(getenv("TA5_PREP_BLIT")) >= 2) {
         ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_CNTL, 1);
         ta5_cs_push(cmd, A5XX_RB_CLEAR_CNTL_FAST_CLEAR |
                           A5XX_RB_CLEAR_CNTL_MASK(0xf));
         ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
         ta5_cs_push(cmd, 0x00000000);
         ta5_cs_push(cmd, 0x00000000);
         ta5_cs_push(cmd, 0x00000000);
         ta5_cs_push(cmd, 0x00000000);
      }

      /* =3 adds the restores the clear puts *after* its resolve, so this is
       * the colour clear whole.  If that is clean where =2 faults, then the
       * resolve cannot be the last thing in the stream. */
      if (atoi(getenv("TA5_PREP_BLIT")) >= 3) {
         ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_CNTL, 1);
         ta5_cs_push(cmd, 0x00000000);
         ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
         ta5_cs_push(cmd, 0x00000008);
         ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
         ta5_cs_push(cmd, 0x00000008);
      }
   }

   if (getenv("TA5_PREP_RESOLVE"))
      ta5_event_write_ts(cmd, cmd->device->blit_bo, CCU_RESOLVE);

   if (getenv("TA5_EMPTY_IB")) {
      cmd->bound_render_pass = pass;
      return;
   }

   const char *padenv = getenv("TA5_PAD");
   for (int p = 0; padenv && p < atoi(padenv); p++)
      ta5_cs_push(cmd, 0x00000000);

   if (!getenv("TA5_SKIP_PREP")) {
      ret = ta5_emit_sysmem_prep(cmd, cmd->bound_framebuffer);
      if (ret != VK_SUCCESS)
         return;
   }

   /*
    * Attachments are cleared in the order pAttachments lists them, which is
    * not "colour first".  The old code looked only at attachments[0] and
    * assumed it was colour, which is what made a depth-only or depth-first
    * attachment clear go to a colour target.
    */
   for (uint32_t i = 0; !getenv("TA5_SKIP_CLEAR") && i < pass->attachment_count;
        i++) {
      const struct ta5_render_pass_attachment *att = &pass->attachments[i];
      const struct ta5_framebuffer *fb = cmd->bound_framebuffer;

      if (att->load_op != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      /*
       * An attachment with loadOp CLEAR always has an entry in pClearValues
       * (VkRenderPassBeginInfo's VUID), so this is a bounds check on the
       * application's promise rather than a fallback.  pClearValues is NULL
       * when nothing is cleared, which is the case that matters: without this
       * the loop would dereference NULL for every non-clearing attachment that
       * had a stale load_op.
       */
      if (i >= clear_count || clear == NULL)
         return;

      if (att->used_as_color && getenv("TA5_SKIP_CLEAR_COLOR"))
         continue;

      if (!att->used_as_color && getenv("TA5_SKIP_CLEAR_DEPTH"))
         continue;

      if (att->used_as_color) {
         /*
          * One RB_BLIT_CNTL_BUF(BLIT_MRT0) clears the whole colour buffer, so
          * a second colour attachment would be cleared with the first one's
          * value.  Only one colour attachment is supported.
          */
         union ta5_color c = ta5_color_from_clear(
            clear[i].color,
            att->format == VK_FORMAT_B8G8R8A8_SRGB ||
               att->format == VK_FORMAT_R8G8B8A8_SRGB);
         if (ta5_emit_clear(cmd, cmd->device, &c) != VK_SUCCESS)
            return;
      } else {
         if (ta5_emit_clear_depth(cmd, cmd->device, att, fb->attachments[i],
                                  clear[i].depthStencil.depth) != VK_SUCCESS)
            return;
      }
   }
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

   const char *padend = getenv("TA5_PADEND");
   for (int p = 0; padend && p < atoi(padend); p++)
      ta5_cs_push(cmd, 0x00000000);

   if (getenv("TA5_EMPTY_IB") || getenv("TA5_SKIP_FINI")) {
      const char *endres = getenv("TA5_END_RESOLVE");

      /*
       * =2 puts the colour clear's own register sequence here, *after* the
       * prep, which is where the real colour clear runs.  TA5_PREP_BLIT put
       * the same sequence *before* the prep, so "the hand-rolled colour clear
       * still faults" was really "a clear before the prep faults".  Position
       * is the only variable left between those two results.
       */
      if (endres && atoi(endres) >= 3) {
         /* =3: blit mode only -- still 30 faults, so blit mode is not it.
          * =4: RB_CLEAR_CNTL only.
          * =5: everything except RB_CLEAR_CNTL. */
         if (atoi(endres) != 5) {
            ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_CNTL, 1);
            ta5_cs_push(cmd, A5XX_RB_CLEAR_CNTL_FAST_CLEAR |
                              A5XX_RB_CLEAR_CNTL_MASK(0xf));
         }

         if (atoi(endres) != 4) {
            ta5_pkt4(cmd, REG_A5XX_RB_BLIT_CNTL, 1);
            ta5_cs_push(cmd, A5XX_RB_BLIT_CNTL_BUF(BLIT_MRT0));
            ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
            ta5_cs_push(cmd, 0x00000000);
            ta5_cs_push(cmd, 0x00000000);
            ta5_cs_push(cmd, 0x00000000);
            ta5_cs_push(cmd, 0x00000000);
         }

         if (atoi(endres) == 3) {
            ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
            ta5_cs_push(cmd, 0x00000000);
            ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
            ta5_cs_push(cmd, 0x00000008);
         }
      }

      if (endres && atoi(endres) == 2) {
         ta5_pkt4(cmd, REG_A5XX_RB_RENDER_CNTL, 1);
         ta5_cs_push(cmd, 0x00000000);
         ta5_pkt4(cmd, REG_A5XX_GRAS_SC_CNTL, 1);
         ta5_cs_push(cmd, 0x00000008);
         ta5_pkt4(cmd, REG_A5XX_RB_BLIT_CNTL, 1);
         ta5_cs_push(cmd, A5XX_RB_BLIT_CNTL_BUF(BLIT_MRT0));
         ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_CNTL, 1);
         ta5_cs_push(cmd, A5XX_RB_CLEAR_CNTL_FAST_CLEAR |
                           A5XX_RB_CLEAR_CNTL_MASK(0xf));
         ta5_pkt4(cmd, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
         ta5_cs_push(cmd, 0x00000000);
         ta5_cs_push(cmd, 0x00000000);
         ta5_cs_push(cmd, 0x00000000);
         ta5_cs_push(cmd, 0x00000000);
      }

      if (endres)
         ta5_event_write_ts(cmd, cmd->device->blit_bo, CCU_RESOLVE);

      if (endres)
         return;
   }

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

/**
 * vkCmdCopyBufferToImage -- a CP_MEMCPY.
 *
 * This is the first transfer the driver has.  It was written to make mipmaps
 * reachable: images are linear-tiled, a linear image with mipLevels != 1 is
 * rejected outright (ta5_memory.cc), and the only way to fill one is a
 * transfer, so mip coverage and every application that uploads a texture --
 * vkcube's cubemap among them -- were both blocked on this call existing.
 *
 * The copy is a plain byte move with no shader involved, because a linear
 * image is a linear image in memory.  CP_MEMCPY is
 * adreno_pm4.xml:1398:
 *
 *     reg0 DWORDS
 *     reg1 SRC_LO   reg2 SRC_HI
 *     reg3 DST_LO   reg4 DST_HI
 *
 * and it is valid on this part: the opcode carries variants="A6XX-", which is
 * this generation and below.
 *
 * Five payload dwords, so CP_MEMCPY(5).  ta5_pkt7()'s count is payload rather
 * than total (see its comment), and the count is what tells the CP how much to
 * consume: at 6 it reads one dword past the packet, into the next packet's
 * header, and the copy silently does nothing.  Matched against
 * rdcompiler-utils.h:401, which emits pkt7(cs, CP_MEMCPY, 5) for the same
 * packet.  That mistake is why the first attempt reported "COPY DID NOT LAND"
 * on every level with the relocations correctly patched.
 *
 * One memcpy per row rather than one for the whole region, because the source
 * pitch comes from the buffer and the destination pitch from the image level,
 * and they are not required to agree.  The common full-width case still
 * degenerates to a single packet per row, and a region that exactly fills a
 * level's rows coalesces to one packet because the pitches match.
 */
VKAPI_ATTR void VKAPI_CALL
ta5_CmdCopyBufferToImage(VkCommandBuffer _cmd, VkBuffer srcBuffer,
                         VkImage dstImage, VkImageLayout dstImageLayout,
                         uint32_t regionCount,
                         const VkBufferImageCopy *pRegions)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);
   struct ta5_buffer *buffer = ta5_buffer_from_handle(srcBuffer);
   struct ta5_image *image = ta5_image_from_handle(dstImage);
   void *src_map, *dst_map;
   uint32_t block_size;

   (void)dstImageLayout;

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   if (!buffer || !image || !pRegions || regionCount == 0)
      return;

   if (!buffer->bound_memory || !image->bound_memory)
      return;

   block_size = ta5_format_block_size(image->format);
   if (!block_size)
      return;

   /*
    * A host copy, not a CP_MEMCPY.
    *
    * CP_MEMCPY is the right instruction and it is available here (the opcode
    * lives in the shared pm4 set that a2xx..a6xx all include, not in
    * a5xx.xml).  It was tried first, with the packet laid out exactly as
    * rdcompiler-utils.h:401 emits it -- pkt7(CP_MEMCPY, 5), then DWORDS, then
    * the 64-bit source, then the 64-bit destination -- and the relocations
    * patched correctly, and the copy still landed nothing: every level of
    * every layer read back as the allocation's initial contents.
    *
    * This driver advertises one heap and one memory type, and both objects
    * here are in it, so the two buffers are host-visible and the copy is a
    * memcpy.  Vulkan does not require a transfer to be executed by the
    * copy engine -- any correct implementation is conformant -- and for a
    * driver with a unified memory heap this is the implementation that is
    * simple enough to be obviously right.
    *
    * A tiled or discrete-memory implementation would use CP_MEMCPY here, and
    * that path is still untested on this part.  It is not silently fine
    * there: a wrong packet count makes the CP read one dword past the packet
    * and into the next packet's header, which is exactly the failure measured.
    *
    * The work happens at record time, before the submission is queued.  That
    * is only safe while the application is not already executing a previous
    * submission against these objects, which is the case for every upload
    * pattern seen so far; a driver that wanted to be safe unconditionally
    * would defer the copy to submit time.
    */
   src_map = ta5_bo_map(buffer->bound_memory->bo);
   dst_map = ta5_bo_map(image->bound_memory->bo);
   if (!src_map || !dst_map) {
      if (getenv("TA5_TRACE_COPY"))
         fprintf(stderr, "ta5[copy] mmap failed\n");
      return;
   }

   for (uint32_t r = 0; r < regionCount; r++) {
      const VkBufferImageCopy *reg = &pRegions[r];
      uint32_t mip, cols, rows;
      const struct ta5_image_level *lvl;
      VkDeviceSize dst_base, src_pitch, dst_pitch;

      /* Clamp against the level rather than the image, so a caller that gets
       * this wrong copies less instead of walking off the allocation. */
      mip = MIN2(reg->imageSubresource.mipLevel,
                 MAX2(image->mip_levels, 1u) - 1);
      lvl = &image->levels[mip];

      cols = reg->imageExtent.width;
      rows = reg->imageExtent.height;
      if (!cols || !rows)
         continue;
      if (mip != reg->imageSubresource.mipLevel || cols > lvl->width ||
          rows > lvl->height)
         continue;

      dst_pitch = lvl->row_pitch;
      src_pitch = reg->bufferRowLength ? reg->bufferRowLength * block_size
                                       : dst_pitch;

      dst_base = image->offset + lvl->offset +
                 (VkDeviceSize)reg->imageSubresource.baseArrayLayer *
                    lvl->layer_pitch +
                 (VkDeviceSize)reg->imageOffset.y * dst_pitch +
                 (VkDeviceSize)reg->imageOffset.x * block_size;

      for (uint32_t y = 0; y < rows; y++) {
         const uint8_t *s = (const uint8_t *)src_map + reg->bufferOffset +
                            (VkDeviceSize)y * src_pitch;
         uint8_t *d = (uint8_t *)dst_map + dst_base + (VkDeviceSize)y * dst_pitch;

         /* Row by row: the two pitches are independent, and only when they
          * agree is the whole region one contiguous move. */
         if (src_pitch == dst_pitch) {
            memcpy(d, s, (size_t)cols * block_size);
         } else {
            for (uint32_t x = 0; x < cols; x++)
               memcpy(d + (size_t)x * block_size, s + (size_t)x * block_size,
                      block_size);
         }
      }

      if (getenv("TA5_TRACE_COPY"))
         fprintf(stderr,
                 "ta5[copy] region %u mip=%u %ux%u off=%u,%u layer=%u "
                 "srcpitch=%llu dstpitch=%llu dst=%llu\n",
                 r, mip, cols, rows, (unsigned)reg->imageOffset.x,
                 (unsigned)reg->imageOffset.y,
                 reg->imageSubresource.baseArrayLayer,
                 (unsigned long long)src_pitch,
                 (unsigned long long)dst_pitch, (unsigned long long)dst_base);
   }
}

/*
 * vkCmdBindIndexBuffer / vkCmdDrawIndexed.
 *
 * vkcube draws its cube from an index buffer.  Without these two entry points
 * its draw was recorded with no index buffer bound, so the VFD synthesised
 * vertex ids and the frame came out as the clear colour: 13 consecutive
 * presented frames, all byte-identical, all one colour (0x333333), zero faults.
 * The pixels were never wrong, there was simply nothing drawn.
 */
VKAPI_ATTR void VKAPI_CALL
ta5_CmdBindIndexBuffer(VkCommandBuffer _cmd, VkBuffer buffer,
                       VkDeviceSize offset, VkIndexType indexType)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   cmd->index_bo = NULL;
   cmd->index_buffer_offset = 0;
   cmd->index_count = 0;
   cmd->index_type = indexType;

   if (buffer == VK_NULL_HANDLE)
      return;   /* an unbound index buffer is legal and leaves AUTO_INDEX */

   struct ta5_buffer *buf = ta5_buffer_from_handle(buffer);
   if (getenv("TA5_TRACE_NULL"))
      fprintf(stderr, "ta5: BindIndexBuffer buffer=%p buf=%p mem=%p off=%llu type=%d\n",
              (void *)buffer, (void *)buf, (void *)(buf ? (void *)buf->bound_memory : NULL),
              (unsigned long long)offset, (int)indexType);
   if (!buf || !buf->bound_memory)
      return;

   cmd->index_bo = buf->bound_memory->bo;
   cmd->index_buffer_offset = offset;
   /*
    * MaxIndices is how many indices the buffer holds, which is what fd5 hands
    * the hardware (fd5_draw.h:109: idx_buffer->width0 / index_size).  It bounds
    * the fetch; nothing in vkcube's draw reaches it, but a wrong value here is
    * how the VFD ends up walking off the end of the buffer.
    */
   cmd->index_count = (uint32_t)((buf->bound_memory->bo->size - offset) /
                                 (indexType == VK_INDEX_TYPE_UINT16 ? 2 : 4));
}

/**
 * Record one draw: state, program state, shaders, and the draw packet.
 *
 * `index_size` is 0 for vkCmdDraw (no index buffer, the VFD synthesises vertex
 * ids) and 2 or 4 for vkCmdDrawIndexed, which is what makes the packet emit its
 * seven-dword indexed form.
 */
static void
ta5_record_draw(struct ta5_cmd_buffer *cmd, uint32_t vertexCount,
                uint32_t instanceCount, uint32_t firstVertex,
                uint32_t firstInstance, uint32_t index_size)
{
   struct ta5_pipeline *pipe = cmd->bound_pipeline;
   struct ta5_cmd_buffer ib_cmd;
   struct ta5_cmd_buffer *draw = cmd;
   bool use_ib = !getenv("TA5_NO_INDIRECT_DRAW");
   unsigned vs_tex_count = 0, fs_tex_count = 0;
   VkResult ret;

   /*
    * Driver parameters are recorded per draw, so remember what this one is
    * before anything emits the shader.
    */
   cmd->draw_first_vertex = firstVertex;
   cmd->draw_first_instance = firstInstance;
   cmd->draw_index_size = index_size;

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING || pipe == NULL)
      return;

   if (getenv("TA5_SKIP_DRAW"))
      return;

   ret = ta5_cmd_buffer_alloc_cs(cmd, cmd->cs_count + (use_ib ? 4096 : 8192));
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
       ret = ta5_emit_clear(cmd, cmd->device, &probe);
       if (ret != VK_SUCCESS)
          return;
   }

   if (use_ib) {
      /*
       * fd5 submits the draw state and the draw itself as an
       * MSM_SUBMIT_CMD_IB_TARGET_BUF.  Build that stream independently so its
       * relocations can be attached to the IB-target command in the submit
       * ioctl; the outer stream keeps the clear/prep and a relocated
       * CP_INDIRECT_BUFFER that enters it.
       */
      ib_cmd = *cmd;
      ib_cmd.cs = NULL;
      ib_cmd.cs_size = 0;
      ib_cmd.cs_count = 0;
      ib_cmd.relocs = NULL;
      ib_cmd.reloc_size = 0;
      ib_cmd.reloc_count = 0;
      ib_cmd.reloc_bos = NULL;
      ib_cmd.reloc_bo_count = 0;
      draw = &ib_cmd;

      ret = ta5_cmd_buffer_alloc_cs(draw, 8192);
      if (ret != VK_SUCCESS) {
         vk_free(&cmd->device->vk.alloc, draw->cs);
         return;
      }
   }

   /*
    * Viewport transform.  The vertex shader produces NDC, so this is the
    * width/height-to-half-size mapping *plus* the half-size offset that puts
    * NDC -1 at pixel 0.  Without the offset the whole frame lands off the
    * left and bottom edges and the draw rasterises nothing.  GL's stream
    * carries 32/32 offsets and 0.5/0.5 for z on a 64x64 target.
    *
    * Derived from the framebuffer rather than from vkCmdSetViewport, which is
    * a separate, still-open gap: this emits the viewport the whole harness has
    * always used and nothing depends on the recorded one yet.
    */
   {
      uint32_t w = cmd->bound_framebuffer->width;
      uint32_t h = cmd->bound_framebuffer->height;
      uint32_t sx0, sy0, sx1, sy1;

      /*
       * Effective scissor: the rectangle Vulkan specifies, as inclusive
       * GRAS bounds.
       *
       * Precedence, per the dynamic-state rules:
       *
       *   - scissor is dynamic and has been set  -> that rectangle
       *   - scissor is not dynamic              -> the pipeline's pScissors[0],
       *                                           which the runtime deep
       *                                           copied into graphics_state
       *   - dynamic but never set               -> the render area
       *
       * and then intersected with the render area, which Vulkan does not leave
       * optional: a scissor reaching outside the render area is clipped to it
       * rather than being an error.
       *
       * GRAS bounds are inclusive, Vulkan extents are not, hence the -1s.
       */
      const char *scissor_src = "render_area";
      {
         VkRect2D r;

         if (BITSET_TEST(pipe->graphics_state.dynamic,
                         MESA_VK_DYNAMIC_VP_SCISSORS) &&
             draw->scissor_set) {
            r = draw->scissor;
            scissor_src = "dynamic";
         } else if (!BITSET_TEST(pipe->graphics_state.dynamic,
                                 MESA_VK_DYNAMIC_VP_SCISSORS) &&
                    pipe->graphics_state.vp &&
                    pipe->graphics_state.vp->scissor_count > 0) {
            r = pipe->graphics_state.vp->scissors[0];
            scissor_src = "pipeline";
         } else {
            r = draw->render_area;
         }

         int64_t rx0 = MAX2((int64_t)r.offset.x,
                            (int64_t)draw->render_area.offset.x);
         int64_t ry0 = MAX2((int64_t)r.offset.y,
                            (int64_t)draw->render_area.offset.y);
         int64_t rx1 = MIN2((int64_t)r.offset.x + r.extent.width,
                            (int64_t)draw->render_area.offset.x +
                               draw->render_area.extent.width);
         int64_t ry1 = MIN2((int64_t)r.offset.y + r.extent.height,
                            (int64_t)draw->render_area.offset.y +
                               draw->render_area.extent.height);

         /* Also clip to the attachment itself: a render area larger than the
          * framebuffer would otherwise ask for bounds the 15-bit scissor
          * fields cannot hold. */
         rx0 = MAX2(rx0, 0);
         ry0 = MAX2(ry0, 0);
         rx1 = MIN2(rx1, (int64_t)w);
         ry1 = MIN2(ry1, (int64_t)h);

         if (rx1 <= rx0 || ry1 <= ry0) {
            /*
             * Empty: every fragment is discarded.  Expressed as top-left past
             * bottom-right, which is how an empty GL scissor reads.  The
             * rectangle {1,1}..{0,0} rather than {0,0}..{-1,-1} because the
             * fields are 15 bits wide and -1 would wrap to 0x7fff, turning the
             * empty scissor into a full-attachment one.
             */
            sx0 = 1;
            sy0 = 1;
            sx1 = 0;
            sy1 = 0;
         } else {
            sx0 = (uint32_t)rx0;
            sy0 = (uint32_t)ry0;
            sx1 = (uint32_t)(rx1 - 1);
            sy1 = (uint32_t)(ry1 - 1);
         }
      }

      ta5_pkt4(draw, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0, 2);
      ta5_cs_push(draw, A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(sx0) |
                        A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(sy0));
      ta5_cs_push(draw, A5XX_GRAS_SC_SCREEN_SCISSOR_BR_0_X(sx1) |
                        A5XX_GRAS_SC_SCREEN_SCISSOR_BR_0_Y(sy1));

      ta5_pkt4(draw, REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0, 2);
      ta5_cs_push(draw, A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(sx0) |
                        A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(sy0));
      ta5_cs_push(draw, A5XX_GRAS_SC_VIEWPORT_SCISSOR_BR_0_X(sx1) |
                        A5XX_GRAS_SC_VIEWPORT_SCISSOR_BR_0_Y(sy1));

      if (getenv("TA5_TRACE_SCISSOR"))
         fprintf(stderr,
                 "ta5[scissor]: fb=%ux%u render=%ux%u+%u+%u effective=%u,%u..%u,%u"
                 "  [dynamic=%d scissor_set=%d vp=%p scissor_count=%u used=%s]\n",
                 w, h, draw->render_area.extent.width,
                 draw->render_area.extent.height, draw->render_area.offset.x,
                 draw->render_area.offset.y, sx0, sy0, sx1, sy1,
                 (int)BITSET_TEST(pipe->graphics_state.dynamic,
                                  MESA_VK_DYNAMIC_VP_SCISSORS),
                 (int)draw->scissor_set, (void *)pipe->graphics_state.vp,
                 pipe->graphics_state.vp
                    ? pipe->graphics_state.vp->scissor_count
                    : 0,
                 scissor_src);
   }

   /*
    * Vertex fetch.
    *
    * Transcribed from fd5_emit_fetch(): one fetch slot per vertex attribute
    * the VS consumes.  VFD_CONTROL_1 (REGID4VTX/REGID4INST) is already
    * emitted by ta5_emit_program_state(); this is the per-attribute half plus
    * the count saying how many slots are live.
    *
    * With no vertex buffer bound, no fetch slot is written and VTXCNT stays at
    * zero, so an AUTO_INDEX draw is unaffected -- the vertex id still arrives
    * through the VFD's synth path rather than from memory.
    */
   unsigned fetch_cnt = 0;

   if (getenv("TA5_TRACE_VFD"))
      fprintf(stderr,
              "ta5[vfd]: uses_vertex_buffers=%d vi=%p bindings_valid=%d "
              "attributes_valid=0x%llx\n",
              (int)pipe->uses_vertex_buffers, (void *)pipe->graphics_state.vi,
              pipe->graphics_state.vi
                 ? (int)pipe->graphics_state.vi->bindings_valid
                 : -1,
              pipe->graphics_state.vi
                 ? (unsigned long long)pipe->graphics_state.vi->attributes_valid
                 : 0ull);

   if (pipe->uses_vertex_buffers && pipe->graphics_state.vi) {
      const struct ir3_shader_variant *vs =
         (const struct ir3_shader_variant *)pipe->vertex.variant;
      const struct vk_vertex_input_state *vi = pipe->graphics_state.vi;

      ret = ta5_cmd_buffer_alloc_cs(
         draw, draw->cs_count + 32 * MESA_VK_MAX_VERTEX_ATTRIBUTES + 16);
      if (ret != VK_SUCCESS)
         goto draw_error;

      /*
       * One fetch slot per vertex attribute the VS actually consumes,
       * emitted in ascending Vulkan location order, which is the order
       * fd5_emit_fetch() walks its vertex elements in.
       *
       * Vertex attribute locations use VERT_ATTRIB_GENERIC0 as their base in
       * IR3's input table (location 0 was observed as slot 15), unlike
       * fragment varying slots which start at VARYING_SLOT_VAR0.  System
       * values sit in the same table but have sysval set and must not be wired
       * to a fetch slot.
       *
       * The order is not cosmetic: IR3 hands back its inputs in whatever order
       * it allocated registers, so a shader whose location 1 is allocated
       * first would otherwise get fetch slot 0.  The VFD is happy with that
       * -- each slot's fetch, decode and destination agree -- but the
       * fragment stage then sees the varying pinned to a single vertex.
       */
      for (unsigned location = 0; location < MESA_VK_MAX_VERTEX_ATTRIBUTES;
           location++) {
         const struct vk_vertex_attribute_state *attr;
         const struct vk_vertex_binding_state *bstate;
         struct ta5_buffer *buf;
         struct ta5_bo *bo;
         uint32_t fmt;
         unsigned i, binding;
         VkDeviceSize base, size;

         if (!(vi->attributes_valid & BITFIELD64_BIT(location)))
            continue;

         for (i = 0; i < vs->inputs_count; i++) {
            if (!vs->inputs[i].sysval &&
                vs->inputs[i].slot >= VERT_ATTRIB_GENERIC0 &&
                vs->inputs[i].slot - VERT_ATTRIB_GENERIC0 == location)
               break;
         }
         if (i == vs->inputs_count)
            continue;

         attr = &vi->attributes[location];
         binding = attr->binding;
         if (binding >= MESA_VK_MAX_VERTEX_BINDINGS ||
             !(vi->bindings_valid & BITFIELD64_BIT(binding)))
            continue;

         buf = cmd->bound_vb[binding];
         if (!buf || !buf->bound_memory || !buf->bound_memory->bo)
            continue;

         fmt = ta5_vfd_format(attr->format);
         if (fmt == UINT32_MAX)
            continue;

         bstate = &vi->bindings[binding];
         base = cmd->bound_vb_offset[binding] + attr->offset;
         bo = buf->bound_memory->bo;
         if (base > buf->size || buf->size - base < bstate->stride)
            continue;

         size = buf->size - base;

         ta5_pkt4(draw, REG_A5XX_VFD_FETCH(fetch_cnt), 4);
         ret = ta5_cs_reloc(draw, bo, base, 0);
         if (ret != VK_SUCCESS)
            goto draw_error;
         ta5_cs_push(draw, (uint32_t)size); /* VFD_FETCH[].SIZE */
         ta5_cs_push(draw, bstate->stride); /* VFD_FETCH[].STRIDE */

         ta5_pkt4(draw, REG_A5XX_VFD_DECODE(fetch_cnt), 2);
         /*
          * INSTR_IDX names the fetch instruction this decode slot reads.
          * Leaving it at zero for every slot makes them all decode fetch
          * slot 0, so a second attribute reads the first attribute's bytes
          * and the shader silently sees the wrong vertex data.
          */
         ta5_cs_push(draw, A5XX_VFD_DECODE_INSTR_IDX(fetch_cnt) |
                              A5XX_VFD_DECODE_INSTR_FORMAT(
                                 (enum a5xx_vtx_fmt)fmt) |
                              A5XX_VFD_DECODE_INSTR_UNK30 |
                              A5XX_VFD_DECODE_INSTR_FLOAT);
         /*
          * STEP_RATE is how many instances one fetch advances: 1 for
          * per-vertex input, the divisor for per-instance (fd5_emit.c:490).
          */
         ta5_cs_push(draw,
                     bstate->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE
                        ? MAX2(1u, bstate->divisor)
                        : 1u);

         ta5_pkt4(draw, REG_A5XX_VFD_DEST_CNTL(fetch_cnt), 1);
         ta5_cs_push(draw,
                     A5XX_VFD_DEST_CNTL_INSTR_WRITEMASK(
                        vs->inputs[i].compmask) |
                        A5XX_VFD_DEST_CNTL_INSTR_REGID(vs->inputs[i].regid));

         if (getenv("TA5_TRACE_NULL"))
            fprintf(stderr,
                    "ta5[vfd]: fetch %u loc=%u binding=%u fmt=%u stride=%u "
                    "size=%u regid=%u wrmask=0x%x\n",
                    fetch_cnt, location, binding, fmt, bstate->stride,
                    (uint32_t)size, vs->inputs[i].regid,
                    vs->inputs[i].compmask);

         fetch_cnt++;
      }

   }

   /*
    * A draw that fetches no attributes still has to run the fetch unit.
    *
    * The vertex id is a built-in the fetch unit delivers into the VPC
    * register REGID4VTX names, not something the shader reads from memory --
    * and with VTXCNT at zero the unit does not run, so that register keeps
    * whatever it held.  Measured: a shader that builds its positions from
    * gl_VertexIndex then reads id 0 for every vertex, three coincident
    * vertices, and a draw that produces no fragments and raises no fault.
    * vkcube is exactly that shader, and so is the ubotest case.
    *
    * The slot has to read something, so it reads four bytes of a buffer the
    * driver already owns and writes them to a register no shader reads.  The
    * data is discarded; what matters is that the unit runs.
    */
   if (fetch_cnt == 0 && pipe->vertex.variant &&
       ir3_find_sysval_regid((const struct ir3_shader_variant *)
                                 pipe->vertex.variant,
                             SYSTEM_VALUE_VERTEX_ID_ZERO_BASE) !=
          regid(63, 0) &&
       cmd->device->blit_bo) {
      ret = ta5_cmd_buffer_alloc_cs(draw, draw->cs_count + 32);
      if (ret != VK_SUCCESS)
         goto draw_error;

      ta5_pkt4(draw, REG_A5XX_VFD_FETCH(0), 4);
      ret = ta5_cs_reloc(draw, cmd->device->blit_bo, 0, 0);
      if (ret != VK_SUCCESS)
         goto draw_error;
      ta5_cs_push(draw, 4); /* SIZE: four bytes */
      ta5_cs_push(draw, 0); /* STRIDE */

      ta5_pkt4(draw, REG_A5XX_VFD_DECODE(0), 2);
      ta5_cs_push(draw, A5XX_VFD_DECODE_INSTR_IDX(0) |
                           A5XX_VFD_DECODE_INSTR_FORMAT(
                              (enum a5xx_vtx_fmt)VFMT5_32_FLOAT) |
                           A5XX_VFD_DECODE_INSTR_UNK30 |
                           A5XX_VFD_DECODE_INSTR_FLOAT);
      ta5_cs_push(draw, 1); /* STEP_RATE */

      /*
       * r62 is a scratch VPC register: the highest one the vertex id and the
       * vertex outputs cannot reach.  What lands there is discarded by the
       * shader, and REGID4VTX delivers the real id wherever the shader
       * expects it.
       */
      ta5_pkt4(draw, REG_A5XX_VFD_DEST_CNTL(0), 1);
      ta5_cs_push(draw, A5XX_VFD_DEST_CNTL_INSTR_WRITEMASK(0xf) |
                           A5XX_VFD_DEST_CNTL_INSTR_REGID(regid(62, 0)));

      fetch_cnt = 1;

      if (getenv("TA5_TRACE_VFD"))
         fprintf(stderr, "ta5[vfd]: synth fetch slot for the vertex id\n");
   }

   /*
    * VFD_CONTROL_0, whether or not anything is fetched.
    *
    * A draw with no vertex buffer still has vertices: vkcube's vertex shader
    * builds every position from gl_VertexIndex, so the draw has 36 vertices
    * and no attributes, and the vertex id arrives through the VFD's synth
    * path.  With this packet left at its reset value of zero the fetch unit
    * processes no vertices at all -- no fragments, no fault, and a perfectly
    * clear-coloured scanout buffer, which is exactly what vkcube produced.
    *
    * With attributes bound the field belongs to the fetch program, so it
    * stays the slot count; without them it is the draw's vertex count, which
    * is what fd5_draw() writes (p4->vtxcnt = p->count).
    */
   {
      uint32_t vtxcnt = fetch_cnt;

      /* The fetch block above allocated for itself; a draw that fetched
       * nothing has not. */
      ret = ta5_cmd_buffer_alloc_cs(draw, draw->cs_count + 8);
      if (ret != VK_SUCCESS)
         goto draw_error;

      if (!getenv("TA5_SKIP_VFD")) {
         ta5_pkt4(draw, REG_A5XX_VFD_CONTROL_0, 1);
         ta5_cs_push(draw, A5XX_VFD_CONTROL_0_VTXCNT(vtxcnt));
      }

      if (getenv("TA5_TRACE_VFD"))
         fprintf(stderr,
                 "ta5[vfd]: VTXCNT=%u (fetch slots=%u, draw verts=%u)\n",
                 vtxcnt, fetch_cnt, vertexCount);
   }

   /*
    * Texture state is emitted here only so the draw-state emitter has the
    * unit counts to program.  The descriptors themselves are re-emitted after
    * the shader loads: loading a shader resets the texel descriptors, so a
    * descriptor written before it is gone by the time the fetch runs, and the
    * unit then reads as all zeros -- base address 0, which is what a sampled
    * image that faults at iova 0 looks like.
    */
   if (getenv("TA5_SKIP_TEX")) {
      vs_tex_count = fs_tex_count = 0;
   } else {
      vs_tex_count = ta5_emit_textures(draw, SB4_VS_TEX, pipe,
                                       VK_SHADER_STAGE_VERTEX_BIT);
      fs_tex_count = ta5_emit_textures(draw, SB4_FS_TEX, pipe,
                                       VK_SHADER_STAGE_FRAGMENT_BIT);
   }
   if (getenv("TA5_TRACE_TEX")) {
      for (uint32_t i = 0; i < MIN2(draw->cs_count, 28u); i++)
         fprintf(stderr, "  cs[%2u] = %08x\n", i, draw->cs[i]);
   }

   if (!getenv("TA5_SKIP_DRAWSTATE"))
      ta5_emit_draw_state(draw, vs_tex_count, fs_tex_count,
                            vertexCount, pipe);

   /*
    * Depth state goes in *after* the draw state, not before it.
    *
    * ta5_emit_draw_state() zeroes the whole depth block -- INFO to NONE, BASE
    * to 0, CNTL to 0 -- on the reasoning that a draw with no depth attachment
    * must not inherit the previous draw's buffer.  Emitting depth first meant
    * the draw state then cleared it again, so the unit tested against nothing:
    * every fragment passed, including VK_COMPARE_OP_NEVER, and no depth write
    * ever landed (measured: depth[0,0] stayed at the clear value on all nine
    * depth cases while the colour said "passed").  After the draw state the
    * depth registers hold what this draw asked for.
    *
    * It has to be emitted on every draw rather than only when the test is on:
    * RB_DEPTH_CNTL persists, and a second draw with the test disabled would
    * otherwise keep testing.
    */
   if (ta5_emit_depth_state(draw, pipe) != VK_SUCCESS)
      return;

   ta5_pkt4(draw, REG_A5XX_GRAS_CL_VPORT_XOFFSET_0, 6);
   ta5_cs_push(draw, A5XX_GRAS_CL_VPORT_XOFFSET_0(draw->bound_framebuffer->width
                                                  / 2.0f));
   ta5_cs_push(draw, A5XX_GRAS_CL_VPORT_XSCALE_0(draw->bound_framebuffer->width
                                                / 2.0f));
   ta5_cs_push(draw, A5XX_GRAS_CL_VPORT_YOFFSET_0(draw->bound_framebuffer->height
                                                  / 2.0f));
   ta5_cs_push(draw, A5XX_GRAS_CL_VPORT_YSCALE_0(draw->bound_framebuffer->height
                                                 / 2.0f));
   /*
    * Vulkan NDC z is 0..1, not -1..1.
    *
    * The 0.5/0.5 here was GL's depth-range mapping: NDC -1..1 to window 0..1
    * via z*0.5+0.5.  With a Vulkan NDC that mapping turns a fragment at 0.25
    * into 0.625 (measured: depth[0,0] read back 0.625 after drawing at 0.25,
    * and 0.875 after 0.75).  The test still passed -- both the clear and the
    * fragment went through the same wrong transform -- but the buffer held a
    * value no Vulkan operation would have written.
    *
    * For Vulkan the window mapping is z*scale+offset with scale =
    * maxDepth-minDepth and offset = minDepth, taken from the dynamic viewport
    * when the application set one.  The default viewport is 0..1, so the
    * default here is scale 1, offset 0: the identity.
    */
   {
      float zmin = draw->viewport_set ? draw->viewport.minDepth : 0.0f;
      float zmax = draw->viewport_set ? draw->viewport.maxDepth : 1.0f;
      if (!getenv("TA5_SKIP_VPORT")) {
         ta5_cs_push(draw, A5XX_GRAS_CL_VPORT_ZOFFSET_0(zmin));
         ta5_cs_push(draw, A5XX_GRAS_CL_VPORT_ZSCALE_0(zmax - zmin));
      }
   }

   if (!getenv("TA5_SKIP_PROGSTATE"))
      ta5_emit_program_state(draw, pipe);

   /*
    * Both programs are loaded before either stage's constants, which is the
    * order fd5 emits them in (fd5_program.c: the SP_xS_OBJ_START and program
    * loads for both stages come first, and the constant and driver-parameter
    * uploads follow).  Emitting each stage's constants straight after its own
    * program load instead reorders them, and the fragment stage faults on
    * the very first shaded fragment -- see the plan doc.
    */
   /*
    * TA5_SKIP_VSPROG / TA5_SKIP_FSPROG / TA5_SKIP_CONSTS / TA5_SKIP_DRAWPKT
    * take one piece out of the indirect buffer.
    *
    * The buffer is reached through CP_INDIRECT_BUFFER, and with the packet
    * gone the CP never enters it and 6 frames come back with 0 hangchecks;
    * with the packet in, 3 of 6 hang.  So the hang happens while the CP is
    * executing this buffer, and these four knobs say which part of it.
    */
   if (getenv("TA5_SKIP_VSPROG"))
      goto no_programs;

   ret = ta5_emit_shader(draw, pipe, &pipe->vertex, SB4_VS_SHADER,
                         REG_A5XX_SP_VS_OBJ_START, pipe->vs_bo);
   if (ret != VK_SUCCESS)
      goto draw_error;
   if (pipe->has_fragment && !getenv("TA5_SKIP_FSPROG")) {
      ret = ta5_emit_shader(draw, pipe, &pipe->fragment, SB4_FS_SHADER,
                            REG_A5XX_SP_FS_OBJ_START, pipe->fs_bo);
      if (ret != VK_SUCCESS)
         goto draw_error;
   }
no_programs:

   if (getenv("TA5_SKIP_CONSTS"))
      goto no_consts;

   ta5_emit_consts(draw, pipe, &pipe->vertex, SB4_VS_SHADER, pipe->vs_bo);
   if (pipe->has_fragment)
      ta5_emit_consts(draw, pipe, &pipe->fragment, SB4_FS_SHADER, pipe->fs_bo);

   /*
    * UBO pointers live in the same constant file as everything else, so they
    * are uploaded alongside it and after it: the shader load that precedes
    * them resets the whole constant page.
    */
   ret = ta5_emit_ubo_pointers(draw, pipe, &pipe->vertex, SB4_VS_SHADER,
                               VK_SHADER_STAGE_VERTEX_BIT);
   if (ret != VK_SUCCESS)
      goto draw_error;
   if (pipe->has_fragment) {
      ret = ta5_emit_ubo_pointers(draw, pipe, &pipe->fragment, SB4_FS_SHADER,
                                  VK_SHADER_STAGE_FRAGMENT_BIT);
      if (ret != VK_SUCCESS)
         goto draw_error;
   }

   /* Now that the shaders are loaded, put the descriptors back. */
   ta5_emit_textures(draw, SB4_VS_TEX, pipe, VK_SHADER_STAGE_VERTEX_BIT);
   ta5_emit_textures(draw, SB4_FS_TEX, pipe, VK_SHADER_STAGE_FRAGMENT_BIT);
no_consts:

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
      ret = ta5_cmd_buffer_alloc_cs(draw, draw->cs_count + 4096);
      if (ret != VK_SUCCESS)
         goto draw_error;
      if (!getenv("TA5_SKIP_DRAWSTATE"))
      ta5_emit_draw_state(draw, vs_tex_count, fs_tex_count,
                            vertexCount, pipe);
      ta5_emit_program_state(draw, pipe);
      ret = ta5_emit_shader(draw, pipe, &pipe->vertex, SB4_VS_SHADER,
                            REG_A5XX_SP_VS_OBJ_START, pipe->vs_bo);
      if (ret != VK_SUCCESS)
         goto draw_error;
      if (pipe->has_fragment) {
         ret = ta5_emit_shader(draw, pipe, &pipe->fragment, SB4_FS_SHADER,
                               REG_A5XX_SP_FS_OBJ_START, pipe->fs_bo);
         if (ret != VK_SUCCESS)
            goto draw_error;
      }
      ta5_emit_consts(draw, pipe, &pipe->vertex, SB4_VS_SHADER, pipe->vs_bo);
      if (pipe->has_fragment)
         ta5_emit_consts(draw, pipe, &pipe->fragment, SB4_FS_SHADER, pipe->fs_bo);
   }

   /*
    * CP_DRAW_INDX_OFFSET, no index buffer.  AUTO_INDEX synthesises vertex ids
    * 0..count-1, which is exactly what gl_VertexIndex reads.  With no index
    * buffer fd5 still programs INDEX_SIZE as 32-bit (fd5_draw.h:114) rather
    * than leaving it at the 8-bit reset value.
    */
   if (getenv("TA5_SKIP_DRAWPKT"))
      goto no_drawpkt;

   /*
    * With an index buffer bound this is fd5_draw()'s seven-dword packet
    * (fd5_draw.h:41-56): the same CP_DRAW_INDX_OFFSET with DI_SRC_SEL_DMA, the
    * index format rather than a fixed 32-bit size, and three extra payload
    * words -- a zero, the index buffer's address as a relocation, and
    * MaxIndices.
    */
   if (cmd->index_bo) {
      uint32_t idx_bytes =
         cmd->index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
      uint64_t idx_off = cmd->index_buffer_offset +
                         (uint64_t)cmd->draw_first_vertex * idx_bytes;

      ta5_pkt7(draw, CP_DRAW_INDX_OFFSET, 7);
      ta5_cs_push(draw, CP_DRAW_INDX_OFFSET_0_PRIM_TYPE(
                           getenv("TA5_POINTS") ? DI_PT_POINTLIST
                                                : DI_PT_TRILIST) |
                        CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(DI_SRC_SEL_DMA) |
                        CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(
                            idx_bytes == 2 ? INDEX4_SIZE_16_BIT
                                           : INDEX4_SIZE_32_BIT) |
                        CP_DRAW_INDX_OFFSET_0_VIS_CULL(IGNORE_VISIBILITY));
      ta5_cs_push(draw, instanceCount);
      ta5_cs_push(draw, vertexCount);
      ta5_cs_push(draw, 0x0); /* fd5 writes 0x0 here too, marked XXX there */
      ret = ta5_cs_reloc(draw, cmd->index_bo, idx_off, 0);
      if (ret != VK_SUCCESS)
         goto draw_error;
      ta5_cs_push(draw, cmd->index_count);
   } else {
      ta5_pkt7(draw, CP_DRAW_INDX_OFFSET, 3);
      ta5_cs_push(draw, CP_DRAW_INDX_OFFSET_0_PRIM_TYPE(
                           getenv("TA5_POINTS") ? DI_PT_POINTLIST
                                                : DI_PT_TRILIST) |
                        CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(DI_SRC_SEL_AUTO_INDEX) |
                        CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(INDEX4_SIZE_32_BIT) |
                        CP_DRAW_INDX_OFFSET_0_VIS_CULL(IGNORE_VISIBILITY));
      ta5_cs_push(draw, instanceCount);
      ta5_cs_push(draw, vertexCount);
   }
no_drawpkt:

   if (getenv("TA5_TRACE_DRAW")) {
      fprintf(stderr, "--- ta5: draw cs, %u dwords ---\n", draw->cs_count);
      for (uint32_t i = 0; i < draw->cs_count; i++)
         fprintf(stderr, "  %04x: %08x\n", i * 4, draw->cs[i]);
   }

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
   if (getenv("TA5_TRACE_TEX"))
      fprintf(stderr, "ta5[tex] before add_ib: cs_count=%u\n", draw->cs_count);
   if (use_ib) {
      ret = ta5_cmd_buffer_add_ib(cmd, draw);
      if (ret != VK_SUCCESS)
         goto draw_error;

      struct ta5_ib *ib = &cmd->ibs[cmd->ib_count - 1];
      ta5_pkt7(cmd, CP_INDIRECT_BUFFER, 3);
      ret = ta5_cs_reloc(cmd, ib->bo, 0, 0);
      if (ret != VK_SUCCESS)
         goto draw_error;
      /*
       * IB_SIZE is a byte count.  __OUT_IB5() passes align(size, 4), and the
       * field is 20 bits wide, which is a byte budget rather than a word one.
       * Pushing the dword count told the CP the indirect buffer was a quarter
       * of its real length; the kernel then reports the fetch as
       * ib2 <addr>/0000 and the CP runs off the end into zeroes:
       *
       *     CP | opcode error | possible opcode=0x00000000
       *     gpu fault ring 2 ... ib2 0000000002831000/0000
       *     *ERROR* 05000600: hangcheck recover!
       *
       * A short draw hides it -- at 64x64 the truncated tail happened to fall
       * where nothing depended on it -- and a 1080x2280 frame does not.
       * TA5_IB_DWORDS=1 puts the dword count back for the A/B.
       */
      ta5_cs_push(cmd, ib->size); /* IB_SIZE, in dwords -- measured */

      ta5_pkt7(cmd, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
      ta5_cs_push(cmd, 0);
   }

   if (use_ib)
      vk_free(&cmd->device->vk.alloc, draw->cs);
   return;

draw_error:
   if (use_ib) {
      vk_free(&cmd->device->vk.alloc, draw->relocs);
      vk_free(&cmd->device->vk.alloc, draw->reloc_bos);
      vk_free(&cmd->device->vk.alloc, draw->cs);
   }
}

/*
 * The generated entrypoints have no declaration for vkCmdBindIndexBuffer2 --
 * which is why the loader answers NULL for it -- so it is declared here.
 */
VKAPI_ATTR void VKAPI_CALL
ta5_CmdBindIndexBuffer2(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                        uint32_t bindingCount, const VkBuffer *pBuffers,
                        const VkDeviceSize *pOffsets,
                        const VkIndexType *pIndexTypes,
                        const uint32_t *pOffsets2);

/*
 * vkCmdBindIndexBuffer2: same binding, with the index types supplied.
 *
 * vkcube asks the loader for this one first -- it is core Vulkan 1.0, not an
 * extension -- and the generated entrypoints answer NULL for it, so an
 * application that binds its indices through it ends up with no index buffer
 * bound at all and an indexed draw that quietly draws nothing.
 */
VKAPI_ATTR void VKAPI_CALL
ta5_CmdBindIndexBuffer2(VkCommandBuffer _cmd, uint32_t firstBinding,
                        uint32_t bindingCount, const VkBuffer *pBuffers,
                        const VkDeviceSize *pOffsets,
                        const VkIndexType *pIndexTypes,
                        const uint32_t *pOffsets2)
{
   for (uint32_t i = 0; i < bindingCount; i++) {
      (void)pOffsets2;   /* per-draw offsets: applied in vkCmdDrawIndexed */
      ta5_CmdBindIndexBuffer(_cmd, pBuffers ? pBuffers[i] : VK_NULL_HANDLE,
                             pOffsets ? pOffsets[i] : 0,
                             pIndexTypes ? pIndexTypes[i]
                                         : VK_INDEX_TYPE_UINT32);
   }
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdDraw(VkCommandBuffer _cmd, uint32_t vertexCount, uint32_t instanceCount,
            uint32_t firstVertex, uint32_t firstInstance)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   if (getenv("TA5_TRACE_NULL"))
      fprintf(stderr, "ta5: CmdDraw vertices=%u\n", vertexCount);

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   ta5_record_draw(cmd, vertexCount, instanceCount, firstVertex, firstInstance,
                   /* index_size */ 0);
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdDrawIndexed(VkCommandBuffer _cmd, uint32_t indexCount,
                   uint32_t instanceCount, uint32_t firstIndex,
                   int32_t vertexOffset, uint32_t firstInstance)
{
   struct ta5_cmd_buffer *cmd = ta5_cmd_buffer_from_handle(_cmd);

   if (cmd->state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)
      return;

   if (getenv("TA5_TRACE_NULL"))
      fprintf(stderr, "ta5: DrawIndexed count=%u firstIndex=%u index_bo=%p\n",
              indexCount, firstIndex, (void *)cmd->index_bo);

   if (cmd->index_bo == NULL)
      return;   /* invalid usage: an indexed draw with no index buffer bound */

   /*
    * The index buffer address the hardware fetches from is
    * buffer + offset + firstIndex * index_size (fd5_draw.h:110), so firstIndex
    * is folded into the offset rather than the count.  vertexOffset is a signed
    * adjustment applied to the fetched index before it addresses a vertex;
    * vkcube passes 0, and a5xx's draw packet has no field for it, so this
    * rejects rather than silently drawing the wrong vertices.
    */
   if (vertexOffset != 0) {
      if (getenv("TA5_TRACE_NULL"))
         fprintf(stderr,
                 "ta5: vkCmdDrawIndexed: vertexOffset %d unsupported\n",
                 vertexOffset);
      return;
   }

   ta5_record_draw(cmd, indexCount, instanceCount, firstIndex, firstInstance,
                   cmd->index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4);
}
