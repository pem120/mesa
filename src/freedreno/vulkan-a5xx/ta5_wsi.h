/*
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 */

/**
 * \file ta5_wsi.h
 *
 * Adreno 5xx Vulkan driver: WSI (display + swapchain) for real KMS scanout.
 *
 * vkcube --wsi display is the target: VK_KHR_surface + VK_KHR_display on the
 * instance, VK_KHR_swapchain on the device, presenting through DRM KMS on
 * card0-DSI-1.  No X11, no Wayland, no fbdev stub.
 */

#ifndef TA5_WSI_H
#define TA5_WSI_H

#include "vulkan/vulkan_core.h"
#include "vk_object.h"

#include <xf86drmMode.h>

struct ta5_instance;
struct ta5_physical_device;
struct ta5_device;

/** One DRM display (a connected connector + its current mode). */
struct ta5_display
{
   struct vk_object_base base;
   struct ta5_physical_device *physical_device;

   uint32_t connector_id;
   uint32_t crtc_id;
   uint32_t plane_id;

   char name[64];

   /* The mode vkcube will use: the connector's preferred mode. */
   uint32_t width;
   uint32_t height;
   uint32_t refresh_numer;
   uint32_t refresh_denom;
   drmModeModeInfo drm_mode;
};

/** A display mode belonging to a ta5_display. */
struct ta5_display_mode
{
   struct vk_object_base base;
   struct ta5_display *display;

   VkDisplayModeParametersKHR params;
};

/** A display-plane surface vkcube renders into. */
struct ta5_surface
{
   struct vk_object_base base;
   struct ta5_instance *instance;

   struct ta5_display *display;
   struct ta5_display_mode *mode;
   uint32_t plane_index;
};

/** One swapchain image: a colour-attachment image plus its GEM state. */
struct ta5_swapchain_image
{
   struct ta5_image *image;
   struct ta5_memory *memory;
   uint32_t fb_id; /**< DRM framebuffer, created on first present */
};

/** A swapchain over a display-plane surface, presented via drmModeSetCrtc. */
struct ta5_swapchain
{
   struct vk_object_base base;
   struct ta5_device *device;
   struct ta5_surface *surface;

   VkFormat format;
   VkColorSpaceKHR color_space;
   VkExtent2D extent;
   uint32_t image_count;
   VkPresentModeKHR present_mode;

   struct ta5_swapchain_image *images;

   /** Next image to hand out; round-robin, the only mode FIFO needs. */
   uint32_t next;

   /** DRM card fd for present.  Opened on first present, kept for life. */
   int drm_fd;
   uint32_t crtc_id;
   uint32_t connector_id;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_display, base, VkDisplayKHR,
                               VK_OBJECT_TYPE_DISPLAY_KHR)
VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_display_mode, base, VkDisplayModeKHR,
                               VK_OBJECT_TYPE_DISPLAY_MODE_KHR)
VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_surface, base, VkSurfaceKHR,
                               VK_OBJECT_TYPE_SURFACE_KHR)
VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_swapchain, base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

#endif /* TA5_WSI_H */
