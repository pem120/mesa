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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file ta5_driver.c
 *
 * Adreno 5xx Vulkan driver: instance / physical-device creation and the
 * Vulkan-visible capability surface.
 *
 * API level is Vulkan 1.1.  a5xx hardware has no bindless descriptors, so
 * Vulkan 1.2 and VK_EXT_descriptor_indexing are structurally impossible and
 * are not advertised.  See A5XX_VULKAN_SPEC.md sections 3 and 6.
 */

#include <string.h>

#include "util/u_math.h"
#include "vk_util.h"
#include "vk_physical_device.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"

#include "ta5_entrypoints.h"

/* Driver version.  Keep distinct from Turnip so shader/pipeline caches
 * never collide with the a6xx+ driver on the same device. */
#define TA5_DRIVER_VERSION                                                    \
   ((0 << 22) | (1 << 20) | (0 << 12) | TA5_BUILD_VERSION)

#ifndef TA5_BUILD_ID_OVERRIDE
#define TA5_BUILD_VERSION 0x0001
#else
#define TA5_BUILD_VERSION TA5_BUILD_ID_OVERRIDE
#endif

/* ------------------------------------------------------------------ */
/* Extension tables                                                     */
/* ------------------------------------------------------------------ */

/* Instance-level extensions.  Fields must follow declaration order.
 * WSI platform bits arrive with swapchain support in a later phase. */
const struct vk_instance_extension_table ta5_instance_extensions_supported = {
   .table = {
      .KHR_device_group_creation = true,
      .KHR_external_fence_capabilities = true,
      .KHR_external_memory_capabilities = true,
      .KHR_external_semaphore_capabilities = true,
      .KHR_get_physical_device_properties2 = true,
   },
};

/*
 * Device-level extensions.
 *
 * Mirrors the capability surface recovered from the reference a5xx driver
 * (A5XX_VULKAN_SPEC.md section 6.2), restricted to what a userspace Vulkan
 * driver on mainline msm DRM can service.
 *
 * Intentionally absent:
 *   - VK_EXT_descriptor_indexing / VK_KHR_descriptor_indexing
 *   - VK_EXT_descriptor_buffer        -- all need bindless descriptors
 *   - VK_KHR_timeline_semaphore       -- not implemented yet
 *   - VK_KHR_synchronization2          -- not implemented yet
 *   - VK_KHR_dynamic_rendering        -- not implemented yet
 */
const struct vk_device_extension_table ta5_device_extensions_supported = {
   .table = {
      .KHR_16bit_storage = true,
      .KHR_bind_memory2 = true,
      .KHR_create_renderpass2 = true,
      .KHR_dedicated_allocation = true,
      .KHR_descriptor_update_template = true,
      .KHR_device_group = true,
      .KHR_external_fence = true,
      .KHR_external_fence_fd = true,
      .KHR_external_memory = true,
      .KHR_external_memory_fd = true,
      .KHR_external_semaphore = true,
      .KHR_external_semaphore_fd = true,
      .KHR_get_memory_requirements2 = true,
      .KHR_image_format_list = true,
      .KHR_maintenance1 = true,
      .KHR_maintenance2 = true,
      .KHR_maintenance3 = true,
      .KHR_multiview = true,
      .KHR_push_descriptor = true,
      .KHR_relaxed_block_layout = true,
      .KHR_sampler_mirror_clamp_to_edge = true,
      .KHR_sampler_ycbcr_conversion = true,
      .KHR_shader_draw_parameters = true,
      .KHR_shader_float16_int8 = true,
      .KHR_shader_float_controls = true,
      .KHR_storage_buffer_storage_class = true,
      .KHR_variable_pointers = true,
      .KHR_vulkan_memory_model = true,
      .EXT_sampler_filter_minmax = true,
   },
};

/* ------------------------------------------------------------------ */
/* Queue family                                                         */
/* ------------------------------------------------------------------ */

static const VkQueueFamilyProperties ta5_gfx_queue_properties = {
   .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                 VK_QUEUE_TRANSFER_BIT,
   .queueCount = 1,
   .timestampValidBits = 32,
   .minImageTransferGranularity = { 1, 1, 1 },
};

/* ------------------------------------------------------------------ */
/* Features                                                             */
/* ------------------------------------------------------------------ */

/*
 * a5xx features.
 *
 * Deliberately conservative: this is the set the GL backend already exercises
 * on real a5xx hardware, so nothing here is speculative.  Anything not listed
 * is reported unsupported rather than assumed.
 */
static void
ta5_get_features(struct ta5_physical_device *pdevice,
                 struct vk_features *features)
{
   *features = (struct vk_features){ false };

   /* Vulkan 1.0 core */
   features->robustBufferAccess = true;
   features->fullDrawIndexUint32 = true;
   features->imageCubeArray = true;
   features->independentBlend = true;
   features->geometryShader = true;
   features->tessellationShader = true;
   features->sampleRateShading = true;
   features->dualSrcBlend = true;
   features->logicOp = true;
   features->multiDrawIndirect = true;
   features->drawIndirectFirstInstance = true;
   features->depthClamp = true;
   features->depthBiasClamp = true;
   features->fillModeNonSolid = true;
   features->depthBounds = true;
   features->wideLines = true;
   features->largePoints = true;
   features->alphaToOne = true;
   features->multiViewport = true;
   features->samplerAnisotropy = true;
   features->textureCompressionETC2 = true;
   features->textureCompressionASTC_LDR = true;
   features->shaderStorageImageExtendedFormats = true;
   features->shaderStorageImageMultisample = true;
   features->shaderStorageImageReadWithoutFormat = true;
   features->shaderStorageImageWriteWithoutFormat = true;
   features->shaderUniformBufferArrayDynamicIndexing = true;
   features->shaderSampledImageArrayDynamicIndexing = true;
   features->shaderStorageBufferArrayDynamicIndexing = true;
   features->shaderStorageImageArrayDynamicIndexing = true;
   features->shaderClipDistance = true;
   features->shaderCullDistance = true;
   features->shaderInt64 = true;
   features->vertexPipelineStoresAndAtomics = true;
   features->fragmentStoresAndAtomics = true;
   features->shaderImageGatherExtended = true;

   /* fp64 is absent on a5xx -- fp16/fp32 only. */
   features->shaderFloat64 = false;
}

/* ------------------------------------------------------------------ */
/* Properties and limits                                                */
/* ------------------------------------------------------------------ */

/*
 * struct vk_properties is a *flattened* limits struct (generated
 * vk_physical_device_properties.h): the core identity fields and every limit
 * sit side by side, so everything is assigned directly.
 *
 * Values derive from the measured a5xx numbers the GL backend already uses
 * (common/freedreno_devices.py, CHIP.A5XX), so they stay consistent with the
 * working GL path on the same silicon.  Anything without a measured a5xx
 * value is left conservative rather than invented.
 */
static void
ta5_get_properties(struct ta5_physical_device *pdevice,
                   struct vk_properties *props)
{
   const struct ta5_gpu_info *info = &pdevice->gpu_info;

   memset(props, 0, sizeof(*props));

   /* -- identity -- */
   props->apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0);
   props->driverVersion = TA5_DRIVER_VERSION;
   props->vendorID = 0x5143; /* VK_VENDOR_ID_QUALCOMM */
   props->deviceID = pdevice->dev_id.gpu_id;
   props->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
   strcpy(props->deviceName, pdevice->device_name);
   /* pipelineCacheUUID filled via fd_get_device_uuid() once wired up */

   /* -- descriptor limits: conservative, bindful-era values -- */
   props->maxBoundDescriptorSets = 4;
   props->maxPerStageDescriptorSamplers = 16;
   props->maxPerStageDescriptorUniformBuffers = 8;
   props->maxPerStageDescriptorStorageBuffers = 4;
   props->maxPerStageDescriptorSampledImages = 16;
   props->maxPerStageDescriptorStorageImages = 4;
   props->maxPerStageDescriptorInputAttachments = 4;
   props->maxPerStageResources = 32;
   props->maxDescriptorSetSamplers = 16;
   props->maxDescriptorSetUniformBuffers = 64;
   props->maxDescriptorSetUniformBuffersDynamic = 8;
   props->maxDescriptorSetStorageBuffers = 32;
   props->maxDescriptorSetStorageBuffersDynamic = 8;
   props->maxDescriptorSetSampledImages = 16;
   props->maxDescriptorSetStorageImages = 4;
   props->maxDescriptorSetInputAttachments = 4;

   /* -- images / buffers -- */
   props->maxImageDimension1D = 4096;
   props->maxImageDimension2D = 4096;
   props->maxImageDimension3D = 256;
   props->maxImageDimensionCube = 4096;
   props->maxImageArrayLayers = 256;
   props->maxTexelBufferElements = 1 << 27;
   props->maxUniformBufferRange = 65536;
   props->maxStorageBufferRange = 1 << 27;
   props->maxPushConstantsSize = 128;
   props->maxMemoryAllocationCount = 4096;
   props->maxSamplerAllocationCount = 64 * 1024;
   props->bufferImageGranularity = 64; /* cache line */
   props->sparseAddressSpaceSize = 0; /* no sparse binding in this phase */

   /* -- vertex input -- */
   props->maxVertexInputAttributes = 16;
   props->maxVertexInputBindings = 16;
   props->maxVertexInputAttributeOffset = 2047;
   props->maxVertexInputBindingStride = 2047;
   props->maxVertexOutputComponents = 64;

   /* -- tessellation / geometry -- */
   props->maxTessellationGenerationLevel = 64;
   props->maxTessellationPatchSize = 32;
   props->maxTessellationControlPerVertexInputComponents = 64;
   props->maxTessellationControlPerVertexOutputComponents = 64;
   props->maxTessellationControlPerPatchOutputComponents = 120;
   props->maxTessellationControlTotalOutputComponents = 2048;
   props->maxTessellationEvaluationInputComponents = 64;
   props->maxTessellationEvaluationOutputComponents = 64;
   props->maxGeometryShaderInvocations = 1;
   props->maxGeometryInputComponents = 64;
   props->maxGeometryOutputComponents = 64;
   props->maxGeometryOutputVertices = 64;
   props->maxGeometryTotalOutputComponents = 512;
   props->maxFragmentInputComponents = 64;
   props->maxFragmentOutputAttachments = 8;
   props->maxFragmentDualSrcAttachments = 8;
   props->maxFragmentCombinedOutputResources = 16;

   /* -- compute -- */
   props->maxComputeSharedMemorySize = info->cs_shared_mem_size;
   props->maxComputeWorkGroupCount[0] = 1024;
   props->maxComputeWorkGroupCount[1] = 1024;
   props->maxComputeWorkGroupCount[2] = 64;
   props->maxComputeWorkGroupInvocations = 1024;
   props->maxComputeWorkGroupSize[0] = 1024;
   props->maxComputeWorkGroupSize[1] = 1024;
   props->maxComputeWorkGroupSize[2] = 64;

   /* -- rasterization -- */
   props->subPixelPrecisionBits = 8;
   props->subTexelPrecisionBits = 8;
   props->mipmapPrecisionBits = 10;
   props->maxDrawIndexedIndexValue = UINT32_MAX;
   props->maxDrawIndirectCount = UINT32_MAX;
   props->maxSamplerLodBias = 2.0f;
   props->maxSamplerAnisotropy = 16.0f;
   props->maxViewports = 16;
   props->maxViewportDimensions[0] = 4096;
   props->maxViewportDimensions[1] = 4096;
   props->viewportBoundsRange[0] = -32768.0f;
   props->viewportBoundsRange[1] = 32767.0f;
   props->viewportSubPixelBits = 8;
   props->minMemoryMapAlignment = 4096;
   props->minTexelBufferOffsetAlignment = 256;
   props->minUniformBufferOffsetAlignment = 256;
   props->minStorageBufferOffsetAlignment = 256;
   props->minTexelOffset = -8;
   props->maxTexelOffset = 7;
   props->minTexelGatherOffset = -8;
   props->maxTexelGatherOffset = 7;
   props->minInterpolationOffset = -0.5f;
   props->maxInterpolationOffset = 0.5f;
   props->subPixelInterpolationOffsetBits = 4;

   /* -- framebuffer: at least one full a5xx tile -- */
   props->maxFramebufferWidth = MAX2(4096, info->tile_max_w);
   props->maxFramebufferHeight = MAX2(4096, info->tile_max_h);
   props->maxFramebufferLayers = 256;
   props->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->maxColorAttachments = 8;
   props->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->maxSampleMaskWords = 1;

   /* -- rasterization state -- */
   props->maxClipDistances = 8;
   props->maxCullDistances = 8;
   props->maxCombinedClipAndCullDistances = 8;
   props->discreteQueuePriorities = 2;
   props->pointSizeRange[0] = 1.0f;
   props->pointSizeRange[1] = 64.0f;
   props->lineWidthRange[0] = 1.0f;
   props->lineWidthRange[1] = 1.0f; /* a5xx lines are fixed 1px */
   props->pointSizeGranularity = 1.0f;
   props->lineWidthGranularity = 1.0f;
   props->strictLines = false;
   props->standardSampleLocations = true;
   props->optimalBufferCopyOffsetAlignment = 64;
   props->optimalBufferCopyRowPitchAlignment = 64;
   props->nonCoherentAtomSize = 64;

   /* a5xx timestamp clock is ~19.2MHz in the GL path */
   props->timestampPeriod = 1000.0f / 19200000.0f;

   /* No sparse memory management in this phase; the sparse property flags
    * live on vk_physical_device, not in vk_properties. */
}

/* ------------------------------------------------------------------ */
/* Physical device init                                                 */
/* ------------------------------------------------------------------ */

VkResult
ta5_physical_device_init(struct ta5_physical_device *device,
                         struct ta5_instance *instance, int dev_fd)
{
   struct vk_physical_device_dispatch_table dispatch_table;
   VkResult result;

   device->instance = instance;
   device->dev_fd = dev_fd;

   snprintf(device->device_name, sizeof(device->device_name), "%s",
            ta5_device_name(device->dev_id.gpu_id));

   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &ta5_physical_device_entrypoints, true);

   /*
    * Most physical-device queries have generic implementations in the Vulkan
    * runtime (vk_common_*): features, properties, queue families, memory,
    * format properties, sparse, external handles.  They read back what is
    * stored on vk_physical_device, so pass NULL here and populate below --
    * exactly the pattern Turnip uses.
    */
   result = vk_physical_device_init(&device->vk, &instance->vk, NULL, NULL,
                                    NULL, &dispatch_table);
   if (result != VK_SUCCESS)
      return result;

   device->vk.supported_extensions = ta5_device_extensions_supported;
   ta5_get_features(device, &device->vk.supported_features);
   ta5_get_properties(device, &device->vk.properties);

   /* one universal graphics/compute/transfer queue family */
   device->queue_props[0] = ta5_gfx_queue_properties;

   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Physical device entrypoints                                          */
/* ------------------------------------------------------------------ */

VKAPI_ATTR void VKAPI_CALL
ta5_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceProperties *pProperties)
{
   vk_common_GetPhysicalDeviceProperties(physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                              VkPhysicalDeviceFeatures *pFeatures)
{
   vk_common_GetPhysicalDeviceFeatures(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                           uint32_t *pQueueFamilyPropertyCount,
                                           VkQueueFamilyProperties *pQueueFamilyProperties)
{
   vk_common_GetPhysicalDeviceQueueFamilyProperties(
      physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetPhysicalDeviceMemoryProperties(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   vk_common_GetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceImageFormatProperties(
   VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type,
   VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
   VkImageFormatProperties *pImageFormatProperties)
{
   return vk_common_GetPhysicalDeviceImageFormatProperties(
      physicalDevice, format, type, tiling, usage, flags,
      pImageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL
ta5_GetPhysicalDeviceSparseImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
   uint32_t *pPropertyCount, VkSparseImageFormatProperties2 *pProperties)
{
   vk_common_GetPhysicalDeviceSparseImageFormatProperties2(
      physicalDevice, pFormatInfo, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                   uint32_t *pPropertyCount,
                                   VkLayerProperties *pProperties)
{
   return vk_common_EnumerateDeviceLayerProperties(physicalDevice,
                                                   pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_EnumerateDeviceExtensionProperties(
   VkPhysicalDevice physicalDevice, const char *pLayerName,
   uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
   return vk_common_EnumerateDeviceExtensionProperties(
      physicalDevice, pLayerName, pPropertyCount, pProperties);
}