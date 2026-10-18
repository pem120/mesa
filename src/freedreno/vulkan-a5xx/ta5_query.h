/*
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 */

#ifndef TA5_QUERY_H
#define TA5_QUERY_H

#include <stdint.h>

#include "vk_object.h"

VKAPI_ATTR VkResult VKAPI_CALL ta5_CreateQueryPool(
   VkDevice device, const VkQueryPoolCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator, VkQueryPool *pQueryPool);

VKAPI_ATTR void VKAPI_CALL ta5_DestroyQueryPool(
   VkDevice device, VkQueryPool queryPool,
   const VkAllocationCallbacks *pAllocator);

/*
 * VK_EXT_host_query_reset, not the Vulkan 1.2 core vkResetQueryPool: this
 * tree's Vulkan headers predate it.  wsi_common asks for ResetQueryPoolEXT
 * (wsi_common.c:199), so that is the one that has to exist.
 */
VKAPI_ATTR void VKAPI_CALL ta5_ResetQueryPoolEXT(VkDevice device,
                                                  VkQueryPool queryPool,
                                                  uint32_t firstQuery,
                                                  uint32_t queryCount);

VKAPI_ATTR VkResult VKAPI_CALL ta5_GetQueryPoolResults(
   VkDevice device, VkQueryPool queryPool, uint32_t firstQuery,
   uint32_t queryCount, size_t dataSize, void *pData, VkDeviceSize stride,
   VkQueryResultFlags flags);

VKAPI_ATTR void VKAPI_CALL ta5_CmdResetQueryPool(VkCommandBuffer commandBuffer,
                                                 VkQueryPool queryPool,
                                                 uint32_t firstQuery,
                                                 uint32_t queryCount);

VKAPI_ATTR void VKAPI_CALL ta5_CmdBeginQuery(VkCommandBuffer commandBuffer,
                                             VkQueryPool queryPool,
                                             uint32_t query,
                                             VkQueryControlFlags flags);

VKAPI_ATTR void VKAPI_CALL ta5_CmdEndQuery(VkCommandBuffer commandBuffer,
                                           VkQueryPool queryPool,
                                           uint32_t query);

#endif /* TA5_QUERY_H */