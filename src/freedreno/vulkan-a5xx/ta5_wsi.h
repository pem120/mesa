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

#include <stdbool.h>
#include <stdint.h>

#include <xf86drmMode.h>

#ifdef TA5_HAVE_WAYLAND
struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_shm;
struct wl_drm;
struct wl_shm_pool;
struct wl_buffer;
struct zwp_linux_dmabuf_v1;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
#endif

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

/**
 * A surface is either a KMS display plane or a Wayland compositor's window.
 *
 * Both are VkSurfaceKHR; the kind decides which query path answers for it and
 * which present path runs.  A display surface has a mode and a plane; a Wayland
 * surface has a wl_display, an xdg_toplevel and a size the compositor dictates.
 */
struct ta5_surface
{
   struct vk_object_base base;
   struct ta5_instance *instance;

   /* Display-plane surface (VK_KHR_display).  NULL for a Wayland surface. */
   struct ta5_display *display;
   struct ta5_display_mode *mode;
   uint32_t plane_index;

   /* VK_KHR_wayland_surface.  All NULL for a display surface. */
   bool wayland;

   /* VK_EXT_headless_surface.  No display, no compositor: the surface is a
    * token the swapchain hangs off, and present is a no-op. */
   bool headless;
   struct wl_display *wl_display;
   struct wl_registry *wl_registry;
   struct wl_compositor *wl_compositor;
   struct zwp_linux_dmabuf_v1 *wl_dmabuf;
   struct wl_shm *wl_shm;
   struct wl_drm *wl_drm;
   struct xdg_wm_base *wl_xdg_wm_base;
   struct xdg_surface *wl_xdg_surface;
   struct xdg_toplevel *wl_toplevel;
   struct wl_surface *wl_surface;

   /* Size the compositor asked for; valid once wl_configured is set. */
   uint32_t wl_width;
   uint32_t wl_height;
   int32_t wl_pending_serial;
   bool wl_configured;
   bool wl_frame_done;
};

/** One swapchain image: a colour-attachment image plus its GEM state. */
struct ta5_swapchain;

/**
 * What a frame callback is told, so it can release the right image.
 *
 * One of these per present; the callback owns it and frees it.
 */
struct ta5_frame_cookie
{
   struct ta5_swapchain *sc;
   uint32_t index;
};

struct ta5_swapchain_image
{
   struct ta5_image *image;
   struct ta5_memory *memory;
   uint32_t fb_id; /**< DRM framebuffer, created on first present */
#ifdef TA5_HAVE_WAYLAND
   struct wl_buffer *wl_buffer;    /**< created on first present, like fb_id */
   struct wl_shm_pool *wl_shm_pool; /**< the shm path's backing pool */
   int wl_shm_fd;                   /**< and its memfd, -1 until created */
#endif
   /**
    * The compositor has this image and has not finished with it.
    *
    * This is wsi_common's per-image `busy` (wsi_common_wayland.c:164): set on
    * acquire, cleared when the frame callback fires.  Acquire hands out a
    * non-busy image, so a buffer the compositor is still sampling is never
    * rendered into again.
    */
   bool busy;
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

#ifdef TA5_HAVE_WAYLAND
/**
 * Flush and dispatch the connection, waiting up to timeout_ms.
 *
 * Exposed for ta5_AcquireNextImageKHR, which waits for a frame callback to
 * release an image.  Sets *timed_out when the deadline passed without events.
 * Returns 0 on success, -1 on error.
 */
int ta5_wayland_acquire_dispatch(struct ta5_surface *surf, int timeout_ms,
                                 bool *timed_out);
#endif

VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_display, base, VkDisplayKHR,
                               VK_OBJECT_TYPE_DISPLAY_KHR)
VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_display_mode, base, VkDisplayModeKHR,
                               VK_OBJECT_TYPE_DISPLAY_MODE_KHR)
VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_surface, base, VkSurfaceKHR,
                               VK_OBJECT_TYPE_SURFACE_KHR)
VK_DEFINE_NONDISP_HANDLE_CASTS(ta5_swapchain, base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

#ifdef TA5_HAVE_WAYLAND
/* ta5_wayland.cc.  Declared here so ta5_wsi.cc can route without including the
 * Wayland headers, which the rest of the driver has no reason to see. */
void ta5_DestroyWaylandSurface(struct ta5_surface *surf,
                              const VkAllocationCallbacks *pAllocator);
VkResult ta5_wayland_capabilities(struct ta5_surface *surf,
                                  VkSurfaceCapabilitiesKHR *pCaps);
VkResult ta5_wayland_present(struct ta5_swapchain *sc, uint32_t index);
void ta5_wayland_swapchain_destroy(struct ta5_swapchain *sc);
#endif

#endif /* TA5_WSI_H */
