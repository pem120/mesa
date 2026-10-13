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

struct ta5_cmd_buffer
{
   struct vk_object_base base;
   struct ta5_device *device;
   struct ta5_cmd_pool *pool;

   enum mesa_vk_command_buffer_state state;

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

   /** Bound state at the point of recording. */
   struct ta5_pipeline *bound_pipeline;
   struct ta5_framebuffer *bound_framebuffer;
   struct ta5_render_pass *bound_render_pass;
   struct ta5_descriptor_set *bound_sets[4];

   /**
    * Vertex buffer binding, as recorded by vkCmdBindVertexBuffers.
    *
    * One slot for now: that is what a non-indexed draw of one attribute
    * needs, and it is the smallest thing that can tell whether the VFD is
    * fed at all.  Slot 0 only -- a binding above firstSlot + bindingCount
    * is dropped rather than silently landing in the wrong slot.
    */
   struct ta5_buffer *bound_vb;
   VkDeviceSize bound_vb_offset;

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