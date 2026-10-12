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

   /** Fence to signal when this submission completes. */
   struct ta5_fence *fence;

   /** Set once submitted, so a second submit can be rejected. */
   uint32_t submitted;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_cmd_buffer, base, VkCommandBuffer,
                               VK_OBJECT_TYPE_COMMAND_BUFFER)

#endif /* TA5_CMD_BUFFER_H */