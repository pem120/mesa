/*
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 */

/**
 * WSI for real KMS scanout.
 *
 * vkcube --wsi display is the target.  The instance gets VK_KHR_surface +
 * VK_KHR_display, the device gets VK_KHR_swapchain, and present goes through
 * DRM KMS on card0-DSI-1 with drmModeSetCrtc.  No X11, no Wayland, no stub.
 *
 * Swapchain images are ordinary ta5 colour-attachment images (linear,
 * host-visible, R8G8B8A8_UNORM).  On present the image's MSM GEM handle is
 * shared to the KMS fd via PRIME, wrapped in a DRM framebuffer with
 * drmModeAddFB2, and set as the CRTC's scanout.  PRIME is the bridge because
 * GEM handles are per-fd: the render node that drew the image is not the
 * card fd that displays it.
 */

#include "ta5_wsi.h"
#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_memory.h"
#include "ta5_submit.h"
#include "ta5_sync.h"
#include "ta5_entrypoints.h"

#include "vk_alloc.h"
#include "vk_object.h"
#include "vk_device.h"
#include "vk_physical_device.h"
#include "vk_queue.h"
#include "vk_sync.h"

#include "ta5_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

/* ------------------------------------------------------------------ */
/* DRM helpers                                                         */
/* ------------------------------------------------------------------ */

/** Open the KMS card.  The render node cannot modeset. */
static int
ta5_drm_open(void)
{
   int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
   if (fd < 0)
      fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
   return fd;
}

/**
 * Find the display to present on: the first connected connector with a mode,
 * its encoder's CRTC, and that CRTC.
 *
 * On msm8953 this is card0-DSI-1 at 1080x2280.  Nothing is hardcoded to that
 * name: the scan takes whatever is connected, so a board with HDMI instead
 * still finds its panel.  What IS assumed is that one display is enough --
 * vkcube asks for one surface, so the first connected connector wins.
 */
static VkResult
ta5_drm_find_display(int fd, uint32_t *conn_out, uint32_t *crtc_out,
                     drmModeModeInfo *mode_out, char *name_out,
                     size_t name_len)
{
   drmModeRes *res = drmModeGetResources(fd);
   drmModeConnector *conn = NULL;
   uint32_t conn_id = 0, crtc_id = 0;
   drmModeModeInfo mode = {};
   bool found = false;

   if (!res)
      return VK_ERROR_INITIALIZATION_FAILED;

   for (int i = 0; i < res->count_connectors && !found; i++) {
      drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
      if (!c)
         continue;
      if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
         /* Prefer the connector's preferred mode, else its largest. */
         int best = 0;
         for (int m = 0; m < c->count_modes; m++) {
            if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
               best = m;
               break;
            }
            if (c->modes[m].hdisplay * c->modes[m].vdisplay >
                c->modes[best].hdisplay * c->modes[best].vdisplay)
               best = m;
         }
         mode = c->modes[best];
         conn_id = c->connector_id;

         /* The encoder's CRTC, else the first CRTC that can drive it. */
         if (c->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(fd, c->encoder_id);
            if (enc) {
               crtc_id = enc->crtc_id;
               drmModeFreeEncoder(enc);
            }
         }
         if (!crtc_id && c->count_encoders > 0) {
            /* Fall back: first CRTC in the encoder's possible_crtcs. */
            drmModeEncoder *enc =
               drmModeGetEncoder(fd, c->encoders[0]);
            if (enc) {
               for (int j = 0; j < res->count_crtcs; j++) {
                  if (enc->possible_crtcs & (1 << j)) {
                     crtc_id = res->crtcs[j];
                     break;
                  }
               }
               drmModeFreeEncoder(enc);
            }
         }
         if (!crtc_id && res->count_crtcs > 0)
            crtc_id = res->crtcs[0];

         if (crtc_id) {
            snprintf(name_out, name_len, "%s-%u",
                     drmModeGetConnectorTypeName(c->connector_type),
                     c->connector_type_id);
            found = true;
         }
      }
      drmModeFreeConnector(c);
   }

   drmModeFreeResources(res);

   if (!found)
      return VK_ERROR_INITIALIZATION_FAILED;

   *conn_out = conn_id;
   *crtc_out = crtc_id;
   *mode_out = mode;
   (void)conn;
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Display enumeration                                                 */
/* ------------------------------------------------------------------ */

/**
 * Build (once) the display and mode objects this physical device hands out.
 *
 * Both are cached on the physical device: an application receives a
 * VkDisplayKHR from vkGetPhysicalDeviceDisplayPropertiesKHR and the same
 * panel's VkDisplayKHR back from vkGetDisplayPlaneSupportedDisplaysKHR, then
 * matches the two by comparing handles.  Allocating per query would give two
 * different pointers for one panel, the match would fail, and vkcube would
 * stop with "Cannot find a plane compatible with the display!" before it ever
 * creates a surface.
 *
 * The mode object is cached for the same reason, one level up: it is passed
 * back into vkGetDisplayPlaneCapabilitiesKHR and
 * vkCreateDisplayPlaneSurfaceKHR, and it is what ties those calls to the
 * panel they were enumerated from.
 */
/**
 * Build the display and mode objects for one physical device.
 *
 * Split out of ta5_wsi_display_init so the lock wrapper has no error paths of
 * its own to jump over.
 */
static VkResult
ta5_wsi_display_create(struct ta5_physical_device *pdevice)
{
   struct ta5_display *disp;
   struct ta5_display_mode *dmode;
   drmModeModeInfo mode;
   char name[64] = {};
   uint32_t conn_id, crtc_id;
   VkResult result;
   int fd;

   fd = ta5_drm_open();
   if (fd < 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   result = ta5_drm_find_display(fd, &conn_id, &crtc_id, &mode, name,
                                 sizeof(name));
   close(fd);
   if (result != VK_SUCCESS)
      return result;

   disp = (struct ta5_display *)vk_zalloc(&pdevice->instance->vk.alloc,
                                          sizeof(*disp), 8,
                                          VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!disp)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   vk_object_base_instance_init(&pdevice->instance->vk, &disp->base,
                                VK_OBJECT_TYPE_DISPLAY_KHR);
   disp->physical_device = pdevice;
   disp->connector_id = conn_id;
   disp->crtc_id = crtc_id;
   disp->width = mode.hdisplay;
   disp->height = mode.vdisplay;
   disp->drm_mode = mode;
   snprintf(disp->name, sizeof(disp->name), "%s", name);
   if (mode.vrefresh) {
      disp->refresh_numer = mode.vrefresh * 1000;
      disp->refresh_denom = 1000;
   } else {
      disp->refresh_numer = 60;
      disp->refresh_denom = 1;
   }

   dmode = (struct ta5_display_mode *)vk_zalloc(&pdevice->instance->vk.alloc,
                                                sizeof(*dmode), 8,
                                                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!dmode) {
      vk_object_base_finish(&disp->base);
      vk_free(&pdevice->instance->vk.alloc, disp);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   vk_object_base_instance_init(&pdevice->instance->vk, &dmode->base,
                                VK_OBJECT_TYPE_DISPLAY_MODE_KHR);
   dmode->display = disp;
   dmode->params.visibleRegion.width = disp->width;
   dmode->params.visibleRegion.height = disp->height;
   dmode->params.refreshRate = disp->refresh_numer / disp->refresh_denom;

   pdevice->wsi_display = disp;
   pdevice->wsi_mode = dmode;
   return VK_SUCCESS;
}

VkResult
ta5_wsi_display_init(struct ta5_physical_device *pdevice)
{
   simple_mtx_lock(&pdevice->wsi_mtx);

   if (!pdevice->wsi_display) {
      VkResult result = ta5_wsi_display_create(pdevice);

      if (result != VK_SUCCESS) {
         simple_mtx_unlock(&pdevice->wsi_mtx);
         return result;
      }
   }

   simple_mtx_unlock(&pdevice->wsi_mtx);
   return VK_SUCCESS;
}

void
ta5_wsi_display_finish(struct ta5_physical_device *pdevice)
{
   /* Only called from the physical-device destructor, so the lock is not
    * needed -- nothing else can reach these objects at that point. */
   if (pdevice->wsi_mode) {
      vk_object_base_finish(&pdevice->wsi_mode->base);
      vk_free(&pdevice->instance->vk.alloc, pdevice->wsi_mode);
      pdevice->wsi_mode = NULL;
   }
   if (pdevice->wsi_display) {
      vk_object_base_finish(&pdevice->wsi_display->base);
      vk_free(&pdevice->instance->vk.alloc, pdevice->wsi_display);
      pdevice->wsi_display = NULL;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice physicalDevice,
                                          uint32_t *pPropertyCount,
                                          VkDisplayPropertiesKHR *pProperties)
{
   struct ta5_physical_device *pdevice =
      (struct ta5_physical_device *)physicalDevice;
   struct ta5_display *disp;
   VkResult result;

   if (!pPropertyCount)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!pProperties) {
      *pPropertyCount = 1;
      return VK_SUCCESS;
   }

   if (*pPropertyCount < 1) {
      *pPropertyCount = 1;
      return VK_INCOMPLETE;
   }

   result = ta5_wsi_display_init(pdevice);
   if (result != VK_SUCCESS)
      return result;

   disp = pdevice->wsi_display;

   pProperties[0].display = ta5_display_to_handle(disp);
   /*
    * The name is handed over as a pointer to our own string rather than
    * copied into the caller's struct.  That is what every other Mesa
    * implementation does (wsi_common_display.c: properties->displayName =
    * metadata->display_name), and it is the only safe choice: displayName is a
    * const char * the application has not initialised, so writing through it
    * lands on whatever the malloc returned.  vkcube's array arrives with
    * garbage in that field and copies it straight into printf.
    */
   pProperties[0].displayName = disp->name;
   pProperties[0].physicalDimensions.width = 0;
   pProperties[0].physicalDimensions.height = 0;
   pProperties[0].physicalResolution.width = disp->width;
   pProperties[0].physicalResolution.height = disp->height;
   pProperties[0].supportedTransforms =
      VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   pProperties[0].planeReorderPossible = VK_FALSE;
   pProperties[0].persistentContent = VK_FALSE;
   *pPropertyCount = 1;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetDisplayModePropertiesKHR(VkPhysicalDevice physicalDevice,
                                VkDisplayKHR display,
                                uint32_t *pPropertyCount,
                                VkDisplayModePropertiesKHR *pProperties)
{
   struct ta5_physical_device *pdevice =
      (struct ta5_physical_device *)physicalDevice;
   struct ta5_display *disp = ta5_display_from_handle(display);
   struct ta5_display_mode *m;
   VkResult result;

   if (!pPropertyCount)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!pProperties) {
      *pPropertyCount = 1;
      return VK_SUCCESS;
   }

   if (*pPropertyCount < 1) {
      *pPropertyCount = 1;
      return VK_INCOMPLETE;
   }

   /* A display handle from another physical device, or a NULL one, is not a
    * display this device can show. */
   if (!disp || disp != pdevice->wsi_display)
      return VK_ERROR_INITIALIZATION_FAILED;

   result = ta5_wsi_display_init(pdevice);
   if (result != VK_SUCCESS)
      return result;

   m = pdevice->wsi_mode;
   pProperties[0].displayMode = ta5_display_mode_to_handle(m);
   pProperties[0].parameters = m->params;
   *pPropertyCount = 1;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceDisplayPlanePropertiesKHR(
   VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount,
   VkDisplayPlanePropertiesKHR *pProperties)
{
   if (!pPropertyCount)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!pProperties) {
      *pPropertyCount = 1;
      return VK_SUCCESS;
   }

   if (*pPropertyCount < 1) {
      *pPropertyCount = 1;
      return VK_INCOMPLETE;
   }

   /* One plane: the primary.  vkcube asks for plane 0, and one plane is all
    * a single-surface application can use. */
   pProperties[0].currentDisplay = VK_NULL_HANDLE;
   pProperties[0].currentStackIndex = 0;
   *pPropertyCount = 1;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice physicalDevice,
                                        uint32_t planeIndex,
                                        uint32_t *pDisplayCount,
                                        VkDisplayKHR *pDisplays)
{
   struct ta5_physical_device *pdevice =
      (struct ta5_physical_device *)physicalDevice;
   VkResult result;

   if (planeIndex != 0)
      return VK_ERROR_INITIALIZATION_FAILED;
   if (!pDisplayCount)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!pDisplays) {
      *pDisplayCount = 1;
      return VK_SUCCESS;
   }

   if (*pDisplayCount < 1) {
      *pDisplayCount = 1;
      return VK_INCOMPLETE;
   }

   result = ta5_wsi_display_init(pdevice);
   if (result != VK_SUCCESS)
      return result;

   /* The same handle the display query returned: see ta5_wsi_display_init. */
   pDisplays[0] = ta5_display_to_handle(pdevice->wsi_display);
   *pDisplayCount = 1;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                   VkDisplayModeKHR mode,
                                   uint32_t planeIndex,
                                   VkDisplayPlaneCapabilitiesKHR *pCapabilities)
{
   struct ta5_physical_device *pdevice =
      (struct ta5_physical_device *)physicalDevice;
   struct ta5_display_mode *m = ta5_display_mode_from_handle(mode);
   struct ta5_display *disp;

   if (planeIndex != 0 || !pCapabilities)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!m || m != pdevice->wsi_mode)
      return VK_ERROR_INITIALIZATION_FAILED;

   disp = m->display;

   pCapabilities->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
   pCapabilities->minSrcPosition = (VkOffset2D){ 0, 0 };
   pCapabilities->maxSrcPosition = (VkOffset2D){ 0, 0 };
   pCapabilities->minSrcExtent = (VkExtent2D){ disp->width, disp->height };
   pCapabilities->maxSrcExtent = (VkExtent2D){ disp->width, disp->height };
   pCapabilities->minDstPosition = (VkOffset2D){ 0, 0 };
   pCapabilities->maxDstPosition = (VkOffset2D){ 0, 0 };
   pCapabilities->minDstExtent = (VkExtent2D){ disp->width, disp->height };
   pCapabilities->maxDstExtent = (VkExtent2D){ disp->width, disp->height };
   (void)physicalDevice;
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateDisplayPlaneSurfaceKHR(
   VkInstance _instance, const VkDisplaySurfaceCreateInfoKHR *pCreateInfo,
   const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
   struct ta5_instance *instance = (struct ta5_instance *)_instance;
   struct ta5_surface *surf;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   if (!pCreateInfo)
      return vk_error(&instance->vk, VK_ERROR_INITIALIZATION_FAILED);

   surf = (struct ta5_surface *)vk_zalloc(
      pAllocator, sizeof(*surf), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!surf)
      return vk_error(&instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_instance_init(&instance->vk, &surf->base,
                                VK_OBJECT_TYPE_SURFACE_KHR);
   surf->instance = instance;
   /* pCreateInfo->displayMode is the mode, and the display comes from it --
    * VkDisplaySurfaceCreateInfoKHR names only the mode. */
   if (pCreateInfo->displayMode) {
      struct ta5_display_mode *m =
         ta5_display_mode_from_handle(pCreateInfo->displayMode);
      surf->mode = m;
      surf->display = m->display;
   }
   surf->plane_index = pCreateInfo->planeIndex;

   *pSurface = ta5_surface_to_handle(surf);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroySurfaceKHR(VkInstance _instance, VkSurfaceKHR _surface,
                      const VkAllocationCallbacks *pAllocator)
{
   struct ta5_instance *instance = (struct ta5_instance *)_instance;
   struct ta5_surface *surf = ta5_surface_from_handle(_surface);

   if (!_surface)
      return;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

#ifdef TA5_HAVE_WAYLAND
   if (surf->wayland) {
      ta5_DestroyWaylandSurface(surf, pAllocator);
      return;
   }
#endif

   vk_object_base_finish(&surf->base);
   vk_free(pAllocator, surf);
   (void)instance;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                       uint32_t queueFamilyIndex,
                                       VkSurfaceKHR surface,
                                       VkBool32 *pSupported)
{
   if (!pSupported)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* One queue family, and it presents everywhere: there is one display and
    * one plane, so every queue presents to every surface. */
   (void)physicalDevice;
   (void)queueFamilyIndex;
   (void)surface;
   *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                            VkSurfaceKHR surface,
                                            VkSurfaceCapabilitiesKHR *pCaps)
{
   struct ta5_surface *surf = ta5_surface_from_handle(surface);

#ifdef TA5_HAVE_WAYLAND
   if (surf && surf->wayland)
      return ta5_wayland_capabilities(surf, pCaps);
#endif

   if (!pCaps || !surf || !surf->display)
      return VK_ERROR_INITIALIZATION_FAILED;

   pCaps->minImageCount = 2;
   pCaps->maxImageCount = 3;
   pCaps->currentExtent.width = surf->display->width;
   pCaps->currentExtent.height = surf->display->height;
   pCaps->minImageExtent = pCaps->currentExtent;
   pCaps->maxImageExtent = pCaps->currentExtent;
   pCaps->maxImageArrayLayers = 1;
   pCaps->supportedTransforms =
      VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   pCaps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   pCaps->supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   pCaps->supportedUsageFlags =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   (void)physicalDevice;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
                                       VkSurfaceKHR surface,
                                       uint32_t *pCount,
                                       VkSurfaceFormatKHR *pFormats)
{
   if (!pCount)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* The swapchain image is an R8G8B8A8_UNORM colour attachment -- the one
    * format this driver renders losslessly -- so that is the only format
    * offered.  vkcube accepts the first format when it has no preference,
    * and its preference list includes UNORM. */
   if (!pFormats) {
      *pCount = 1;
      return VK_SUCCESS;
   }

   if (*pCount < 1) {
      *pCount = 1;
      return VK_INCOMPLETE;
   }

   pFormats[0].format = VK_FORMAT_R8G8B8A8_UNORM;
   pFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   (void)physicalDevice;
   (void)surface;
   *pCount = 1;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
                                            VkSurfaceKHR surface,
                                            uint32_t *pCount,
                                            VkPresentModeKHR *pModes)
{
   if (!pCount)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* FIFO only: drmModeSetCrtc is synchronous, so every present waits for
    * the scanout.  MAILBOX and IMMEDIATE would need a flip queue that does
    * not exist yet. */
   if (!pModes) {
      *pCount = 1;
      return VK_SUCCESS;
   }

   if (*pCount < 1) {
      *pCount = 1;
      return VK_INCOMPLETE;
   }

   pModes[0] = VK_PRESENT_MODE_FIFO_KHR;
   (void)physicalDevice;
   (void)surface;
   *pCount = 1;
   return VK_SUCCESS;
}

/*
 * VK_KHR_get_surface_capabilities2: the same three questions as the
 * 1-variants, asked through structures that carry a pNext chain.  The
 * answers are the ones above, so both entry points are wrappers.
 *
 * They have to exist, and being absent was not a tolerable gap.  The
 * runtime resolves physical-device names through trampolines that are
 * handed out whenever the extension is enabled, implemented or not: the
 * loader took vk_tramp_GetPhysicalDeviceSurfaceFormats2KHR, called it, and
 * it jumped through the NULL slot where the missing implementation would
 * have been.  vulkaninfo died jumping to address 0 right after creating
 * its device; vkgears died the same way.  vkcube never noticed because it
 * asks the 1-variants.
 */
VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceSurfaceCapabilities2KHR(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
   VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   if (!pSurfaceInfo || !pSurfaceInfo->surface || !pSurfaceCapabilities)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkResult result = ta5_GetPhysicalDeviceSurfaceCapabilitiesKHR(
      physicalDevice, pSurfaceInfo->surface,
      &pSurfaceCapabilities->surfaceCapabilities);

   /* A chained-in structure is a query the caller wants answered.  The
    * only one this driver recognises is protected memory, and a5xx has
    * none: answer it explicitly rather than leaving whatever the caller
    * initialised it to. */
   for (VkBaseOutStructure *out =
           (VkBaseOutStructure *)pSurfaceCapabilities->pNext;
        out != NULL; out = out->pNext) {
      if (out->sType == VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR) {
         ((VkSurfaceProtectedCapabilitiesKHR *)out)->supportsProtected =
            VK_FALSE;
      }
   }

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDeviceSurfaceFormats2KHR(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
   uint32_t *pSurfaceFormatCount, VkSurfaceFormat2KHR *pSurfaceFormats)
{
   if (!pSurfaceFormatCount)
      return VK_ERROR_INITIALIZATION_FAILED;

   /* The same single format as the 1-variant; see the comment there. */
   if (!pSurfaceFormats) {
      *pSurfaceFormatCount = 1;
      return VK_SUCCESS;
   }

   if (*pSurfaceFormatCount < 1) {
      *pSurfaceFormatCount = 1;
      return VK_INCOMPLETE;
   }

   pSurfaceFormats[0].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
   pSurfaceFormats[0].surfaceFormat.format = VK_FORMAT_R8G8B8A8_UNORM;
   pSurfaceFormats[0].surfaceFormat.colorSpace =
      VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   (void)physicalDevice;
   (void)pSurfaceInfo;
   *pSurfaceFormatCount = 1;
   return VK_SUCCESS;
}

/*
 * VK_KHR_device_group's present queries.  The group is one device, so every
 * answer is the single-device answer: device 0 is present everywhere, the
 * only present mode is LOCAL, and the only present rectangle is the surface
 * itself.
 *
 * They have to exist for the same reason as the get_surface_capabilities2
 * wrappers above.  vkGetDeviceGroupPresentCapabilitiesKHR belongs to an
 * extension the driver advertises, and it has no core-promoted alias to hide
 * behind: the loader asked for it, got the trampoline, and jumped through
 * the NULL slot -- vulkaninfo's full dump died there right after creating
 * the device it uses to check presentation support.
 */
VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetDeviceGroupPresentCapabilitiesKHR(
   VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *pCapabilities)
{
   if (!pCapabilities)
      return VK_ERROR_INITIALIZATION_FAILED;

   pCapabilities->sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_CAPABILITIES_KHR;
   pCapabilities->pNext = NULL;
   for (uint32_t i = 0; i < VK_MAX_DEVICE_GROUP_SIZE; i++)
      pCapabilities->presentMask[i] = i == 0 ? 1u : 0u;
   pCapabilities->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
   (void)device;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetDeviceGroupSurfacePresentModesKHR(
   VkDevice device, VkSurfaceKHR surface,
   VkDeviceGroupPresentModeFlagsKHR *pModes)
{
   if (!pModes)
      return VK_ERROR_INITIALIZATION_FAILED;

   *pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
   (void)device;
   (void)surface;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetPhysicalDevicePresentRectanglesKHR(
   VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
   uint32_t *pRectCount, VkRect2D *pRects)
{
   VkSurfaceCapabilitiesKHR caps;
   VkResult result;

   if (!pRectCount)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!pRects) {
      *pRectCount = 1;
      return VK_SUCCESS;
   }

   if (*pRectCount < 1) {
      *pRectCount = 1;
      return VK_INCOMPLETE;
   }

   result = ta5_GetPhysicalDeviceSurfaceCapabilitiesKHR(
      physicalDevice, surface, &caps);
   if (result != VK_SUCCESS)
      return result;

   pRects[0].offset.x = 0;
   pRects[0].offset.y = 0;
   pRects[0].extent = caps.currentExtent;
   *pRectCount = 1;
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Swapchains                                                          */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateSwapchainKHR(VkDevice _device,
                       const VkSwapchainCreateInfoKHR *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkSwapchainKHR *pSwapchain)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_surface *surf;
   struct ta5_swapchain *sc;
   uint32_t count;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   if (!pCreateInfo || !pCreateInfo->surface)
      return vk_error(&device->instance->vk, VK_ERROR_INITIALIZATION_FAILED);

   surf = ta5_surface_from_handle(pCreateInfo->surface);
   if (!surf)
      return vk_error(&device->instance->vk, VK_ERROR_INITIALIZATION_FAILED);

#ifdef TA5_HAVE_WAYLAND
   /*
    * A Wayland surface takes its extent from the compositor, so there is no
    * mode here to compare it against: the application asked for a size and the
    * size is what it gets.  The KMS branch still insists on an exact match
    * with the mode, because there the mode *is* the image and any other size
    * is a request to scale, which this path does not do.
    */
   if (surf->wayland) {
      if (pCreateInfo->imageExtent.width == 0 ||
          pCreateInfo->imageExtent.height == 0 ||
          pCreateInfo->imageExtent.width > 4096 ||
          pCreateInfo->imageExtent.height > 4096)
         return vk_error(&device->instance->vk, VK_ERROR_INITIALIZATION_FAILED);
   } else
#endif
   {
      if (!surf->display)
         return vk_error(&device->instance->vk, VK_ERROR_INITIALIZATION_FAILED);

      /* vkcube creates with the currentExtent; anything else is a request to
       * scale, which KMS would have to do and this path does not. */
      if (pCreateInfo->imageExtent.width != surf->display->width ||
          pCreateInfo->imageExtent.height != surf->display->height)
         return vk_error(&device->instance->vk,
                         VK_ERROR_INITIALIZATION_FAILED);
   }

   if (pCreateInfo->imageFormat != VK_FORMAT_R8G8B8A8_UNORM &&
       pCreateInfo->imageFormat != VK_FORMAT_B8G8R8A8_UNORM)
      return vk_error(&device->instance->vk, VK_ERROR_FORMAT_NOT_SUPPORTED);

   count = pCreateInfo->minImageCount;
   if (count < 2)
      count = 2;
   if (count > 3)
      count = 3;

   sc = (struct ta5_swapchain *)vk_zalloc(
      pAllocator, sizeof(*sc), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!sc)
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &sc->base,
                       VK_OBJECT_TYPE_SWAPCHAIN_KHR);
   sc->device = device;
   sc->surface = surf;
   sc->format = pCreateInfo->imageFormat;
   sc->color_space = pCreateInfo->imageColorSpace;
   sc->extent = pCreateInfo->imageExtent;
   sc->present_mode = pCreateInfo->presentMode;
   sc->image_count = count;
   sc->next = 0;
   sc->drm_fd = -1;
   /* A Wayland surface never opens the card: its buffers go to the compositor
    * as dma-bufs and are composited by phoc, which owns the display. */
   sc->crtc_id = surf->display ? surf->display->crtc_id : 0;
   sc->connector_id = surf->display ? surf->display->connector_id : 0;

   sc->images = (struct ta5_swapchain_image *)vk_zalloc(
      pAllocator, count * sizeof(*sc->images), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!sc->images) {
      vk_free(pAllocator, sc);
      return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /*
    * Per-image setup, written without a goto because the label sits outside the
    * loop's scope and C forbids jumping into it past an initialisation.  A
    * single exit that frees images [0, n) is what the failure path needs, and
    * a loop body with one `break` says that more plainly than three gotos.
    */
   uint32_t i;
   for (i = 0; i < count; i++) {
      VkImageCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = sc->format,
         .extent = { sc->extent.width, sc->extent.height, 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         /* ta5 has one heap and one memory type; index 0 is always right. */
         .memoryTypeIndex = 0,
      };
      VkMemoryRequirements mr;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      VkImage img = VK_NULL_HANDLE;
      VkResult ret;

      ret = ta5_CreateImage(_device, &ici, pAllocator, &img);
      if (ret != VK_SUCCESS)
         break;

      ta5_GetImageMemoryRequirements(_device, img, &mr);
      mai.allocationSize = mr.size;

      ret = ta5_AllocateMemory(_device, &mai, pAllocator, &mem);
      if (ret != VK_SUCCESS) {
         ta5_DestroyImage(_device, img, pAllocator);
         break;
      }

      ret = ta5_BindImageMemory(_device, img, mem, 0);
      if (ret != VK_SUCCESS) {
         ta5_FreeMemory(_device, mem, pAllocator);
         ta5_DestroyImage(_device, img, pAllocator);
         break;
      }

      sc->images[i].image = ta5_image_from_handle(img);
      sc->images[i].memory = ta5_memory_from_handle(mem);
      sc->images[i].fb_id = 0;
#ifdef TA5_HAVE_WAYLAND
      sc->images[i].wl_buffer = NULL;
      sc->images[i].wl_shm_pool = NULL;
      sc->images[i].wl_shm_fd = -1;
#endif
   }

   if (i < count) {
      /* Free what was made, including the image that failed partway.  The
       * loop above already destroyed a partially-built one, so only fully
       * built entries appear here. */
      for (uint32_t j = 0; j < i; j++) {
         ta5_FreeMemory(_device, ta5_memory_to_handle(sc->images[j].memory),
                        pAllocator);
         ta5_DestroyImage(_device, ta5_image_to_handle(sc->images[j].image),
                          pAllocator);
      }
      vk_free(pAllocator, sc->images);
      vk_free(pAllocator, sc);
      return vk_error(&device->instance->vk, i == count
                                        ? VK_SUCCESS
                                        : VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   *pSwapchain = ta5_swapchain_to_handle(sc);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ta5_DestroySwapchainKHR(VkDevice _device, VkSwapchainKHR _swapchain,
                        const VkAllocationCallbacks *pAllocator)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_swapchain *sc = ta5_swapchain_from_handle(_swapchain);

   if (!_swapchain)
      return;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

#ifdef TA5_HAVE_WAYLAND
   if (sc->surface->wayland)
      ta5_wayland_swapchain_destroy(sc);
#endif

   for (uint32_t i = 0; i < sc->image_count; i++) {
      if (sc->images[i].fb_id && sc->drm_fd >= 0)
         drmModeRmFB(sc->drm_fd, sc->images[i].fb_id);
      if (sc->images[i].image)
         ta5_DestroyImage(_device,
                          ta5_image_to_handle(sc->images[i].image),
                          pAllocator);
      if (sc->images[i].memory)
         ta5_FreeMemory(_device,
                        ta5_memory_to_handle(sc->images[i].memory),
                        pAllocator);
   }
   if (sc->drm_fd >= 0)
      close(sc->drm_fd);

   vk_object_base_finish(&sc->base);
   vk_free(pAllocator, sc->images);
   vk_free(pAllocator, sc);
   (void)device;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_GetSwapchainImagesKHR(VkDevice _device, VkSwapchainKHR _swapchain,
                          uint32_t *pCount, VkImage *pImages)
{
   struct ta5_swapchain *sc = ta5_swapchain_from_handle(_swapchain);

   if (!sc || !pCount)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!pImages) {
      *pCount = sc->image_count;
      return VK_SUCCESS;
   }

   if (*pCount < sc->image_count) {
      *pCount = sc->image_count;
      return VK_INCOMPLETE;
   }

   for (uint32_t i = 0; i < sc->image_count; i++)
      pImages[i] = ta5_image_to_handle(sc->images[i].image);

   *pCount = sc->image_count;
   (void)_device;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_AcquireNextImageKHR(VkDevice _device, VkSwapchainKHR _swapchain,
                        uint64_t timeout, VkSemaphore _semaphore,
                        VkFence _fence, uint32_t *pImageIndex)
{
   struct ta5_device *device = (struct ta5_device *)_device;
   struct ta5_swapchain *sc = ta5_swapchain_from_handle(_swapchain);

   if (!sc || !pImageIndex)
      return vk_error(&device->instance->vk, VK_ERROR_INITIALIZATION_FAILED);

   /*
    * Hand out an image the compositor is not using, and wait -- bounded by the
    * application's own timeout -- for one to be released if they are all busy.
    *
    * This is wsi_common_wayland.c:3021-3052: scan for a non-busy image, and
    * when there is none, dispatch so that frame callbacks run and free one,
    * returning VK_TIMEOUT or VK_NOT_READY when the deadline passes.
    *
    * The old round-robin was wrong for wayland in the way that matters: it
    * ignored `busy`, so the compositor could be sampling a buffer we were
    * rendering into.  It is kept for the KMS path, where present is
    * synchronous with the scanout and nothing else can be using the buffer.
    *
    * A fence here is signalled immediately through the fence's vk_sync: the
    * image is already available, and vkcube waits on it before drawing.
    * Without this the wait never returns.  A semaphore is left alone --
    * ta5_QueueSubmit ignores wait semaphores entirely, so there is nothing
    * to signal it for.
    */
   (void)_semaphore;

#ifdef TA5_HAVE_WAYLAND
   if (sc->surface && sc->surface->wl_surface) {
      struct timespec start, deadline;
      uint64_t ns;

      clock_gettime(CLOCK_MONOTONIC, &start);
      /* Clamp before converting: vkcube asks for UINT64_MAX. */
      ns = (uint64_t)(timeout > 1000000ull ? 1000000ull : timeout) * 1000000ull;
      deadline.tv_sec = start.tv_sec + (time_t)(ns / 1000000000ull);
      deadline.tv_nsec =
         start.tv_nsec + (long)(ns % 1000000000ull);
      if (deadline.tv_nsec >= 1000000000L) {
         deadline.tv_nsec -= 1000000000L;
         deadline.tv_sec += 1;
      }

      while (true) {
         for (uint32_t i = 0; i < sc->image_count; i++) {
            uint32_t idx = (sc->next + i) % sc->image_count;

            if (!sc->images[idx].busy) {
               *pImageIndex = idx;
               sc->images[idx].busy = true;
               sc->next = (idx + 1) % sc->image_count;
               goto acquired;
            }
         }

         /* All busy: the compositor has to be given the chance to release
          * one, which happens by dispatching the connection. */
         {
            struct timespec now;
            int64_t left_ms;
            bool timed_out = false;

            clock_gettime(CLOCK_MONOTONIC, &now);
            left_ms = (int64_t)(deadline.tv_sec - now.tv_sec) * 1000 +
                      (deadline.tv_nsec - now.tv_nsec) / 1000000;
            if (left_ms <= 0)
               return vk_error(&device->instance->vk,
                               timeout ? VK_TIMEOUT : VK_NOT_READY);

            /*
             * Wait a second at a time even when the application asked for
             * longer.  vkcube passes UINT64_MAX, which overflows the deadline
             * arithmetic, and a single unbounded dispatch would put the
             * sixteen-minute hang back inside acquire instead of present.
             */
            if (left_ms > 1000)
               left_ms = 1000;

            if (ta5_wayland_acquire_dispatch(sc->surface, (int)left_ms,
                                             &timed_out) < 0)
               return vk_error(&device->instance->vk, VK_ERROR_OUT_OF_DATE_KHR);
            if (timed_out)
               return vk_error(&device->instance->vk,
                               timeout ? VK_TIMEOUT : VK_NOT_READY);
         }
      }
   }
#endif

   /* KMS: nothing else can be using the scanout buffer, so round-robin. */
   (void)timeout;
   *pImageIndex = sc->next;
   sc->next = (sc->next + 1) % sc->image_count;

#ifdef TA5_HAVE_WAYLAND
acquired:
#endif
   {

   if (_fence != VK_NULL_HANDLE) {
      struct ta5_fence *fence = ta5_fence_from_handle(_fence);
      VkResult ret = vk_sync_signal(&device->vk, fence->sync, 1);
      if (ret != VK_SUCCESS)
         return vk_error(&device->instance->vk, ret);
      fence->signaled = true;
   }

   return VK_SUCCESS;
   }
}

/**
 * Write the scanout buffer to a PPM, on a knob.
 *
 * Proof that the chain works end to end: this is the buffer whose framebuffer
 * id was just handed to KMS, read back by the host after the queue went idle,
 * so what lands in the file is what the panel is being asked to scan out.
 * DRM has no readback path of its own, so this is the only way to look at it
 * from userspace.
 *
 * TA5_TRACE_PPM=<path> writes one file and only on the first present; the
 * image is ABGR in memory (R8G8B8A8_UNORM) and RGB in the file.
 */
static void
ta5_dump_ppm(struct ta5_swapchain *sc, uint32_t index)
{
   struct ta5_swapchain_image *img = &sc->images[index];
   const char *path = getenv("TA5_TRACE_PPM");
   uint8_t *map;
   FILE *f;

   if (!path || !img->memory || !img->memory->bo)
      return;

   /*
    * One file per process by default: unset the knob so the next present is
    * quiet.  TA5_TRACE_PPM_ALL=1 keeps writing, one file per present with the
    * index in the name, which is the only way to tell "the first frame is
    * wrong" from "every frame is wrong" -- two failures that look identical
    * when only the first frame is ever looked at.
    */
   if (!getenv("TA5_TRACE_PPM_ALL"))
      unsetenv("TA5_TRACE_PPM");

   map = (uint8_t *)ta5_bo_map(img->memory->bo);
   if (!map) {
      fprintf(stderr, "ta5[wsi] ppm: map failed\n");
      return;
   }

   if (getenv("TA5_TRACE_PPM_ALL")) {
      static uint32_t frame;
      char name[600];
      snprintf(name, sizeof(name), "%s.%u", path, frame++);
      path = name;
   }

   f = fopen(path, "wb");
   if (!f) {
      fprintf(stderr, "ta5[wsi] ppm: cannot open %s\n", path);
      return;
   }

   /*
    * TA5_TRACE_PPM_SCALE=N writes every Nth pixel in each direction.  The
    * link to the development host runs at about 1.25 MB/s, so a full
    * 1080x2280 frame (7.4 MB) takes six seconds to pull and a live preview
    * is impossible; at N=4 the frame is 460 KB and streams at a few fps.
    * Nearest-neighbour, which for a downscaled preview is the right choice
    * anyway -- it cannot invent a pixel that was not rendered.
    */
   uint32_t scale = 1;
   const char *scale_env = getenv("TA5_TRACE_PPM_SCALE");
   uint32_t out_w, out_h;

   if (scale_env)
      scale = (uint32_t)atoi(scale_env);
   if (scale < 1)
      scale = 1;
   if (scale > sc->extent.width || scale > sc->extent.height)
      scale = 1;

   out_w = (sc->extent.width + scale - 1) / scale;
   out_h = (sc->extent.height + scale - 1) / scale;

   fprintf(f, "P6\n%u %u\n255\n", out_w, out_h);

   for (uint32_t y = 0; y < out_h; y++) {
      const uint8_t *row = map + (size_t)(y * scale) * img->image->row_pitch;

      for (uint32_t x = 0; x < out_w; x++) {
         const uint8_t *px = row + (size_t)(x * scale) * 4;

         fputc(px[0], f);   /* R */
         fputc(px[1], f);   /* G */
         fputc(px[2], f);   /* B */
      }
   }

   fclose(f);
   fprintf(stderr, "ta5[wsi] ppm: wrote %s (%ux%u)\n", path,
           sc->extent.width, sc->extent.height);
}

/**
 * Present one image: share its GEM handle to the KMS fd and scan it out.
 *
 * The image was allocated on the render node.  GEM handles do not cross fds,
 * so the handle is exported as a dma-buf and reimported on the card fd --
 * the standard PRIME bridge.  The imported handle is then wrapped once in a
 * DRM framebuffer (cached in the swapchain image) and set with
 * drmModeSetCrtc.
 *
 * R8G8B8A8_UNORM linear is DRM_FORMAT_ABGR32; B8G8R8A8_UNORM is ARGB32.
 * Pitch comes from the image's own row_pitch, which is what the GPU wrote.
 */
static VkResult
ta5_present_image(struct ta5_swapchain *sc, uint32_t index)
{
   struct ta5_swapchain_image *img = &sc->images[index];
   struct ta5_bo *bo = img->memory->bo;
   uint32_t gem_handle = bo->handle;
   uint32_t fb = img->fb_id;
   int prime_fd = -1;
   uint32_t kmsgem = 0;
   int ret;

#ifdef TA5_HAVE_WAYLAND
   /*
    * A Wayland swapchain is never scanned out by this process: the compositor
    * reads the same memory, handed over as a dma-buf.  Everything below is
    * KMS-only and would need DRM master, which is not ours to take.
    */
   if (sc->surface->wayland)
      return ta5_wayland_present(sc, index);
#endif

   if (sc->drm_fd < 0) {
      sc->drm_fd = ta5_drm_open();
      if (sc->drm_fd < 0)
         return VK_ERROR_INITIALIZATION_FAILED;
      /* Become DRM master if no one else is.  Failing here means another
       * client owns the display; the present cannot run, but the swapchain
       * itself is still valid. */
      if (drmSetMaster(sc->drm_fd) && getenv("TA5_TRACE_WSI"))
         fprintf(stderr, "ta5[wsi] drmSetMaster: %s\n", strerror(errno));
   }

   if (!fb) {
      /* PRIME export from the render node, import on the card. */
      ret = drmPrimeHandleToFD(sc->device->fd, gem_handle,
                               DRM_CLOEXEC, &prime_fd);
      if (ret)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;

      ret = drmPrimeFDToHandle(sc->drm_fd, prime_fd, &kmsgem);
      close(prime_fd);
      if (ret)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;

      uint32_t pitches[4] = { (uint32_t)img->image->row_pitch, 0, 0, 0 };
      uint32_t offsets[4] = { 0, 0, 0, 0 };
      uint32_t handles[4] = { kmsgem, 0, 0, 0 };
      uint32_t fmt = (img->image->format == VK_FORMAT_B8G8R8A8_UNORM)
                        ? DRM_FORMAT_ARGB8888
                        : DRM_FORMAT_ABGR8888;

      ret = drmModeAddFB2(sc->drm_fd, sc->extent.width, sc->extent.height,
                          fmt, handles, pitches, offsets, &fb, 0);
      if (getenv("TA5_TRACE_WSI"))
         fprintf(stderr,
                 "ta5[wsi] AddFB2: %ux%u fmt=%s pitch=%u ret=%d errno=%s\n",
                 sc->extent.width, sc->extent.height,
                 (fmt == DRM_FORMAT_ARGB8888) ? "ARGB8888" : "ABGR8888",
                 pitches[0], ret, ret ? strerror(errno) : "-");

      if (ret) {
         struct drm_gem_close cl = { .handle = kmsgem };
         drmIoctl(sc->drm_fd, DRM_IOCTL_GEM_CLOSE, &cl);
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
      img->fb_id = fb;
   }

   /*
    * Dumped before the scanout is programmed, not after: what this file shows
    * is what the driver rendered, and it stays measurable while another client
    * holds DRM master -- which is most of the time on this device, where phoc
    * and systemd-logind between them own card0 and the only way to get it is
    * to kill the compositor.  Iterating on the render should not need that.
    */
   ta5_dump_ppm(sc, index);

   ret = drmModeSetCrtc(sc->drm_fd, sc->crtc_id, fb, 0, 0,
                        &sc->connector_id, 1,
                        &sc->surface->display->drm_mode);

   if (getenv("TA5_TRACE_WSI"))
      fprintf(stderr,
              "ta5[wsi] present: crtc=%u connector=%u fb=%u %ux%u ret=%d "
              "errno=%s\n",
              sc->crtc_id, sc->connector_id, fb, sc->extent.width,
              sc->extent.height, ret, ret ? strerror(errno) : "-");

   if (ret)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_QueuePresentKHR(VkQueue _queue,
                    const VkPresentInfoKHR *pPresentInfo)
{
   (void)_queue;
   VkResult overall = VK_SUCCESS;

   if (!pPresentInfo)
      return VK_ERROR_INITIALIZATION_FAILED;

   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      struct ta5_swapchain *sc =
         ta5_swapchain_from_handle(pPresentInfo->pSwapchains[i]);
      uint32_t index = pPresentInfo->pImageIndices[i];
      VkResult ret;

      if (!sc || index >= sc->image_count)
         return VK_ERROR_INITIALIZATION_FAILED;

      /* The draw must have finished before the scanout reads the buffer.
       * A full queue wait is the whole barrier: one queue, in-order
       * submission, so idle means the image is complete. */
      ta5_QueueWaitIdle(_queue);

      ret = ta5_present_image(sc, index);
      if (ret != VK_SUCCESS)
         overall = ret;
      if (pPresentInfo->pResults)
         ((VkResult *)pPresentInfo->pResults)[i] = ret;
   }

   return overall;
}
