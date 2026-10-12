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
 * \file ta5_instance.cc
 *
 * Adreno 5xx Vulkan driver: instance creation and DRM physical-device
 * enumeration.
 *
 * The Vulkan runtime scans DRM nodes and invokes the try_create_for_drm
 * callback registered here, exactly as Turnip does.  The driver itself never
 * walks /dev/dri.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <drm/msm_drm.h>
#include <xf86drm.h>

#include "util/u_debug.h"
#include "util/u_math.h"
#include "vk_util.h"
#include "vk_object.h"
#include "vk_instance.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"

#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_entrypoints.h"

#define TA5_DBG(fmt, ...)                                                  \
   do {                                                                    \
      fprintf(stderr, "ta5: " fmt "\n", ##__VA_ARGS__);                    \
   } while (0)

/*
 * Report an enumeration failure.  The Vulkan runtime treats
 * VK_ERROR_INCOMPATIBLE_DRIVER from try_create_for_drm as "skip this node
 * silently", which is how a non-msm or non-a5xx device is rejected.
 */

/* ------------------------------------------------------------------ */
/* DRM parameter queries                                                */
/* ------------------------------------------------------------------ */

static int
ta5_msm_get_param(int fd, uint32_t param, uint64_t *value)
{
   struct drm_msm_param req = {
      .pipe = MSM_PIPE_3D0,
      .param = param,
   };

   if (drmCommandWriteRead(fd, DRM_MSM_GET_PARAM, &req, sizeof(req)))
      return -1;

   *value = req.value;
   return 0;
}

/* ------------------------------------------------------------------ */
/* Physical device enumeration                                          */
/* ------------------------------------------------------------------ */

/*
 * Enumeration callback invoked by the Vulkan runtime for each DRM node.
 * Returning VK_ERROR_INCOMPATIBLE_DRIVER makes the runtime silently skip the
 * node, which is how a5xx parts are skipped when running on a6xx and vice
 * versa -- both ICDs can coexist on a machine.
 */
VkResult
ta5_physical_device_try_create(struct vk_instance *vk_instance,
                               struct _drmDevice *drm_device,
                               struct vk_physical_device **out)
{
   struct ta5_instance *instance =
      container_of(vk_instance, struct ta5_instance, vk);
   struct ta5_physical_device *device;
   drmVersionPtr version;
   uint64_t gpu_id = 0, chip_id = 0;
   const char *path;
   VkResult result;
   int fd;

   *out = NULL;

   /* msm is a platform device but we should not care about the bus type. */
   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)))
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   path = drm_device->nodes[DRM_NODE_RENDER];

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail;
   }

   version = drmGetVersion(fd);
   if (!version) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail_fd;
   }

   /* Only the msm kernel driver is supported. */
   if (strcmp(version->name, "msm") != 0) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail_version;
   }

   /*
    * Both ids are required: the shared device-info table is keyed on the
    * (gpu_id, chip_id) pair, exactly as the GL backend does in
    * freedreno_pipe.c.  Querying gpu_id alone is not sufficient.
    */
   if (ta5_msm_get_param(fd, MSM_PARAM_GPU_ID, &gpu_id) ||
       ta5_msm_get_param(fd, MSM_PARAM_CHIP_ID, &chip_id)) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto fail_version;
   }

   device = (struct ta5_physical_device *)vk_zalloc(
      &instance->vk.alloc, sizeof(*device), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_version;
   }

   device->dev_fd = fd;
   device->dev_id.gpu_id = (uint32_t)gpu_id;
   device->dev_id.chip_id = chip_id;

   /*
    * Reject anything that is not an a5xx part.  This is what lets the a5xx
    * ICD and Turnip coexist: on an a6xx GPU only Turnip accepts the device,
    * on a5xx only this driver does.
    */
   if (!ta5_get_gpu_info(device->dev_id.gpu_id, device->dev_id.chip_id,
                         &device->gpu_info)) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail_device;
   }

   result = ta5_physical_device_init(device, instance, fd);
   if (result != VK_SUCCESS)
      goto fail_device;

   TA5_DBG("found a5xx device '%s' gpu_id=%u chip_id=0x%" PRIx64 " (%s)",
          path, device->dev_id.gpu_id, device->dev_id.chip_id,
          device->device_name);

   *out = &device->vk;
   drmFreeVersion(version);
   return VK_SUCCESS;

fail_device:
   vk_free(&instance->vk.alloc, device);
fail_version:
   drmFreeVersion(version);
fail_fd:
   close(fd);
fail:
   return result;
}

static void
ta5_destroy_physical_device(struct vk_physical_device *vk_pdevice)
{
   struct ta5_physical_device *device =
      container_of(vk_pdevice, struct ta5_physical_device, vk);

   if (device->dev_fd >= 0)
      close(device->dev_fd);

   vk_physical_device_finish(&device->vk);
   vk_free(&device->instance->vk.alloc, device);
}

/*
 * Enumeration for non-DRM device nodes.  This driver is msm/DRM only, so
 * there is nothing to do here -- return INCOMPATIBLE_DRIVER and let the
 * runtime's DRM scan drive enumeration via try_create_for_drm.
 */
static VkResult
ta5_enumerate_devices(struct vk_instance *vk_instance)
{
   return VK_ERROR_INCOMPATIBLE_DRIVER;
}

/* ------------------------------------------------------------------ */
/* Instance                                                             */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
   struct ta5_instance *instance;
   struct vk_instance_dispatch_table dispatch_table;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   instance = (struct ta5_instance *)vk_zalloc(
      pAllocator, sizeof(*instance), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &ta5_instance_entrypoints, true);

   result = vk_instance_init(&instance->vk,
                             &ta5_instance_extensions_supported,
                             &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return vk_error(NULL, result);
   }

   instance->api_version = pCreateInfo->pApplicationInfo
                              ? pCreateInfo->pApplicationInfo->apiVersion
                              : VK_API_VERSION_1_1;

   /* The Vulkan runtime scans DRM nodes and calls these. */
   instance->vk.physical_devices.try_create_for_drm =
      ta5_physical_device_try_create;
   instance->vk.physical_devices.enumerate = ta5_enumerate_devices;
   instance->vk.physical_devices.destroy = ta5_destroy_physical_device;

   *pInstance = ta5_instance_to_handle(instance);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroyInstance(VkInstance _instance,
                    const VkAllocationCallbacks *pAllocator)
{
   struct ta5_instance *instance = (struct ta5_instance *)_instance;

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_EnumerateInstanceExtensionProperties(
   const char *pLayerName, uint32_t *pPropertyCount,
   VkExtensionProperties *pProperties)
{
   return vk_enumerate_instance_extension_properties(
      &ta5_instance_extensions_supported, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                     VkLayerProperties *pProperties)
{
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   /* a5xx targets Vulkan 1.1: bindful descriptors make 1.2 impossible. */
   *pApiVersion = VK_API_VERSION_1_1;
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Vulkan loader ICD interface                                          */
/* ------------------------------------------------------------------ */

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ta5_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   struct ta5_instance *instance = (struct ta5_instance *)_instance;
   return vk_instance_get_proc_addr(instance ? &instance->vk : NULL,
                                    &ta5_instance_entrypoints, pName);
}

/*
 * vk_icdGetInstanceProcAddr is the one ICD entry point the Vulkan runtime does
 * not provide: the other two (vk_icdNegotiateLoaderICDInterfaceVersion,
 * vk_icdGetPhysicalDeviceProcAddr) exist there and are deliberately not
 * redefined here.  Turnip defines this one explicitly for the same reason.
 *
 * The loader also asks for this second entry point to work around
 * LD_PRELOAD issues in some applications, hence the thin wrapper.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
__attribute__((visibility("default")))
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return ta5_GetInstanceProcAddr(instance, pName);
}