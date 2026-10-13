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
 * \file ta5_memory.h
 *
 * Adreno 5xx Vulkan driver: device-level resource objects.
 */

#ifndef TA5_MEMORY_H
#define TA5_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

#include "vk_buffer.h"
#include "vk_image.h"
#include "vk_util.h"
#include "vk_alloc.h"
#include "vk_object.h"
#include "vk_device.h"

#include "ta5_instance.h"

/**
 * Device allocation.
 *
 * a5xx has no dedicated VRAM: allocations are sub-allocations of the single
 * system-memory heap reported by vkGetPhysicalDeviceMemoryProperties2.  This
 * phase performs host allocations only -- no DMA mapping happens until the
 * command buffer layer lands -- but the objects, their sizes and their
 * binding relationships are modelled faithfully so the object-lifetime rules
 * can be enforced.
 */
struct ta5_memory
{
   struct vk_object_base base;
   struct vk_device_dispatch_table dispatch_table;
   struct ta5_device *device;

   VkDeviceSize size;      /**< requested size */
   uint32_t memory_type_index;

   struct list_head bound_images;  /**< list of ta5_image */
   struct list_head bound_buffers; /**< list of ta5_buffer */
   bool is_linear;                 /**< linear tiling */

   /**
    * Backing GEM object.
    *
    * Until now allocation was pure bookkeeping and vkMapMemory reported
    * MEMAP_FAILED.  Once the GPU writes render targets, the allocation has to
    * be a real msm buffer that the command stream can reference through a
    * relocation, and that the host can map for readback.
    */
   struct ta5_bo *bo;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_memory, base, VkDeviceMemory,
                       VK_OBJECT_TYPE_DEVICE_MEMORY)

struct ta5_image
{
   struct vk_image vk;
   struct ta5_device *device;

   VkImageCreateFlags flags;
   VkImageType image_type;
   VkFormat format;
   VkExtent3D extent;
   uint32_t mip_levels;
   uint32_t array_layers;
   VkSampleCountFlagBits samples;
   VkImageTiling tiling;
   VkImageUsageFlags usage;

   /** Offset/stride for linear tiling; tiled layout is assigned later. */
   VkDeviceSize offset;
   VkDeviceSize row_pitch;
   VkDeviceSize array_pitch;
   VkDeviceSize data_size;

   struct list_head memory_binding;
   struct ta5_memory *bound_memory;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)

struct ta5_buffer
{
   struct vk_buffer vk;
   struct ta5_device *device;

   VkDeviceSize size;
   VkBufferUsageFlags usage;
   VkBufferCreateFlags flags;

   struct list_head memory_binding;
   struct ta5_memory *bound_memory;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_buffer, vk.base, VkBuffer, VK_OBJECT_TYPE_BUFFER)

/**
 * Number of memory types reported over the single system-memory heap.
 * Must match ta5_GetPhysicalDeviceMemoryProperties2.
 */
#define TA5_MEMORY_TYPE_COUNT 4

#endif /* TA5_MEMORY_H */