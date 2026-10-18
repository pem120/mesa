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
 * \file ta5_descriptor.h
 *
 * Adreno 5xx Vulkan driver: bindful descriptors.
 *
 * a5xx predates bindless descriptors: every descriptor bound at a given
 * binding number is copied into a per-shader-stage hardware array at bind
 * time, at a fixed stride, and the shader reads it by index rather than by
 * address.  There is no descriptor heap and therefore no room for
 * VK_EXT_descriptor_indexing -- see A5XX_VULKAN_SPEC.md section 3.
 *
 * The hardware has 8 descriptor arrays.  Vulkan descriptor classes are mapped
 * onto them by a non-sequential table (Vulkan classes 1..6 -> arrays
 * 2, 4, 1, 7, 5, 8), and each class has a slot cost: a combined image sampler
 * costs two slots because it occupies both the texture array and the sampler
 * array.
 *
 * Those two tables are reproduced below exactly as reverse engineered.
 */

#ifndef TA5_DESCRIPTOR_H
#define TA5_DESCRIPTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "util/list.h"
#include "vk_object.h"
#include "vk_device.h"

#include "ta5_instance.h"

/** Highest VkDescriptorType this driver understands (Vulkan 1.1: 10). */
#define TA5_DESCRIPTOR_TYPE_COUNT 11

/**
 * VkDescriptorType -> internal hardware class.
 *
 * Reverse engineered from DAT_001dd060 / its 8-byte mirror DAT_001dd000.
 * Classes 1..6 each map to a base descriptor array (see
 * ta5_descriptor_class_array()); class 0 means "shares another class's slot".
 */
enum ta5_descriptor_class
{
   TA5_DESC_CLASS_SHARED = 0,
   TA5_DESC_CLASS_1 = 1,
   TA5_DESC_CLASS_2 = 2,
   TA5_DESC_CLASS_3 = 3,
   TA5_DESC_CLASS_4 = 4,
   TA5_DESC_CLASS_5 = 5,
   TA5_DESC_CLASS_6 = 6,
};

/**
 * Number of hardware descriptor slots a class consumes.
 *
 * Reverse engineered from DAT_001dd3b0.  Class 4 costs two: a combined image
 * sampler occupies both a texture slot and a sampler slot.
 */
uint32_t ta5_descriptor_class_cost(enum ta5_descriptor_class cls);

/**
 * Base hardware descriptor array for a class, or 0 if it shares one.
 *
 * Reverse engineered from DAT_001dd3d0, indexed by class-1.  Classes 1..6 map
 * to arrays 2, 4, 1, 7, 5, 8 out of the 8 hardware arrays.
 */
uint32_t ta5_descriptor_class_array(enum ta5_descriptor_class cls);

/** Hardware class for a VkDescriptorType, or -1 if unsupported. */
int ta5_descriptor_type_class(VkDescriptorType type);

/** One entry of a descriptor set layout. */
struct ta5_descriptor_layout_binding
{
   uint32_t binding;
   VkDescriptorType type;
   uint32_t descriptor_count;
   uint32_t stage_flags; /**< VkShaderStageFlags */

   /** Offset into the set's descriptor array, in descriptors. */
   uint32_t offset;

   /** Hardware slots this binding consumes. */
   uint32_t slot_cost;

   /** First dynamic offset in binding/array-element order, or UINT32_MAX. */
   uint32_t dynamic_offset_index;
};

struct ta5_descriptor_set_layout
{
   struct vk_object_base base;
   struct ta5_device *device;

   VkDescriptorSetLayoutCreateFlags flags;

   uint32_t binding_count;
   struct ta5_descriptor_layout_binding *bindings;
   uint32_t dynamic_offset_count;

   /** Total hardware slots the whole layout needs. */
   uint32_t total_slots;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_descriptor_set_layout, base,
                               VkDescriptorSetLayout,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)

/** A bound descriptor.  Tagged by type, as Vulkan allows reuse of a slot. */
struct ta5_descriptor
{
   VkDescriptorType type;

   union {
      struct {
         VkBuffer buffer;
         VkDeviceSize offset;
         VkDeviceSize range;
      } buffer;

      struct {
         VkImageView image_view;
         VkSampler sampler;
      } image;

      VkSampler sampler;
   } data;
};

/**
 * Descriptor set.
 *
 * Layout is fixed at allocation time; descriptors are written by
 * vkUpdateDescriptorSets and packed into GPU-visible memory at bind time
 * (64 bytes per texture, 16 per sampler, per A5XX_VULKAN_SPEC.md 3.1).
 */
struct ta5_descriptor_set
{
   struct vk_object_base base;
   struct ta5_device *device;
   struct ta5_descriptor_set_layout *layout;

   uint32_t descriptor_count;
   struct ta5_descriptor *descriptors;

   /** Link in ta5_descriptor_pool::sets */
   struct list_head pool_link;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_descriptor_set, base, VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)

struct ta5_descriptor_pool
{
   struct vk_object_base base;
   struct ta5_device *device;

   VkDescriptorPoolCreateFlags flags;
   uint32_t max_sets;

   struct list_head sets;

   /** Remaining descriptors, indexed by VkDescriptorType. */
   uint32_t remaining[TA5_DESCRIPTOR_TYPE_COUNT];
   uint32_t capacity[TA5_DESCRIPTOR_TYPE_COUNT];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_descriptor_pool, base, VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

/** Maximum texture units a5xx can bind per stage. */
#define TA5_MAX_TEX_UNITS 16

/** One buffer descriptor, in the order it becomes a UBO pointer slot. */
struct ta5_buffer_binding
{
   uint32_t set;
   uint32_t binding;
   uint32_t element;   /**< index within a descriptor array binding */
   VkDescriptorType type;
};

/** One image or sampler binding, in the order it becomes a texel unit. */
struct ta5_tex_binding
{
   uint32_t set;
   uint32_t binding;
   VkDescriptorType type;
};

/**
 * Collect the image and sampler bindings a stage can see, in the order they
 * become texture and sampler units: ascending (set, binding).
 *
 * a5xx has no descriptor heap, so a shader names a texture unit by number and
 * the draw has to put the right descriptor in that slot.  Two places need
 * that number and they must agree exactly: the draw-time emitter, which fills
 * the slots, and the shader compiler, which numbers the texture instructions.
 * Both derive it here rather than each sorting its own copy.
 *
 * Returns the number of entries written, capped at `max`.
 */
unsigned ta5_pipeline_layout_tex_bindings(
   const struct ta5_pipeline_layout *layout, VkShaderStageFlags stage,
   struct ta5_tex_binding *out, unsigned max);

/**
 * Collect the uniform and storage buffer bindings a stage can see, in the
 * order they become UBO pointer slots: ascending (set, binding), then array
 * element.
 *
 * Same contract as ta5_pipeline_layout_tex_bindings() and the same reason for
 * it: the shader compiler numbers its buffer loads from this list and the
 * draw-time emitter fills the slots from it, so the two halves have to agree.
 * The list is also what decides a shader's UBO block count, so both have to
 * take it from the pipeline layout rather than sorting their own copies.
 *
 * Dynamic buffers occupy the same slots as ordinary buffers.  Their offsets
 * are applied to the pointers emitted from command-buffer binding state.
 *
 * Returns the number of entries written, capped at `max`.
 */
unsigned ta5_pipeline_layout_buffer_bindings(
   const struct ta5_pipeline_layout *layout, VkShaderStageFlags stage,
   struct ta5_buffer_binding *out, unsigned max);

/** Highest UBO pointer slot count a stage can be given. */
#define TA5_MAX_UBOS 8

#endif /* TA5_DESCRIPTOR_H */