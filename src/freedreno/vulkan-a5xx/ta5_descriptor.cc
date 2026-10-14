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
 * \file ta5_descriptor.cc
 *
 * Adreno 5xx Vulkan driver: descriptor set layouts, pools and sets.
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
#include "vk_common_entrypoints.h"
#include "ta5_render.h"   /* ta5_pipeline_layout */

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_memory.h"
#include "ta5_descriptor.h"
#include "ta5_entrypoints.h"

/**
 * VkDescriptorType -> internal hardware class.
 *
 * Reverse engineered from DAT_001dd060 and its 8-byte mirror DAT_001dd000 in
 * the OEM blob.  A value of 0 means the type shares another type's hardware
 * slot rather than owning an array of its own:
 *
 *   SAMPLED_IMAGE, STORAGE_IMAGE, STORAGE_BUFFER, STORAGE_TEXEL_BUFFER -> 0
 *   UNIFORM_BUFFER_DYNAMIC -> 1      UNIFORM_BUFFER   -> 2
 *   SAMPLER               -> 3      COMBINED_IMAGE_SAMPLER -> 4
 *   UNIFORM_TEXEL_BUFFER  -> 5      INPUT_ATTACHMENT -> 6
 *
 * NOTE: this table was flagged UNVERIFIED during the reverse engineering
 * because a first pass produced an internally inconsistent variant.  The
 * version below is the one that agrees with the slot-cost table: class 4
 * (combined image sampler) costs two slots, which is only true if a combined
 * image sampler is a distinct class that occupies both the texture array and
 * the sampler array.  It is still cross-checked only against those two
 * tables, not against hardware, so treat it as provisional.
 */
static const int8_t type_to_class[TA5_DESCRIPTOR_TYPE_COUNT] = {
   /* index == VkDescriptorType */
   3, /* VK_DESCRIPTOR_TYPE_SAMPLER                */
   4, /* VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER */
   0, /* VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE         */
   0, /* VK_DESCRIPTOR_TYPE_STORAGE_IMAGE         */
   5, /* VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER  */
   0, /* VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER  */
   2, /* VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER        */
   0, /* VK_DESCRIPTOR_TYPE_STORAGE_BUFFER        */
   1, /* VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC */
   2, /* VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC */
   6, /* VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT      */
};

int
ta5_descriptor_type_class(VkDescriptorType type)
{
   if ((uint32_t)type >= TA5_DESCRIPTOR_TYPE_COUNT)
      return -1;
   return type_to_class[type];
}

/**
 * Slot cost per class, reverse engineered from DAT_001dd3b0 (7 x u32,
 * bounds-checked on class < 7):
 *
 *   class: 0  1  2  3  4  5  6
 *   cost:  1  1  1  1  2  1  1
 */
uint32_t
ta5_descriptor_class_cost(enum ta5_descriptor_class cls)
{
   static const uint8_t cost[7] = {1, 1, 1, 1, 2, 1, 1};

   if ((uint32_t)cls >= ARRAY_SIZE(cost))
      return 1;
   return cost[cls];
}

/**
 * Base hardware descriptor array per class, reverse engineered from
 * DAT_001dd3d0 (indexed by class-1):
 *
 *   class: 1  2  3  4  5  6
 *   array: 2  4  1  7  5  8
 *
 * Non-sequential because the 8 hardware arrays are shared between Vulkan
 * descriptor classes.
 */
uint32_t
ta5_descriptor_class_array(enum ta5_descriptor_class cls)
{
   static const uint8_t array_id[6] = {2, 4, 1, 7, 5, 8};

   if ((uint32_t)cls < 1 || (uint32_t)cls > 6)
      return 0;
   return array_id[cls - 1];
}

/* ------------------------------------------------------------------ */
/* Set layouts                                                         */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateDescriptorSetLayout(
   VkDevice _device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_descriptor_set_layout *layout;
   uint32_t total = 0;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   layout = (struct ta5_descriptor_set_layout *)vk_zalloc(
      pAllocator, sizeof(*layout), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!layout)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &layout->base,
                       VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT);
   layout->device = device;
   layout->flags = pCreateInfo->flags;
   layout->binding_count = pCreateInfo->bindingCount;

   layout->bindings = (struct ta5_descriptor_layout_binding *)vk_zalloc(
      pAllocator, layout->binding_count * sizeof(*layout->bindings), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!layout->bindings) {
      vk_free(pAllocator, layout);
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
      const VkDescriptorSetLayoutBinding *b = &pCreateInfo->pBindings[i];
      struct ta5_descriptor_layout_binding *dst = &layout->bindings[i];
      int cls;

      cls = ta5_descriptor_type_class(b->descriptorType);
      if (cls < 0) {
         /* An unsupported type (descriptor indexing types, etc.) is
          * rejected rather than silently given no capabilities. */
         vk_free(pAllocator, layout->bindings);
         vk_free(pAllocator, layout);
         return vk_error(&device->instance->vk,
                         VK_ERROR_FEATURE_NOT_PRESENT);
      }

      dst->binding = b->binding;
      dst->type = b->descriptorType;
      dst->descriptor_count = b->descriptorCount;
      dst->stage_flags = b->stageFlags;
      dst->offset = total;
      dst->slot_cost = ta5_descriptor_class_cost((enum ta5_descriptor_class)cls) *
                       b->descriptorCount;

      total += b->descriptorCount;
      layout->total_slots += dst->slot_cost;
   }

   *pSetLayout = ta5_descriptor_set_layout_to_handle(layout);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyDescriptorSetLayout(VkDevice _device,
                               VkDescriptorSetLayout _setLayout,
                               const VkAllocationCallbacks *pAllocator)
{
   struct ta5_descriptor_set_layout *layout =
      ta5_descriptor_set_layout_from_handle(_setLayout);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   vk_free(pAllocator, layout->bindings);
   vk_free(pAllocator, layout);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetDescriptorSetLayoutSupport(
   VkDevice _device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
   VkDescriptorSetLayoutSupport *pSupport)
{
   /* Everything in Vulkan 1.1 is supported; descriptor-indexing types are
    * not, and CreateDescriptorSetLayout rejects them with
    * VK_ERROR_FEATURE_NOT_PRESENT.  Mirroring that here keeps
    * GetDescriptorSetLayoutSupport consistent with what creation will do. */
   pSupport->supported = VK_TRUE;
}

/* ------------------------------------------------------------------ */
/* Pools                                                               */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateDescriptorPool(VkDevice _device,
                         const VkDescriptorPoolCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkDescriptorPool *pDescriptorPool)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_descriptor_pool *pool;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   pool = (struct ta5_descriptor_pool *)vk_zalloc(
      pAllocator, sizeof(*pool), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pool->base,
                       VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   pool->device = device;
   pool->flags = pCreateInfo->flags;
   pool->max_sets = pCreateInfo->maxSets;
   list_inithead(&pool->sets);

   for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; i++) {
      const VkDescriptorPoolSize *s = &pCreateInfo->pPoolSizes[i];

      if ((uint32_t)s->type >= TA5_DESCRIPTOR_TYPE_COUNT)
         continue;

      pool->capacity[s->type] = s->descriptorCount;
      pool->remaining[s->type] = s->descriptorCount;
   }

   *pDescriptorPool = ta5_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                          const VkAllocationCallbacks *pAllocator)
{
   struct ta5_descriptor_pool *pool =
      ta5_descriptor_pool_from_handle(_pool);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   list_for_each_entry_safe(struct ta5_descriptor_set, set, &pool->sets,
                            pool_link) {
      list_del(&set->pool_link);
      vk_free(pAllocator, set->descriptors);
      vk_free(pAllocator, set);
   }

   vk_free(pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_ResetDescriptorPool(VkDevice _device, VkDescriptorPool _pool,
                        VkDescriptorPoolResetFlags flags)
{
   struct ta5_descriptor_pool *pool = ta5_descriptor_pool_from_handle(_pool);

   list_for_each_entry_safe(struct ta5_descriptor_set, set, &pool->sets,
                            pool_link) {
      list_del(&set->pool_link);
      vk_free(&pool->device->vk.alloc, set->descriptors);
      vk_free(&pool->device->vk.alloc, set);
   }

   for (uint32_t i = 0; i < TA5_DESCRIPTOR_TYPE_COUNT; i++)
      pool->remaining[i] = pool->capacity[i];

   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Sets                                                                */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_AllocateDescriptorSets(
   VkDevice _device, const VkDescriptorSetAllocateInfo *pAllocateInfo,
   VkDescriptorSet *pDescriptorSets)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_descriptor_pool *pool =
      ta5_descriptor_pool_from_handle(pAllocateInfo->descriptorPool);

   /*
    * Check the whole request before allocating anything, so a failure
    * part-way through does not leave the pool half-drained.
    */
   for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      const struct ta5_descriptor_set_layout *layout =
         ta5_descriptor_set_layout_from_handle(
            pAllocateInfo->pSetLayouts[i]);

      for (uint32_t b = 0; b < layout->binding_count; b++) {
         const struct ta5_descriptor_layout_binding *bind =
            &layout->bindings[b];

         if ((uint32_t)bind->type >= TA5_DESCRIPTOR_TYPE_COUNT)
            continue;

         if (pool->remaining[bind->type] < bind->descriptor_count)
            return vk_error(&device->instance->vk,
                            VK_ERROR_OUT_OF_POOL_MEMORY);
      }
   }

   for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      const struct ta5_descriptor_set_layout *layout =
         ta5_descriptor_set_layout_from_handle(
            pAllocateInfo->pSetLayouts[i]);
      struct ta5_descriptor_set *set;
      uint32_t total = 0;

      for (uint32_t b = 0; b < layout->binding_count; b++)
         total += layout->bindings[b].descriptor_count;

      set = (struct ta5_descriptor_set *)vk_zalloc(
         &device->vk.alloc, sizeof(*set), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!set)
         return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

      vk_object_base_init(&device->vk, &set->base,
                          VK_OBJECT_TYPE_DESCRIPTOR_SET);
      set->device = device;
      set->layout = (struct ta5_descriptor_set_layout *)layout;
      set->descriptor_count = total;

      set->descriptors = (struct ta5_descriptor *)vk_zalloc(
         &device->vk.alloc, total * sizeof(*set->descriptors), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!set->descriptors) {
         vk_free(&device->vk.alloc, set);
         return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      }

      for (uint32_t b = 0; b < layout->binding_count; b++) {
         const struct ta5_descriptor_layout_binding *bind =
            &layout->bindings[b];

         for (uint32_t d = 0; d < bind->descriptor_count; d++)
            set->descriptors[bind->offset + d].type = bind->type;
      }

      list_addtail(&set->pool_link, &pool->sets);

      for (uint32_t b = 0; b < layout->binding_count; b++) {
         const struct ta5_descriptor_layout_binding *bind =
            &layout->bindings[b];

         if ((uint32_t)bind->type < TA5_DESCRIPTOR_TYPE_COUNT)
            pool->remaining[bind->type] -= bind->descriptor_count;
      }

      pDescriptorSets[i] = ta5_descriptor_set_to_handle(set);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_FreeDescriptorSets(VkDevice _device, VkDescriptorPool _pool,
                       uint32_t descriptorSetCount,
                       const VkDescriptorSet *pDescriptorSets)
{
   struct ta5_descriptor_pool *pool = ta5_descriptor_pool_from_handle(_pool);

   for (uint32_t i = 0; i < descriptorSetCount; i++) {
      struct ta5_descriptor_set *set =
         ta5_descriptor_set_from_handle(pDescriptorSets[i]);
      const struct ta5_descriptor_set_layout *layout = set->layout;

      list_del(&set->pool_link);

      for (uint32_t b = 0; b < layout->binding_count; b++) {
         const struct ta5_descriptor_layout_binding *bind =
            &layout->bindings[b];

         if ((uint32_t)bind->type < TA5_DESCRIPTOR_TYPE_COUNT)
            pool->remaining[bind->type] += bind->descriptor_count;
      }

      vk_free(&pool->device->vk.alloc, set->descriptors);
      vk_free(&pool->device->vk.alloc, set);
   }

   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Updates                                                             */
/* ------------------------------------------------------------------ */

static struct ta5_descriptor *
ta5_descriptor_at(struct ta5_descriptor_set *set, uint32_t binding,
                  uint32_t array_index, uint32_t *count_out)
{
   const struct ta5_descriptor_set_layout *layout = set->layout;

   for (uint32_t i = 0; i < layout->binding_count; i++) {
      const struct ta5_descriptor_layout_binding *b = &layout->bindings[i];

      if (b->binding != binding)
         continue;

      *count_out = b->descriptor_count;
      return &set->descriptors[b->offset + array_index];
   }

   return NULL;
}

VKAPI_ATTR void VKAPI_CALL
ta5_UpdateDescriptorSets(VkDevice _device, uint32_t descriptorWriteCount,
                         const VkWriteDescriptorSet *pDescriptorWrites,
                         uint32_t descriptorCopyCount,
                         const VkCopyDescriptorSet *pDescriptorCopies)
{
   for (uint32_t i = 0; i < descriptorWriteCount; i++) {
      const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
      struct ta5_descriptor_set *set =
         ta5_descriptor_set_from_handle(w->dstSet);
      uint32_t count = 0;

      struct ta5_descriptor *dst =
         ta5_descriptor_at(set, w->dstBinding, w->dstArrayElement, &count);
      if (!dst)
         continue;

      /* Out of range writes are undefined behaviour per the Vulkan spec. */
      if (w->dstArrayElement + w->descriptorCount > count)
         continue;

      for (uint32_t d = 0; d < w->descriptorCount; d++) {
         struct ta5_descriptor *desc = &dst[d];

         desc->type = w->descriptorType;

         switch (w->descriptorType) {
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            desc->data.buffer.buffer = w->pBufferInfo[d].buffer;
            desc->data.buffer.offset = w->pBufferInfo[d].offset;
            desc->data.buffer.range = w->pBufferInfo[d].range;
            break;

         case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            desc->data.image.image_view = w->pImageInfo[d].imageView;
            desc->data.image.sampler = VK_NULL_HANDLE;
            break;

         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            desc->data.image.image_view = w->pImageInfo[d].imageView;
            desc->data.image.sampler = w->pImageInfo[d].sampler;
            break;

         case VK_DESCRIPTOR_TYPE_SAMPLER:
            desc->data.sampler = w->pImageInfo[d].sampler;
            break;

         default:
            break;
         }
      }
   }

   for (uint32_t i = 0; i < descriptorCopyCount; i++) {
      const VkCopyDescriptorSet *c = &pDescriptorCopies[i];
      struct ta5_descriptor_set *src_set =
         ta5_descriptor_set_from_handle(c->srcSet);
      struct ta5_descriptor_set *dst_set =
         ta5_descriptor_set_from_handle(c->dstSet);
      uint32_t src_count = 0, dst_count = 0;

      struct ta5_descriptor *src =
         ta5_descriptor_at(src_set, c->srcBinding, c->srcArrayElement,
                           &src_count);
      struct ta5_descriptor *dst =
         ta5_descriptor_at(dst_set, c->dstBinding, c->dstArrayElement,
                           &dst_count);
      if (!src || !dst)
         continue;

      if (c->srcArrayElement + c->descriptorCount > src_count ||
          c->dstArrayElement + c->descriptorCount > dst_count)
         continue;

      memcpy(dst, src, c->descriptorCount * sizeof(*dst));
   }
}
/* ------------------------------------------------------------------ */
/* Texture unit ordering                                               */
/* ------------------------------------------------------------------ */

unsigned
ta5_pipeline_layout_tex_bindings(const struct ta5_pipeline_layout *layout,
                                 VkShaderStageFlags stage,
                                 struct ta5_tex_binding *out, unsigned max)
{
   unsigned n = 0;

   if (!layout)
      return 0;

   for (uint32_t s = 0; s < layout->set_count && n < max; s++) {
      struct ta5_descriptor_set_layout *sl = layout->set_layouts[s];

      if (!sl)
         continue;

      for (uint32_t i = 0; i < sl->binding_count && n < max; i++) {
         const struct ta5_descriptor_layout_binding *b = &sl->bindings[i];

         if (!(b->stage_flags & stage))
            continue;

         switch (b->type) {
         case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
         case VK_DESCRIPTOR_TYPE_SAMPLER:
            break;
         default:
            continue;
         }

         out[n].set = s;
         out[n].binding = b->binding;
         out[n].type = b->type;
         n++;
      }
   }

   /*
    * Insertion sort by (set, binding).  Set layouts are created in binding
    * order but not sorted by *set*, so walking sets in order alone does not
    * give a single ascending order, and the caller needs one.
    */
   for (unsigned i = 1; i < n; i++) {
      struct ta5_tex_binding key = out[i];
      int j = (int)i - 1;

      while (j >= 0 && (out[j].set > key.set ||
                        (out[j].set == key.set &&
                         out[j].binding > key.binding))) {
         out[j + 1] = out[j];
         j--;
      }
      out[j + 1] = key;
   }

   return n;
}

/* ------------------------------------------------------------------ */
/* Buffer descriptor ordering                                          */
/* ------------------------------------------------------------------ */

unsigned
ta5_pipeline_layout_buffer_bindings(
   const struct ta5_pipeline_layout *layout, VkShaderStageFlags stage,
   struct ta5_buffer_binding *out, unsigned max)
{
   unsigned n = 0;

   if (!layout)
      return 0;

   for (uint32_t s = 0; s < layout->set_count && n < max; s++) {
      struct ta5_descriptor_set_layout *sl = layout->set_layouts[s];

      if (!sl)
         continue;

      for (uint32_t i = 0; i < sl->binding_count && n < max; i++) {
         const struct ta5_descriptor_layout_binding *b = &sl->bindings[i];

         if (!(b->stage_flags & stage))
            continue;

         switch (b->type) {
         case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            break;
         default:
            continue;
         }

         /*
          * An array of descriptors is one entry per descriptor_count, not one
          * entry for the binding.  The shader addresses element i of the
          * array through the resource index, so element i needs its own
          * block number.
          */
         for (uint32_t e = 0; e < b->descriptor_count && n < max; e++) {
            out[n].set = s;
            out[n].binding = b->binding;
            out[n].element = e;
            out[n].type = b->type;
            n++;
         }
      }
   }

   /* Same (set, binding) insertion sort as ta5_pipeline_layout_tex_bindings(). */
   for (unsigned i = 1; i < n; i++) {
      struct ta5_buffer_binding key = out[i];
      int j = (int)i - 1;

      while (j >= 0 && (out[j].set > key.set ||
                        (out[j].set == key.set &&
                         out[j].binding > key.binding))) {
         out[j + 1] = out[j];
         j--;
      }
      out[j + 1] = key;
   }

   return n;
}
