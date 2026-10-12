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
   cmd->submitted = 0;
   cmd->fence = NULL;
   cmd->state = MESA_VK_COMMAND_BUFFER_STATE_INITIAL;

   return VK_SUCCESS;
}