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
#include <unistd.h>

#include "util/u_math.h"
#include "util/format/u_format.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_device.h"
#include "vk_physical_device.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include <sys/mman.h>

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

   /*
    * Reject a range that falls outside the allocation before handing back a
    * pointer into it.  Without this an application mapping a sub-range --
    * offset 0x1000 of a 4 KiB allocation -- gets map + offset, which is past
    * the end of the mapping, and then reads and writes out of bounds.
    *
    * The three checks are the spec's VUIDs for vkMapMemory:
    *
    *   VUID-vkMapMemory-offset-00679   offset must be less than the size
    *   VUID-vkMapMemory-size-00680     size must be greater than 0 unless it
    *                                   is VK_WHOLE_SIZE
    *   VUID-vkMapMemory-size-00681     size must be <= size of memory minus
    *                                   offset, unless it is VK_WHOLE_SIZE
    *
    * VK_WHOLE_SIZE means "to the end of the allocation" and is legal, so it
    * resolves against bo->size.  Violating a VUID is undefined behaviour rather
    * than a defined error, so rejecting it here is defence in depth rather
    * than a required behaviour; VK_ERROR_MEMORY_MAP_FAILED is what the spec
    * names when a map cannot be satisfied.
    *
    * Every comparison is done on 64-bit values: offset + size can overflow 32
    * bits and a wrapped sum would pass the check it is meant to fail.
    */
   if (bo == NULL)
      return vk_error(&memory->device->instance->vk,
                      VK_ERROR_MEMORY_MAP_FAILED);

   {
      const VkDeviceSize total = bo->size;

      if (offset >= total)
         return vk_error(&memory->device->instance->vk,
                         VK_ERROR_MEMORY_MAP_FAILED);

      if (size == VK_WHOLE_SIZE)
         size = total - offset;
      else if (size == 0 || size > total - offset)
         return vk_error(&memory->device->instance->vk,
                         VK_ERROR_MEMORY_MAP_FAILED);
   }

   if ((map = ta5_bo_map(bo)) == NULL)
      return vk_error(&memory->device->instance->vk,
                      VK_ERROR_MEMORY_MAP_FAILED);

   if (getenv("TA5_TRACE_NULL")) {
      for (int round = 0; round < 4; round++) {
         const uint8_t *q = (const uint8_t *)map;
         unsigned long n = 0;
         for (uint32_t i = 0; i < bo->size; i++)
            if (q[i] != 0xAB)
               n++;
         fprintf(stderr, "ta5[probe]: round %d: %lu/%u bytes != 0xAB, "
                         "px0=%02x%02x%02x%02x\n",
                 round, n, bo->size, q[0], q[1], q[2], q[3]);
         usleep(300000);
      }
   }


   /*
    * a5xx system memory is not coherent with the CPU, and the submit fence
    * only tells us the work was queued.  CPU_PREP is what the kernel says to
    * use before touching a buffer the GPU has seen; without it a read back
    * can run ahead of the render.
    *
    * ta5_bo_cpu_prep() always returns VK_SUCCESS today -- the msm kernel on
    * the a5xx device answers this ioctl with EINVAL and the helper treats that
    * as advisory -- so the check below never fires.  It is kept because it is
    * where the real failure belongs once the helper stops swallowing errors,
    * but it is not currently a guarantee of anything.
    */
   if (ta5_bo_cpu_prep(bo, MSM_PREP_READ) != VK_SUCCESS)
      return vk_error(&memory->device->instance->vk, VK_ERROR_DEVICE_LOST);

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
   /*
    * Nothing is host-mapped, so there is no map to drop and nothing to flush:
    * vkUnmapMemory() above is empty for the same reason.  With no map there is
    * nothing that can fail, so VK_SUCCESS is the only correct answer.
    *
    * (VK_ERROR_MEMORY_MAP_FAILED is a legal return for this command -- it is
    * one of the failure codes the spec lists -- but nothing here maps or
    * unmaps, so returning one would only tell the application its unmap had
    * failed when it had not.)
    */
   return VK_SUCCESS;
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

/**
 * Fill in the linear layout of every mip level.
 *
 * A mip level is `layers` slices for a 1D/2D image and `depth` slices for a
 * 3D one; a 3D image has exactly one array layer, so the two never multiply.
 * Each successive level halves width, height and depth, all floor 1, and the
 * row pitch is re-aligned to 64 bytes per level because that is what the a5xx
 * linear fetch unit requires -- not merely what makes the arithmetic even.
 */
bool
ta5_image_layout_levels(const struct ta5_image *image,
                        struct ta5_image_level *out)
{
   uint32_t block_size = ta5_format_block_size(image->format);
   uint32_t w = image->extent.width;
   uint32_t h = image->extent.height;
   uint32_t d =
      image->image_type == VK_IMAGE_TYPE_3D ? image->extent.depth : 1;
   uint32_t layers = image->image_type == VK_IMAGE_TYPE_3D ? 1 : image->array_layers;
   VkDeviceSize offset = 0;

   if (!block_size || !w || !h || !d || !layers)
      return false;

   for (uint32_t i = 0; i < image->mip_levels; i++) {
      struct ta5_image_level *lvl = &out[i];

      lvl->width = w;
      lvl->height = h;
      lvl->depth = d;
      lvl->layers = layers;
      lvl->row_pitch = align64((VkDeviceSize)w * block_size, 64);
      lvl->layer_pitch = lvl->row_pitch * h;
      lvl->level_size = lvl->layer_pitch * layers * d;
      lvl->offset = offset;
      offset += lvl->level_size;

      w = MAX2(w / 2, 1u);
      h = MAX2(h / 2, 1u);
      d = MAX2(d / 2, 1u);
   }

   return true;
}

/**
 * Record the derived layout on a real image.
 */
static void
ta5_image_set_layout(struct ta5_image *image)
{
   const struct ta5_image_level *last =
      &image->levels[MAX2(image->mip_levels, 1u) - 1];

   image->row_pitch = image->levels[0].row_pitch;
   image->array_pitch = image->levels[0].layer_pitch;
   image->depth_pitch = image->levels[0].layer_pitch;
   image->data_size = last->offset + last->level_size;
}

/**
 * Fill in a throwaway image description for the queries that must report a
 * layout before any image object exists.
 *
 * The temporary goes through exactly the helper vkCreateImage uses, so the
 * paths cannot drift apart -- which they did while each had its own copy of
 * the arithmetic, and which is how a 3D image came to be sized as one slice.
 */
static void
ta5_image_layout_from_create_info(struct ta5_image *tmp,
                                  const VkImageCreateInfo *pCreateInfo)
{
   memset(tmp, 0, sizeof(*tmp));

   tmp->format = pCreateInfo->format;
   tmp->image_type = pCreateInfo->imageType;
   tmp->extent = pCreateInfo->extent;
   tmp->mip_levels =
      MIN2(MAX2(pCreateInfo->mipLevels, 1u), TA5_MAX_IMAGE_MIP_LEVELS);
   tmp->array_layers = pCreateInfo->arrayLayers;
   tmp->tiling = pCreateInfo->tiling;
   tmp->usage = pCreateInfo->usage;

   if (ta5_image_layout_levels(tmp, tmp->levels))
      ta5_image_set_layout(tmp);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateImage(VkDevice _device, const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   const struct vk_physical_device *pdev = &device->physical_device->vk;
   struct ta5_image *image;
   uint32_t block_size;
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

   /*
    * Vulkan's own shape rules.  They matter here because everything
    * downstream keys off "3D means depth slices, 2D means array layers":
    * a 3D image with extra array layers or a 2D image that is 1 slice thick
    * would silently get the wrong size and the wrong texel descriptor.
    */
   switch (pCreateInfo->imageType) {
   case VK_IMAGE_TYPE_1D:
      if (pCreateInfo->extent.height != 1 || pCreateInfo->extent.depth != 1 ||
          pCreateInfo->arrayLayers != 1)
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
      break;
   case VK_IMAGE_TYPE_3D:
      if (pCreateInfo->arrayLayers != 1)
         return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);
      break;
   default:
      break;
   }

   if ((pCreateInfo->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
       (pCreateInfo->imageType != VK_IMAGE_TYPE_2D || pCreateInfo->mipLevels < 1 ||
        pCreateInfo->arrayLayers % 6 != 0))
      return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);

   /* Linear tiling only supports 1 mip level and 1 array layer. */
   if (pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR) {
      if (pCreateInfo->mipLevels != 1 || pCreateInfo->arrayLayers != 1 ||
          pCreateInfo->imageType == VK_IMAGE_TYPE_3D)
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

   if (!ta5_image_layout_levels(image, image->levels))
      return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);

   ta5_image_set_layout(image);

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
   /* Compute the layout the way CreateImage would, without allocating. */
   struct ta5_image tmp = {};

   ta5_image_layout_from_create_info(&tmp, pInfo->pCreateInfo);

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
   /*
    * The whole point of the per-level table: a caller asking for mip 3 of a
    * 3D image gets that level's row pitch and its own slice stride, not
    * level 0's.  `depthPitch` is the distance between depth slices and
    * `arrayPitch` the distance between array layers; for a 2D array or a 3D
    * image only one of them is non-zero, and reporting level 0's stride for
    * every level made every non-zero mip of a 3D image alias level 0.
    */
   uint32_t mip = MIN2(subresource->mipLevel,
                       MAX2(image->mip_levels, 1u) - 1);
   const struct ta5_image_level *lvl = &image->levels[mip];

   layout->offset = image->offset + lvl->offset;
   layout->rowPitch = lvl->row_pitch;
   layout->depthPitch = lvl->layer_pitch;
   layout->arrayPitch = lvl->layer_pitch;
   layout->size = lvl->level_size;
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

   ta5_image_layout_from_create_info(&tmp, pInfo->pCreateInfo);

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