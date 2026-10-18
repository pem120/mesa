/*
 * Window system integration for the a5xx Vulkan driver.
 *
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 *
 * This file used to carry a hand-written Wayland surface, a KMS/display path
 * and a whole swapchain implementation -- about 2500 lines across ta5_wsi.cc
 * and ta5_wayland.cc.  All of it is gone, and none of it worked: the buffers
 * it produced could not be imported by the compositor.
 *
 * phoc's own log is the diagnosis:
 *
 *     [EGL] eglCreateImageKHR, EGL_BAD_ALLOC: "createImageFromDmaBufs failed"
 *     [types/wlr_compositor.c:437] Failed to upload buffer
 *     phoc terminated abnormally with signal 11/SEGV
 *
 * and the stall that follows is reproducible without vkmark in the picture:
 * vkcube --c 3 completes instantly, --c 10 hangs, --c 30 kills the compositor.
 * Three is exactly our image_count, so present worked that many times and then
 * acquire blocked waiting for frame callbacks that never came.
 *
 * The reason is a missing negotiation step.  The compositor advertises
 * (fourcc, modifier) pairs over wl_dmabuf; wsi_common picks one of them and
 * asks the driver to allocate the swapchain image with a matching modifier
 * (wsi_common_wayland.c:3910, wsi_drm_image_params in wsi_common_private.h).
 * We bound wl_dmabuf and never read what it advertised, so we exported a
 * linear system-memory buffer that matched nothing the compositor could
 * import.
 *
 * Turnip has no WSI entry points at all.  tu_wsi.cc is 67 lines that call
 * wsi_device_init(); vk_device_init() merges the common entrypoints without
 * overwriting driver-provided ones (vk_device.c:179), so the shared
 * implementations take over.  That is the shape of this file now: bring up a
 * wsi_device, and supply the handful of entry points wsi_common cannot
 * implement for us.
 */

#include "ta5_wsi.h"

#include <fcntl.h>
#include <stdio.h>
#include <xf86drm.h>

#include "drm-uapi/drm_fourcc.h"

#include "vk_util.h"
#include "wsi_common_drm.h"

#include "ta5_instance.h"
#include "ta5_memory.h"
#include "ta5_submit.h" /* struct ta5_bo, for bo->handle */

#define TA5_DBG(fmt, ...)                                                  \
   do {                                                                    \
      fprintf(stderr, "ta5: " fmt "\n", ##__VA_ARGS__);                    \
   } while (0)

#define TA5_TRACE(fmt, ...)                                                \
   do {                                                                    \
      if (getenv("TA5_TRACE_WSI"))                                         \
         fprintf(stderr, "ta5[wsi]: " fmt "\n", ##__VA_ARGS__);            \
   } while (0)

/**
 * The wsi_common proc-addr shim.
 *
 * wsi_common resolves names against the *instance* table, so this hands it the
 * unchecked lookup; anything WSI-shaped it wants is in the common tables.
 */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
ta5_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   struct ta5_physical_device *pdevice =
      (struct ta5_physical_device *)physicalDevice;

   return vk_instance_get_proc_addr_unchecked(&pdevice->instance->vk, pName);
}

/**
 * Can this device present on `fd`?
 *
 * wsi_common uses this to decide whether a swapchain is on the same GPU as its
 * compositor.  Our memory is system memory on the same device, and the display
 * fd we opened for the device is the one we pass to wsi_device_init().
 */
static bool
ta5_wsi_can_present_on_device(VkPhysicalDevice physicalDevice, int fd)
{
#ifdef HAVE_LIBDRM
   struct ta5_physical_device *pdevice =
      (struct ta5_physical_device *)physicalDevice;

   return wsi_common_drm_devices_equal(fd, pdevice->dev_fd);
#else
   return true;
#endif
}

VkResult
ta5_wsi_init(struct ta5_physical_device *pdevice)
{
   /*
    * sw_device = false: this is a real GPU, and it selects
    * WSI_WL_BUFFER_NATIVE -- the compositor-imports-our-dma-buf path.
    *
    * sw_device = true was tried here and is NOT the answer, for a reason worth
    * recording.  It moves presentation to WSI_WL_BUFFER_SHM_MEMCPY, where wsi
    * renders into a CPU image and copies it into a wl_shm buffer the compositor
    * can always import.  On this device that does keep phoc alive -- the
    * client stopped provoking "createImageFromDmaBufs failed" and the
    * compositor stopped dying.  But swapchain creation then crashed the client
    * instead:
    *
    *     vk_common_CmdCopyImageToBuffer2 -> disp->CmdCopyImageToMemoryKHR = NULL
    *     wsi_cmd_blit_image_to_buffer (wsi_common.c:3275)
    *
    * because the blit needs VK_KHR_external_memory_fd *image* import and we
    * have none: ta5_memory.cc handles no VkImportMemoryInfoKHR at all, even
    * though VK_KHR_external_memory_fd is advertised (ta5_driver.cc:123) and
    * only its export half exists.
    *
    * So the honest state of this device, measured:
    *
    *   - native path (false): the client works end to end -- vkcube --wsi
    *     wayland --c 30 exits 0 -- but phoc cannot import the buffer and
    *     segfaults, because it advertises zero dma-buf formats.
    *   - shm path (true): the compositor survives, but the client cannot
    *     create a swapchain until external-memory image import exists.
    *
    * Neither is a working desktop.  false is kept because it is the path a
    * real driver takes and the one that will work once the compositor can
    * import dma-bufs; see A5XX-WSI-STATUS.md section 10.8 for the probes that
    * established the compositor advertises no formats at all.
    */
   const struct wsi_device_options options = { .sw_device = false };
   VkResult result;

   result = wsi_device_init(&pdevice->wsi_device,
                            ta5_physical_device_to_handle(pdevice),
                            ta5_wsi_proc_addr, &pdevice->instance->vk.alloc,
                            pdevice->master_fd,
                            /* No driconf parsing in this driver.  NULL is
                             * safe: wsi_common only reads the cache when it
                             * is non-NULL (wsi_common.c:296). */
                            NULL, &options);
   if (result != VK_SUCCESS)
      return result;

   /*
    * supports_modifiers stays false, and this is the single change that makes
    * Wayland swapchain creation work at all.
    *
    * Setting it true promises VK_EXT_image_drm_format_modifier: it makes
    * wsi_common_wayland.c read the compositor's wl_dmabuf format/modifier list
    * (wsi_common_wayland.c:3910) and set num_modifier_lists = 1.  That in turn
    * makes wsi_drm_configure_image() query modifiers from the driver:
    *
    *     GetPhysicalDeviceFormatProperties2() with a
    *     VkDrmFormatModifierPropertiesListEXT, then one
    *     GetPhysicalDeviceImageFormatProperties2() per modifier.
    *
    * We implement none of that.  ta5_GetPhysicalDeviceFormatProperties2
    * (ta5_formats.cc:248) fills in only linear/optimal/buffer features and
    * leaves drmFormatModifierCount at zero, and
    * ta5_GetPhysicalDeviceImageFormatProperties2 rejects anything that is not
    * VK_IMAGE_TILING_LINEAR or _OPTIMAL outright (ta5_formats.cc:344).
    *
    * The result was measured, not guessed.  GDB at wsi_common_drm.c:611,
    * breaking on the assertion just after the query:
    *
    *     drmFormatModifierCount = 0
    *     params = { num_modifier_lists = 1, ... }
    *
    * With no modifiers in the list, no modifier survives the filter, so
    * wsi_configure_native_image() falls through to its only failure path:
    *
    *     assert(!"Failed to find a supported modifier!  This should never
    *             happen because LINEAR should always be available");
    *     goto fail_oom;
    *
    * which returns VK_ERROR_OUT_OF_HOST_MEMORY.  That error code is
    * misleading -- nothing ran out of memory, the device had 41G free -- and it
    * is what vkCreateSwapchainKHR reported, after which vkcube queried images
    * on the null swapchain handle and segfaulted.
    *
    * False sends wsi_common down the legacy path instead, which sets
    * info->wsi.scanout = true and exports the dma-buf with no explicit
    * modifier.  That is the honest answer for a driver with linear-only
    * images, and it matches the old hand-written path.
    *
    * Turning this on for real means implementing
    * vkGetImageDrmFormatModifierPropertiesEXT, honouring
    * VkImageDrmFormatModifierListCreateInfoEXT in image creation, and
    * answering tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT -- i.e.
    * advertising DRM_FORMAT_MOD_LINEAR from the query above so that phoc's
    * own modifier list has something to match.  Not done here.
    */
   pdevice->wsi_device.supports_modifiers = false;
   pdevice->wsi_device.can_present_on_device = ta5_wsi_can_present_on_device;

   pdevice->vk.wsi_device = &pdevice->wsi_device;

   return VK_SUCCESS;
}

void
ta5_wsi_finish(struct ta5_physical_device *pdevice)
{
   pdevice->vk.wsi_device = NULL;
   wsi_device_finish(&pdevice->wsi_device, &pdevice->instance->vk.alloc);
}

/* ------------------------------------------------------------------ */
/* External memory: dmabuf export                                       */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL ta5_GetMemoryFdKHR(
   VkDevice device, const VkMemoryGetFdInfoKHR *pGetFdInfo, int *pFd);

VKAPI_ATTR VkResult VKAPI_CALL ta5_GetMemoryFdPropertiesKHR(
   VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, int fd,
   VkMemoryFdPropertiesKHR *pMemoryFdProperties);

/**
 * Export a device memory allocation as a dma-buf.
 *
 * This is the entry point wsi_common needs and that we were missing entirely:
 * VK_KHR_external_memory_fd was advertised at ta5_driver.cc:117 while neither
 * this nor ta5_GetMemoryFdPropertiesKHR existed, leaving the dispatch slots
 * NULL -- any application calling them jumped to address 0.
 *
 * wsi_common calls this for every swapchain image (wsi_common_wayland.c:3625)
 * and puts the fd, the offset, the row pitch and the modifier straight into
 * zwp_linux_dmabuf_v1_create_params().
 *
 * Turnip additionally stamps the modifier onto the buffer as fb-modifier
 * metadata so the compositor can read it back without being told (tu_device.cc:
 * 4460-4478).  Every image here is linear, so the modifier is always
 * DRM_FORMAT_MOD_LINEAR and saying so once, at export, keeps the compositor's
 * import and ours describing the same thing.
 */
VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetMemoryFdKHR(VkDevice _device, const VkMemoryGetFdInfoKHR *pGetFdInfo,
                   int *pFd)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_memory *memory;
   int prime_fd;

   if (!pGetFdInfo || !pGetFdInfo->memory || !pFd)
      return vk_error(&device->instance->vk,
                      VK_ERROR_INITIALIZATION_FAILED);

   if (pGetFdInfo->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
       pGetFdInfo->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
      return vk_error(&device->instance->vk, VK_ERROR_FEATURE_NOT_PRESENT);

   memory = ta5_memory_from_handle(pGetFdInfo->memory);

   if (!memory || !memory->bo)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   if (drmPrimeHandleToFD(device->fd, memory->bo->handle, DRM_CLOEXEC,
                          &prime_fd) ||
       prime_fd < 0) {
      TA5_DBG("GetMemoryFdKHR: prime export failed for bo %u",
              memory->bo->handle);
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   TA5_TRACE("GetMemoryFdKHR: handle %u -> fd %d (linear, modifier 0x%llx)",
             memory->bo->handle, prime_fd,
             (unsigned long long)DRM_FORMAT_MOD_LINEAR);

   *pFd = prime_fd;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetMemoryFdPropertiesKHR(VkDevice _device,
                             VkExternalMemoryHandleTypeFlagBits handleType,
                             int fd, VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   struct ta5_device *device = (struct ta5_device *)_device;

   if (!pMemoryFdProperties)
      return vk_error(&device->instance->vk,
                      VK_ERROR_INITIALIZATION_FAILED);

   if (handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
      return vk_error(&device->instance->vk, VK_ERROR_FEATURE_NOT_PRESENT);

   /* One heap and it is host-visible, so type 0 is the only one and it fits. */
   pMemoryFdProperties->memoryTypeBits = 1;

   return VK_SUCCESS;
}