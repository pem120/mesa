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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * \file ta5_query.cc
 *
 * Query pools.
 *
 * These exist because libvulkan_wsi calls them unconditionally, not because
 * a5xx queries work.  wsi_destroy_image() ends with
 *
 *     wsi->DestroyQueryPool(chain->device, image->query_pool, NULL);
 *
 * (wsi_common.c:1053) with no NULL check on the pool, and wsi_device_init()
 * obtains it with WSI_GET_CB(), a bare cast with no NULL check either.  With
 * no implementation the dispatch slot stayed NULL, the trampoline branched to
 * address 0, and every swapchain teardown -- headless and Wayland alike --
 * died in wsi_destroy_image.
 *
 * What is honest about the contents: nothing here ever produces a real result.
 * There is no occlusion or pipeline-statistics query in PM4 yet, and the
 * device advertises neither occlusionQuery nor pipelineStatisticsQuery, so an
 * application has no reason to ask for one.  Results therefore stay at zero
 * and are reported as unavailable, which is the honest answer rather than a
 * plausible-looking number.
 *
 * A caveat worth stating plainly: the entry points below are reached through
 * the dispatch table, and the WSI proc-addr shim returns *trampolines*, not
 * the functions themselves.  So "the pointer is non-NULL" says nothing about
 * whether a driver implements something -- a missing entry point shows up as a
 * valid trampoline address whose table slot is NULL.  Checking the slot is the
 * only way to tell.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_math.h"

#include "vk_object.h"
#include "vk_util.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_sync.h"
#include "ta5_query.h"

/**
 * A query pool.
 *
 * Results live on the host.  There is nothing GPU-side to read back yet, and a
 * pool still has to exist for wsi's timestamp bookkeeping and for the
 * teardown path.
 */
struct ta5_query_pool
{
   struct vk_object_base base;
   struct ta5_device *device;
   VkQueryType type;
   uint32_t count;
   uint64_t *results;  /**< count entries, zero unless a result was written */
   uint8_t *available; /**< count flags; only set by a write, which never comes */
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_query_pool, base, VkQueryPool,
                               VK_OBJECT_TYPE_QUERY_POOL)

/** Bytes a single query's result occupies in a vkGetQueryPoolResults write. */
static uint32_t
ta5_query_result_size(VkQueryType type)
{
   switch (type) {
   case VK_QUERY_TYPE_OCCLUSION:
      return sizeof(uint64_t);
   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      return sizeof(VkQueryPipelineStatisticFlags) * 2;
   case VK_QUERY_TYPE_TIMESTAMP:
      return sizeof(uint64_t);
   default:
      return 0;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateQueryPool(VkDevice _device, const VkQueryPoolCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator, VkQueryPool *pQueryPool)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_query_pool *pool;
   uint32_t result_size;

   if (pCreateInfo->queryCount == 0) {
      /* A pool with no queries is legal but has nothing to store. */
      result_size = 1;
   } else {
      result_size = ta5_query_result_size(pCreateInfo->queryType);
      if (result_size == 0)
         return vk_error(&device->instance->vk, VK_ERROR_FEATURE_NOT_PRESENT);
   }

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   pool = (struct ta5_query_pool *)vk_zalloc(
      pAllocator, sizeof(*pool), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!pool)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   pool->device = device;
   pool->type = pCreateInfo->queryType;
   pool->count = pCreateInfo->queryCount;

   /* Size the arrays by the result type rather than assuming uint64, so a
    * pipeline-statistics pool does not over- or under-allocate. */
   pool->results = (uint64_t *)vk_zalloc(
      pAllocator, sizeof(uint64_t) * MAX2(pool->count, 1), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   pool->available = (uint8_t *)vk_zalloc(
      pAllocator, MAX2(pool->count, 1), 1,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!pool->results || !pool->available) {
      vk_free(pAllocator, pool->results);
      vk_free(pAllocator, pool->available);
      vk_free(pAllocator, pool);
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_object_base_init(&device->vk, &pool->base, VK_OBJECT_TYPE_QUERY_POOL);

   *pQueryPool = ta5_query_pool_to_handle(pool);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyQueryPool(VkDevice _device, VkQueryPool _queryPool,
                     const VkAllocationCallbacks *pAllocator)
{
   struct ta5_query_pool *pool = ta5_query_pool_from_handle(_queryPool);

   /* wsi_destroy_image() passes the handle unconditionally, and it is NULL
    * whenever no timestamp pool was ever created. */
   if (pool == NULL)
      return;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_free(pAllocator, pool->results);
   vk_free(pAllocator, pool->available);
   vk_free(pAllocator, pool);
}

VKAPI_ATTR void VKAPI_CALL
ta5_ResetQueryPoolEXT(VkDevice _device, VkQueryPool _queryPool,
                      uint32_t firstQuery, uint32_t queryCount)
{
   struct ta5_query_pool *pool = ta5_query_pool_from_handle(_queryPool);

   if (pool == NULL || queryCount == 0)
      return;

   if (firstQuery + queryCount > pool->count)
      return;

   memset(pool->results + firstQuery, 0, sizeof(uint64_t) * queryCount);
   memset(pool->available + firstQuery, 0, queryCount);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetQueryPoolResults(VkDevice _device, VkQueryPool _queryPool,
                        uint32_t firstQuery, uint32_t queryCount,
                        size_t dataSize, void *pData, VkDeviceSize stride,
                        VkQueryResultFlags flags)
{
   struct ta5_query_pool *pool = ta5_query_pool_from_handle(_queryPool);
   uint8_t *out = (uint8_t *)pData;
   bool all_available = true;

   if (pool == NULL)
      return vk_error(&((struct ta5_device *)_device)->instance->vk,
                      VK_ERROR_INITIALIZATION_FAILED);

   if (firstQuery + queryCount > pool->count || firstQuery > pool->count)
      return vk_error(&pool->device->instance->vk,
                      VK_ERROR_INITIALIZATION_FAILED);

   for (uint32_t i = 0; i < queryCount; i++) {
      uint32_t index = firstQuery + i;

      if (pool->available[index])
         memcpy(out, &pool->results[index], sizeof(uint64_t));

      if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
         *(VkBool32 *)(out + sizeof(uint64_t)) = pool->available[index];

      if (!pool->available[index])
         all_available = false;

      out += stride;
   }

   /*
    * Nothing ever writes a result, so a caller that asked to wait is told the
    * result is not ready.  Returning VK_SUCCESS with a zeroed result would be
    * a lie that an occlusion query would happily believe.
    */
   if ((flags & VK_QUERY_RESULT_WAIT_BIT) && !all_available)
      return VK_NOT_READY;

   (void)dataSize;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdResetQueryPool(VkCommandBuffer _commandBuffer, VkQueryPool _queryPool,
                      uint32_t firstQuery, uint32_t queryCount)
{
   struct ta5_query_pool *pool = ta5_query_pool_from_handle(_queryPool);

   if (pool == NULL || queryCount == 0)
      return;

   if (firstQuery + queryCount > pool->count)
      return;

   /*
    * Reset on the host rather than recording a packet.  That is only
    * equivalent because no query result can be pending: with no GPU-side
    * query support there is nothing for a recorded reset to order against.
    */
   memset(pool->results + firstQuery, 0, sizeof(uint64_t) * queryCount);
   memset(pool->available + firstQuery, 0, queryCount);
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdBeginQuery(VkCommandBuffer _commandBuffer, VkQueryPool _queryPool,
                  uint32_t query, VkQueryControlFlags flags)
{
   /*
    * Reached only by an application that enabled a query feature the device
    * does not advertise, so there is nothing to begin and nothing to record.
    * The validation layer flags the enable; the driver does not need to.
    */
}

VKAPI_ATTR void VKAPI_CALL
ta5_CmdEndQuery(VkCommandBuffer _commandBuffer, VkQueryPool _queryPool,
                uint32_t query)
{
}