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
 * \file ta5_cmd_buffer.h
 *
 * Adreno 5xx Vulkan driver: command pools and command buffers.
 */

#ifndef TA5_CMD_BUFFER_H
#define TA5_CMD_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "util/u_math.h"
#include "vk_object.h"
#include "vk_device.h"
#include "vk_command_buffer.h"

#include "ta5_instance.h"

/** Command pool flags, kept verbatim so vkResetCommandPool can honour them. */
struct ta5_cmd_pool
{
   struct vk_object_base base;
   struct ta5_device *device;
   uint32_t queue_family_index;
   VkCommandPoolCreateFlags flags;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_cmd_pool, base, VkCommandPool,
                               VK_OBJECT_TYPE_COMMAND_POOL)

/**
 * Command buffer.
 *
 * a5xx command streams are a flat array of 32-bit PM4 words, so a command
 * buffer is a growable byte buffer (ta5_cs).  Bin and render state is
 * gathered separately and emitted at flush time; see ta5_cmd_buffer.cc.
 */
struct ta5_pipeline;
struct ta5_buffer;
struct ta5_framebuffer;
struct ta5_render_pass;
struct ta5_descriptor_set;
struct ta5_bo;

/** One buffer relocation in the recorded command stream. */
struct ta5_reloc
{
   uint32_t bo_index;   /**< index into ta5_cmd_buffer::reloc_bos */
   uint64_t bo_offset;  /**< offset into that BO */
   uint32_t dst_off;    /**< byte offset of the dword to patch */
   uint32_t or_mask;    /**< value OR'd into the patched dword */
   int32_t shift;       /**< kernel right-shifts the address by -shift */
};

/**
 * A relocatable second-level command buffer.
 *
 * The MSM submit UAPI supports MSM_SUBMIT_CMD_IB_TARGET_BUF: the kernel
 * relocates these BOs normally, but does not add them to the first-level ring.
 * CP_INDIRECT_BUFFER in the main stream enters the target after all relocations
 * have been applied.  fd5 uses this for its per-draw state and shader loads.
 */
struct ta5_ib
{
   struct ta5_bo *bo;
   uint32_t size; /**< command buffer size in dwords */

   struct ta5_reloc *relocs;
   uint32_t reloc_count;
   struct ta5_bo **reloc_bos;
   uint32_t reloc_bo_count;
};

/** The advertised maxPushConstantsSize (128), in dwords. */
#define TA5_MAX_PUSH_CONST_DWORDS 32

/**
 * A host-side attachment clear, deferred to submit time.
 *
 * A depth clear for D32_SFLOAT is a CPU write rather than a CCU fast clear,
 * because the fast clear does not clear a Z32 attachment on this part -- the
 * working GL driver refuses to attempt one for the same reason (fd5_draw.c:229).
 *
 * Deferring is not optional.  Recording happens arbitrarily far ahead of
 * execution, so a clear written at record time is overwritten by whatever the
 * application does to the image next, and the render pass then begins with an
 * attachment nobody cleared.  The depth buffer is host-visible linear memory, so
 * performing the write just before the batch reaches the GPU is both possible
 * and correct: the clear lands, then the draws that follow in the same stream
 * see it.
 *
 * This is the opposite position from the image -> buffer copies below, which
 * must run *after* the GPU has finished because they read its output.
 */
struct ta5_deferred_clear
{
   struct ta5_image *image;
   VkDeviceSize row_pitch;
   VkDeviceSize layer_pitch;
   VkDeviceSize level_offset;
   uint32_t width;  /**< visible width at this level */
   uint32_t height; /**< visible height at this level */
   uint32_t layers;
   uint32_t texel_size; /**< bytes per texel, 2 or 4 */
   uint32_t word;       /**< the clear value, already packed for the format */
};

/**
 * Perform the host clears queued by a render pass begin.
 * Called immediately before the batch is submitted.
 *
 * The recording is kept rather than consumed: a command buffer may be submitted
 * repeatedly, and a clear drained by the first submission leaves every later
 * one rendering against an uncleared attachment.
 */
void ta5_cmd_buffer_run_deferred_clears(struct ta5_cmd_buffer *cmd);

/** Release clears no longer part of the recording. */
void ta5_cmd_buffer_clear_deferred_clears(struct ta5_cmd_buffer *cmd);

/**
 * Queue a host-side clear of one image level.  Shared by the depth and colour
 * paths.
 */
bool ta5_cmd_buffer_queue_clear(struct ta5_cmd_buffer *cmd,
                                struct ta5_image *image, uint32_t mip_level,
                                uint32_t layer, uint32_t texel_size,
                                uint32_t word);

/** A vkCmdCopyImageToBuffer whose host copy is deferred to submit time. */
struct ta5_deferred_i2b
{
   struct ta5_image *image;
   VkBuffer buffer;

   /*
    * A snapshot of the level layout rather than a struct ta5_image_level,
    * because ta5_memory.h is not visible here.  The values are taken at
    * record time, so a later rebind cannot move the data out from under the
    * copy.
    */
   VkDeviceSize row_pitch;
   VkDeviceSize layer_pitch;
   VkDeviceSize level_offset;

   uint32_t block_size;
   uint32_t mip;
   VkDeviceSize image_offset;
   VkBufferImageCopy *regions;
   uint32_t region_count;
};

/**
 * Perform the image -> buffer copies queued by ta5_CmdCopyImageToBuffer.
 * Called after the GPU has finished the batch.
 *
 * The recording is kept rather than consumed: a command buffer may be submitted
 * repeatedly, and wsi's blit is recorded once and submitted on every present.
 */
void ta5_cmd_buffer_run_deferred_i2b(struct ta5_cmd_buffer *cmd);

/** Release copies recorded but no longer part of the recording. */
void ta5_cmd_buffer_clear_deferred_i2b(struct ta5_cmd_buffer *cmd);

struct ta5_cmd_buffer
{
   struct vk_object_base base;
   struct ta5_device *device;
   struct ta5_cmd_pool *pool;

   enum mesa_vk_command_buffer_state state;
   VkResult record_result;

   /** Recorded command stream. */
   uint32_t *cs;
   uint32_t cs_size;    /**< capacity in words */
   uint32_t cs_count;   /**< used words */

   /**
    * Buffer relocations referenced by the command stream.
    *
    * a5xx submits with relocations rather than binding buffers into the IB
    * (freedreno never sets MSM_PARAM_EN_VM_BIND), so any buffer the GPU must
    * touch -- a render target, a vertex buffer -- is patched into the
    * command stream by the kernel at submit time.  Each entry names a BO and
    * the dword in the stream that receives its GPU address.
    */
   struct ta5_reloc *relocs;
   uint32_t reloc_size;   /**< capacity in entries */
   uint32_t reloc_count;  /**< used entries */

   /** BOs referenced, so they can be listed in the submit ioctl. */
   struct ta5_bo **reloc_bos;
   uint32_t reloc_bo_count;

   /** Second-level draw/state command buffers, submitted as IB targets. */
   struct ta5_ib *ibs;
   uint32_t ib_count;
   uint32_t ib_size;

   /**
    * Image -> buffer copies deferred to submit time.
    *
    * vkCmdCopyImageToBuffer is a host memcpy here, but a host memcpy runs
    * when the command is *recorded*, and recording can be arbitrarily far
    * ahead of execution.  That is fatal for the wl_shm blit wsi records:
    * wsi_finish_create_blit_context() records image -> buffer exactly once,
    * during swapchain creation, long before the application has drawn
    * anything.  Copying then produced the image's initial contents -- zeros --
    * so every presented frame was solid black while the client reported
    * success throughout.
    *
    * So the copy is queued here and performed by ta5_queue_submit_batch()
    * after the kernel fence says the GPU is done.  The region data is copied
    * rather than referenced, because the caller's arrays need not outlive the
    * recording.
    */
   struct ta5_deferred_i2b *i2b;
   uint32_t i2b_count;
   uint32_t i2b_size;

   /** Host-side attachment clears, performed just before the batch is sent. */
   struct ta5_deferred_clear *clears;
   uint32_t clears_count;
   uint32_t clears_size;

   /** Bound state at the point of recording. */
   struct ta5_pipeline *bound_pipeline;
   struct ta5_framebuffer *bound_framebuffer;
   struct ta5_framebuffer *trace_framebuffer;
   struct ta5_render_pass *bound_render_pass;
   struct ta5_descriptor_set *bound_sets[4];
   /* Eight dynamic UBOs plus eight dynamic SSBOs, matching the device limits.
    * Kept per set so binding another set preserves these offsets.
    */
   uint32_t dynamic_offsets[4][16];

   /**
    * Push constant values, as written by vkCmdPushConstants().
    *
    * Sized to the advertised maxPushConstantsSize (128 bytes).  The draw
    * emitter uploads the slice each shader's const allocation covers with a
    * CP_LOAD_STATE4/ST4_CONSTANTS load; dwords the application never pushed
    * stay zero, which is what the spec promises for unpushed ranges.
    */
   uint32_t push_consts[TA5_MAX_PUSH_CONST_DWORDS];

   /**
    * Render area of the pass being recorded.
    *
    * The effective scissor is the scissor rectangle intersected with this, so
    * it has to be recorded: a scissor of {0,0,100,100} against a render area
    * of {0,0,64,64} is the full attachment and not an error.
    */
   VkRect2D render_area;

   /**
    * Dynamic viewport and scissor, as recorded by vkCmdSetViewport and
    * vkCmdSetScissor.
    *
    * a5xx has one viewport, so only index 0 is kept; a call naming a different
    * index is rejected at record time rather than silently ignored.  The
    * *_set flags distinguish "never set", which is the case the pipeline's own
    * static rectangle covers, from "set to an empty rectangle", which is a
    * legitimate thing to ask for and discards every fragment.
    *
    * These are command-buffer state and are deliberately not reset by
    * vkCmdBeginRenderPass: only the states added by VK_EXT_extended_dynamic_state
    * are reset there, and this driver does not advertise it.  They do not
    * survive vkResetCommandPool.
    */
   VkViewport viewport;
   VkRect2D scissor;
   float blend_constants[4];
   bool viewport_set;
   bool scissor_set;

   /**
    * Vertex buffer bindings, as recorded by vkCmdBindVertexBuffers.
    *
    * One entry per binding slot, indexed by binding number, so a pipeline
    * whose attributes name more than one binding can fetch each of them.
    * A slot that has not been bound stays NULL and its offset is zero;
    * attributes pointing at it are skipped at draw time.
    */
   struct ta5_buffer *bound_vb[MESA_VK_MAX_VERTEX_BINDINGS];
   VkDeviceSize bound_vb_offset[MESA_VK_MAX_VERTEX_BINDINGS];

   /**
    * The draw currently being recorded, needed by the driver-parameter upload.
    *
    * IR3 hands the vertex stage a block of driver parameters at c0.x and the
    * shader reads them -- this one's c0.y is vtxid_base -- so they have to be
    * filled in per draw rather than once per pipeline.
    */
   uint32_t draw_first_vertex;
   uint32_t draw_first_instance;
   uint32_t draw_index_size;

   /**
    * The bound index buffer, for vkCmdDrawIndexed.
    *
    * vkcube draws its cube from an index buffer, so without this it issued a
    * draw the driver had nowhere to fetch indices from: the frame came out as
    * the clear colour and nothing else.  A NULL buffer means no index buffer is
    * bound, which is what vkCmdDraw uses (it synthesises vertex ids instead).
    */
   struct ta5_bo *index_bo;
   uint64_t index_buffer_offset;
   uint32_t index_type;   /* enum VkIndexType */
   uint32_t index_count;  /* indices in the whole buffer, for MaxIndices */

   /** Fence to signal when this submission completes. */
   struct ta5_fence *fence;

   /** Set once submitted, so a second submit can be rejected. */
   uint32_t submitted;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_cmd_buffer, base, VkCommandBuffer,
                               VK_OBJECT_TYPE_COMMAND_BUFFER)

/**
 * PM4 packet construction.
 *
 * A5xx command streams are a bare sequence of PM4 packets -- there is no
 * stream header.  Emitted packets are **type 7**, not type 3:
 *
 *   CP_TYPE7_PKT | cnt | (parity(cnt) << 15) |
 *                 ((opcode & 0x7f) << 16) | (parity(opcode) << 23)
 *
 * with CP_TYPE7_PKT == 0x70000000.  This mirrors
 * src/freedreno/common/freedreno_pm4.h:pm4_pkt7_hdr(), which is what the
 * working GL backend uses for this part (fd5_emit.c uses OUT_PKT7).
 *
 * VERIFIED against the reverse engineering of the OEM blob, whose packet base
 * 0x70b00003 decomposes exactly as pkt7(opcode 0x30, cnt 3) -- 0x30 being
 * CP_LOAD_STATE4, matching A5XX_VULKAN_SPEC.md section 4.3.
 *
 * Using type-3 here instead is silently wrong: the CP decodes the header,
 * finds an opcode it does not implement, and faults the ring ("CP | opcode
 * error" in dmesg, followed by a hangcheck recovery).
 */
#define TA5_CP_TYPE7_PKT 0x70000000u

/**
 * Odd parity of x, as PM4 header fields require.
 *
 * The bit at position i of 0x6996 is set when popcount(i) is odd, so
 * inverting it and shifting by x isolates the parity bit.  This has to be
 * *odd* parity: folding x down to its parity (x ^ x>>1 ... & 1) gives the
 * even-parity answer, which is wrong for exactly the multi-bit values --
 * counts of 3 and larger -- and the CP rejects a header whose parity bit is
 * wrong, aborting the whole indirect buffer.  Compare pm4_odd_parity_bit().
 */
static inline uint32_t
ta5_pm4_odd_parity(uint32_t x)
{
   x ^= x >> 16;
   x ^= x >> 8;
   x ^= x >> 4;
   x &= 0xf;
   return (~0x6996u >> x) & 1;
}

/** Build a type-7 packet header for an opcode with `count` total dwords. */
static inline uint32_t
ta5_pm4_pkt7(uint32_t opcode, uint32_t count)
{
   return TA5_CP_TYPE7_PKT | count | (ta5_pm4_odd_parity(count) << 15) |
          ((opcode & 0x7f) << 16) |
          (ta5_pm4_odd_parity(opcode) << 23);
}

/** CP_LOAD_STATE4 -- the state-load packet family this GPU generation uses. */
#define TA5_CP_LOAD_STATE4 0x30

#endif /* TA5_CMD_BUFFER_H */
