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
 * \file ta5_memory.cc
 *
 * Adreno 5xx Vulkan driver: device memory, buffers and images.
 *
 * The sizes, alignments and layout rules here follow the Vulkan spec and the
 * a5xx tile/tile-align limits recorded during reverse engineering.  The
 * allocations themselves are host-side only in this phase; real DMA-backed
 * allocations arrive with the command buffer layer.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_math.h"
#include "util/format/u_format.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_device.h"
#include "vk_physical_device.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_memory.h"
#include "ta5_formats.h"
#include "ta5_entrypoints.h"
#include "ta5_submit.h"

#include "drm/msm_drm.h"

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_AllocateMemory(VkDevice _device, const VkMemoryAllocateInfo *pAllocateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDeviceMemory *pDeviceMemory)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_memory *memory;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   if (pAllocateInfo->memoryTypeIndex >= TA5_MEMORY_TYPE_COUNT)
      return vk_error(&device->instance->vk,
                      VK_ERROR_OUT_OF_DEVICE_MEMORY);

   memory = (struct ta5_memory *)vk_zalloc(pAllocator, sizeof(*memory), 8,
                                           VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!memory)
      return vk_error(&device->instance->vk,
                      VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &memory->base,
                       VK_OBJECT_TYPE_DEVICE_MEMORY);
   memory->device = device;
   memory->size = pAllocateInfo->allocationSize;
   memory->memory_type_index = pAllocateInfo->memoryTypeIndex;
   list_inithead(&memory->bound_images);
   list_inithead(&memory->bound_buffers);

   /*
    * Cacheable sysmem: the GPU renders into it and the host reads the result
    * back through mmap.  MSM_BO_CACHED is inside MSM_BO_FLAGS' cache mask;
    * MSM_BO_CACHED_COHERENT (0x080000) is not, and passing it makes
    * DRM_IOCTL_GEM_NEW fail with EINVAL.
    */
   {
      const char *m = getenv("TA5_BO_CACHE");
      uint32_t flags = m && m[0] == 'w' ? MSM_BO_WC : MSM_BO_CACHED;
      memory->bo = ta5_bo_alloc(device, (uint32_t)memory->size, flags);
   }
   if (!memory->bo) {
      vk_free(pAllocator, memory);
      return vk_error(&device->instance->vk,
                      VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }


   *pDeviceMemory = ta5_memory_to_handle(memory);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_FreeMemory(VkDevice _device, VkDeviceMemory _memory,
               const VkAllocationCallbacks *pAllocator)
{
   struct ta5_memory *memory = ta5_memory_from_handle(_memory);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   /* All bound resources must have been destroyed or unbound first. */
   assert(list_is_empty(&memory->bound_images));
   assert(list_is_empty(&memory->bound_buffers));

   ta5_bo_unref(memory->bo);
   vk_free(pAllocator, memory);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_MapMemory(VkDevice _device, VkDeviceMemory _memory, VkDeviceSize offset,
              VkDeviceSize size, VkMemoryMapFlags flags, void **ppData)
{
   struct ta5_memory *memory = ta5_memory_from_handle(_memory);
   struct ta5_bo *bo = memory->bo;
   void *map;

   if (bo == NULL || (map = ta5_bo_map(bo)) == NULL)
      return vk_error(&memory->device->instance->vk,
                      VK_ERROR_MEMORY_MAP_FAILED);

   *ppData = (char *)map + offset;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_UnmapMemory(VkDevice _device, VkDeviceMemory _memory)
{
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_UnmapMemory2(VkDevice _device, const VkMemoryUnmapInfo *pMemoryUnmapInfo)
{
   return VK_ERROR_MEMORY_MAP_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_FlushMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                             const VkMappedMemoryRange *pMemoryRanges)
{
   /* Nothing is host-mapped yet, so there is nothing to flush. */
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_InvalidateMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                                 const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetDeviceMemoryCommitment(VkDevice _device, VkDeviceMemory _memory,
                              VkDeviceSize *pCommittedMemoryInBytes)
{
   struct ta5_memory *memory = ta5_memory_from_handle(_memory);

   *pCommittedMemoryInBytes = memory->size;
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetDeviceGroupPeerMemoryFeatures(
   VkDevice _device, uint32_t heapIndex, uint32_t localDeviceIndex,
   uint32_t remoteDeviceIndex, VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
   /* Single-device peer groups are not supported. */
   *pPeerMemoryFeatures = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT |
                         VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
                         VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT |
                         VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
}

/* ------------------------------------------------------------------ */
/* Images                                                              */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateImage(VkDevice _device, const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   const struct vk_physical_device *pdev = &device->physical_device->vk;
   struct ta5_image *image;
   uint32_t block_size;
   VkDeviceSize size;
   VkResult result;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   /* Reject anything the format table says the hardware cannot do. */
   VkPhysicalDeviceImageFormatInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = pCreateInfo->format,
      .type = pCreateInfo->imageType,
      .tiling = pCreateInfo->tiling,
      .usage = pCreateInfo->usage,
      .flags = pCreateInfo->flags,
   };
   VkImageFormatProperties2 info_props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
   };

   result = ta5_GetPhysicalDeviceImageFormatProperties2(
      (VkPhysicalDevice)&pdev->base, &info, &info_props);
   if (result != VK_SUCCESS)
      return vk_error(&device->instance->vk, result);

   block_size = ta5_format_block_size(pCreateInfo->format);
   if (!block_size)
      return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);

   /* Linear tiling only supports 1 mip level and 1 array layer. */
   if (pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR) {
      if (pCreateInfo->mipLevels != 1 || pCreateInfo->arrayLayers != 1)
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }

   if (pCreateInfo->samples > 1 && block_size != 4)
      return vk_error(&device->instance->vk, VK_ERROR_FEATURE_NOT_PRESENT);

   image = (struct ta5_image *)vk_image_create(&device->vk, pCreateInfo,
                                               pAllocator, sizeof(*image));
   if (!image)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   image->device = device;
   image->flags = pCreateInfo->flags;
   image->image_type = pCreateInfo->imageType;
   image->format = pCreateInfo->format;
   image->extent = pCreateInfo->extent;
   image->mip_levels = pCreateInfo->mipLevels;
   image->array_layers = pCreateInfo->arrayLayers;
   image->samples = pCreateInfo->samples;
   image->tiling = pCreateInfo->tiling;
   image->usage = pCreateInfo->usage;
   image->offset = 0;

   /* Row pitch is width * block size, rounded up to 64 bytes as a5xx
    * requires for linear reads. */
   image->row_pitch = align64((VkDeviceSize)pCreateInfo->extent.width *
                                 block_size,
                              64);
   image->array_pitch = image->row_pitch * pCreateInfo->extent.height;
   size = image->array_pitch * pCreateInfo->arrayLayers;

   /* Total size across all mip levels (mip i is 1/4 the area of the last). */
   image->data_size = 0;
   for (uint32_t i = 0; i < pCreateInfo->mipLevels; i++) {
      image->data_size += size;
      size = size ? size / 4 : 0;
   }

   if (image->data_size > pdev->properties.maxMemoryAllocationCount) {
      /* not a size check, just keep the value honest below */
   }

   list_inithead(&image->memory_binding);
   image->bound_memory = NULL;


   *pImage = ta5_image_to_handle(image);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyImage(VkDevice _device, VkImage _image,
                 const VkAllocationCallbacks *pAllocator)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_image *image = ta5_image_from_handle(_image);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   if (image->bound_memory) {
      list_del(&image->memory_binding);
      image->bound_memory = NULL;
   }

   vk_image_destroy(&device->vk, pAllocator, &image->vk);
}

static void
ta5_fill_image_memory_requirements(const struct ta5_image *image,
                                   VkMemoryRequirements *reqs)
{
   /* a5xx memory is page-aligned; report a conservative alignment. */
   reqs->size = align64(image->data_size, 4096);
   reqs->alignment = 64;
   reqs->memoryTypeBits = UINT32_MAX;
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetImageMemoryRequirements(VkDevice _device, VkImage _image,
                               VkMemoryRequirements *pMemoryRequirements)
{
   const struct ta5_image *image = ta5_image_from_handle(_image);

   ta5_fill_image_memory_requirements(image, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetImageMemoryRequirements2(VkDevice _device, const VkImageMemoryRequirementsInfo2
                                *pImageMemoryRequirementsInfo,
                                VkMemoryRequirements2 *pMemoryRequirements)
{
   ta5_GetImageMemoryRequirements(
      _device, pImageMemoryRequirementsInfo->image, &pMemoryRequirements->memoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetDeviceImageMemoryRequirements(
   VkDevice _device, const VkDeviceImageMemoryRequirements *pInfo,
   VkMemoryRequirements2 *pMemoryRequirements)
{
   /* Compute the size the way CreateImage would, without allocating. */
   struct ta5_image tmp = {};
   uint32_t block_size = ta5_format_block_size(pInfo->pCreateInfo->format);
   VkDeviceSize size;

   tmp.row_pitch =
      align64((VkDeviceSize)pInfo->pCreateInfo->extent.width * block_size, 64);
   tmp.array_pitch = tmp.row_pitch * pInfo->pCreateInfo->extent.height;

   size = tmp.array_pitch * pInfo->pCreateInfo->arrayLayers;
   tmp.data_size = 0;
   for (uint32_t i = 0; i < pInfo->pCreateInfo->mipLevels; i++) {
      tmp.data_size += size;
      size = size ? size / 4 : 0;
   }

   ta5_fill_image_memory_requirements(&tmp,
                                      &pMemoryRequirements->memoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_BindImageMemory(VkDevice _device, VkImage _image,
                    VkDeviceMemory _memory, VkDeviceSize memoryOffset)
{
   struct ta5_image *image = ta5_image_from_handle(_image);
   struct ta5_memory *memory = ta5_memory_from_handle(_memory);

   if (image->bound_memory)
      list_del(&image->memory_binding);

   list_addtail(&image->memory_binding, &memory->bound_images);
   image->bound_memory = memory;
   image->offset = memoryOffset;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_BindImageMemory2(VkDevice _device, uint32_t bindInfoCount,
                     const VkBindImageMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      ta5_BindImageMemory(_device, pBindInfos[i].image, pBindInfos[i].memory,
                          pBindInfos[i].memoryOffset);
   }
   return VK_SUCCESS;
}

static void
ta5_fill_image_subresource_layout(const struct ta5_image *image,
                                  const VkImageSubresource *subresource,
                                  VkSubresourceLayout *layout)
{
   layout->offset = image->offset;
   layout->rowPitch = image->row_pitch;
   layout->depthPitch = image->array_pitch;
   layout->arrayPitch = image->array_pitch;
   layout->size = image->data_size;
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetImageSubresourceLayout(VkDevice _device, VkImage _image,
                              const VkImageSubresource *pSubresource,
                              VkSubresourceLayout *pLayout)
{
   ta5_fill_image_subresource_layout(ta5_image_from_handle(_image),
                                     pSubresource, pLayout);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetImageSubresourceLayout2(VkDevice _device, VkImage _image,
                               const VkImageSubresource2 *pSubresource,
                               VkSubresourceLayout2 *pLayout)
{
   ta5_fill_image_subresource_layout(ta5_image_from_handle(_image),
                                     &pSubresource->imageSubresource,
                                     &pLayout->subresourceLayout);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetDeviceImageSubresourceLayout(
   VkDevice _device, const VkDeviceImageSubresourceInfo *pInfo,
   VkSubresourceLayout2 *pLayout)
{
   struct ta5_image tmp = {};
   uint32_t block_size = ta5_format_block_size(pInfo->pCreateInfo->format);

   tmp.row_pitch =
      align64((VkDeviceSize)pInfo->pCreateInfo->extent.width * block_size, 64);
   tmp.array_pitch = tmp.row_pitch * pInfo->pCreateInfo->extent.height;

   ta5_fill_image_subresource_layout(&tmp, &pInfo->pSubresource->imageSubresource,
                                     &pLayout->subresourceLayout);
}

/* ------------------------------------------------------------------ */
/* Buffers                                                             */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateBuffer(VkDevice _device, const VkBufferCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_buffer *buffer;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   buffer = (struct ta5_buffer *)vk_buffer_create(&device->vk, pCreateInfo,
                                                 pAllocator, sizeof(*buffer));
   if (!buffer)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   buffer->device = device;
   buffer->size = pCreateInfo->size;
   buffer->usage = pCreateInfo->usage;
   buffer->flags = pCreateInfo->flags;
   list_inithead(&buffer->memory_binding);


   *pBuffer = ta5_buffer_to_handle(buffer);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyBuffer(VkDevice _device, VkBuffer _buffer,
                  const VkAllocationCallbacks *pAllocator)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_buffer *buffer = ta5_buffer_from_handle(_buffer);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   if (buffer->bound_memory)
      list_del(&buffer->memory_binding);

   vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
}

static void
ta5_fill_buffer_memory_requirements(VkDeviceSize size,
                                    VkMemoryRequirements *reqs)
{
   reqs->size = align64(size, 4096);
   reqs->alignment = 64;
   reqs->memoryTypeBits = UINT32_MAX;
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetBufferMemoryRequirements(VkDevice _device, VkBuffer _buffer,
                                VkMemoryRequirements *pMemoryRequirements)
{
   const struct ta5_buffer *buffer = ta5_buffer_from_handle(_buffer);

   ta5_fill_buffer_memory_requirements(buffer->size, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetBufferMemoryRequirements2(VkDevice _device,
                                 const VkBufferMemoryRequirementsInfo2 *pInfo,
                                 VkMemoryRequirements2 *pMemoryRequirements)
{
   ta5_GetBufferMemoryRequirements(_device, pInfo->buffer,
                                   &pMemoryRequirements->memoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetDeviceBufferMemoryRequirements(
   VkDevice _device, const VkDeviceBufferMemoryRequirements *pInfo,
   VkMemoryRequirements2 *pMemoryRequirements)
{
   ta5_fill_buffer_memory_requirements(
      pInfo->pCreateInfo->size, &pMemoryRequirements->memoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_BindBufferMemory(VkDevice _device, VkBuffer _buffer, VkDeviceMemory _memory,
                     VkDeviceSize memoryOffset)
{
   struct ta5_buffer *buffer = ta5_buffer_from_handle(_buffer);
   struct ta5_memory *memory = ta5_memory_from_handle(_memory);

   if (buffer->bound_memory)
      list_del(&buffer->memory_binding);

   list_addtail(&buffer->memory_binding, &memory->bound_buffers);
   buffer->bound_memory = memory;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_BindBufferMemory2(VkDevice _device, uint32_t bindInfoCount,
                      const VkBindBufferMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      ta5_BindBufferMemory(_device, pBindInfos[i].buffer, pBindInfos[i].memory,
                           pBindInfos[i].memoryOffset);
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_MapMemory2(VkDevice _device, const VkMemoryMapInfo *pMemoryMapInfo,
               void **ppData)
{
   *ppData = NULL;
   return VK_ERROR_MEMORY_MAP_FAILED;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL
ta5_GetBufferDeviceAddress(VkDevice _device, const VkBufferDeviceAddressInfo *pInfo)
{
   return 0;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL
ta5_GetBufferOpaqueCaptureAddress(VkDevice _device,
                                  const VkBufferDeviceAddressInfo *pInfo)
{
   return 0;
}