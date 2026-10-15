/*
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 */

/**
 * \file ta5_wayland.cc
 *
 * Adreno 5xx Vulkan driver: VK_KHR_wayland_surface.
 *
 * A Wayland surface for a real compositor -- phoc on the device, or waypipe on
 * the far end of an ssh tunnel -- presenting zero-copy through linux-dmabuf.
 * The swapchain image's GEM handle is exported as a dma-buf and imported by the
 * compositor directly: the same PRIME step ta5_present_image() does for KMS,
 * with the compositor reading the very memory the GPU wrote.
 *
 * Everything here is conditional on TA5_HAVE_WAYLAND.  The driver cross-builds
 * -static for a device that ships only libwayland-client.so.0, so that library
 * is linked in explicitly and the extension is advertised only when the build
 * found everything it needs; see meson.build.
 */

#if TA5_HAVE_WAYLAND

#include "ta5_wsi.h"
#include "ta5_instance.h"
#include "ta5_device.h"
#include "ta5_memory.h"
#include "ta5_submit.h"

/*
 * ta5_entrypoints.h guards its Wayland declarations on
 * VK_USE_PLATFORM_WAYLAND_KHR, and nothing else defines it: Khronos leaves the
 * macro to the application, and vulkan_wayland.h only declares the structs.
 * This driver is the application here, so it defines the macro itself --
 * without it the two Wayland entry points compile with no prototype in sight
 * and -Wmissing-prototypes stops the build on functions that are declared,
 * just not in this translation unit's view of the header.
 */
#ifndef VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#endif
#include <vulkan/vulkan_wayland.h>

#include "ta5_entrypoints.h"

#include "vk_alloc.h"
#include "vk_object.h"

#include <xf86drm.h>
#include <drm_fourcc.h>

#include <wayland-client.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>

#include "xdg_shell-client-protocol.h"
#include "linux_dmabuf-client-protocol.h"

/* Set on the first present, so the stopgap rate limit has a reference. */
static struct timespec ta5_last_present;

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

/**
 * Bind what a surface needs, at the highest version both sides have.
 *
 * The registry round trip has already delivered every global by the time this
 * runs, so binding here is not speculative.
 */
static void
ta5_wl_global(void *data, struct wl_registry *registry, uint32_t name,
              const char *interface, uint32_t version)
{
   struct ta5_surface *surf = (struct ta5_surface *)data;

   if (strcmp(interface, wl_compositor_interface.name) == 0) {
      /*
       * Bound at 5, not the highest on offer.  wl_compositor v6 adds
       * wl_surface.preferred_buffer_scale and preferred_buffer_transform, and
       * this file's listener table comes from the wayland-client headers the
       * build was given, which may predate those events.  A v6 surface then
       * gets two events the table has no entries for, and libwayland aborts
       * the process with "listener function for opcode 2 of wl_surface is
       * NULL" -- which is what happened, on the first frame, after one
       * successful present.  Nothing here needs v6: create_surface and
       * create_region have not changed since v1.
       */
      surf->wl_compositor = (struct wl_compositor *)wl_registry_bind(
         registry, name, &wl_compositor_interface, version < 5 ? version : 5);
   } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
      /*
       * Version 3 is the floor: create_params() taking an fd arrived in v3,
       * and v1/v2 only describe buffers this driver does not make.
       */
      if (version >= 3)
         surf->wl_dmabuf = (struct zwp_linux_dmabuf_v1 *)wl_registry_bind(
            registry, name, &zwp_linux_dmabuf_v1_interface,
            version < 4 ? version : 4);
   } else if (strcmp(interface, wl_shm_interface.name) == 0) {
      surf->wl_shm = (struct wl_shm *)wl_registry_bind(
         registry, name, &wl_shm_interface, version < 1 ? version : 1);
   } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
      surf->wl_xdg_wm_base = (struct xdg_wm_base *)wl_registry_bind(
         registry, name, &xdg_wm_base_interface, version < 3 ? version : 3);
   }
}

static void
ta5_wl_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
   (void)data;
   (void)registry;
   (void)name;
}

static const struct wl_registry_listener ta5_wl_registry_listener = {
   ta5_wl_global,
   ta5_wl_global_remove,
};

/**
 * Answer the compositor's ping.
 *
 * An xdg client that does not pong is treated as unresponsive and the
 * compositor is entitled to kill it, so this is not optional once there is a
 * window on the screen.
 */
static void
ta5_wl_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
   (void)data;
   xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener ta5_wl_wm_base_listener = {
   ta5_wl_wm_base_ping,
};

/* ------------------------------------------------------------------ */
/* xdg_surface configure                                               */
/* ------------------------------------------------------------------ */

/**
 * Acknowledge the configure and remember its serial.
 *
 * Wayland has no "what size is the window" query: the compositor says, and the
 * client has to use it.  Until the first configure arrives the surface has no
 * size, and ta5_wayland_capabilities() reports 0xFFFFFFFF for currentExtent
 * rather than inventing one.
 */
static void
ta5_wl_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                             uint32_t serial)
{
   struct ta5_surface *surf = (struct ta5_surface *)data;

   surf->wl_pending_serial = (int32_t)serial;
   surf->wl_configured = true;
   xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener ta5_wl_xdg_surface_listener = {
   ta5_wl_xdg_surface_configure,
};

/**
 * The size, which is the one thing capabilities has to know.
 *
 * xdg_surface.configure carries only a serial; the width and height come from
 * xdg_toplevel.configure, and missing that listener is what made
 * vkGetPhysicalDeviceSurfaceCapabilitiesKHR report 0x0 on a surface it also
 * said was configured.  vkcube builds its swapchain from currentExtent, asked
 * for a 0x0 image, and crashed in vkGetImageMemoryRequirements with a NULL
 * image.
 *
 * A compositor is allowed to configure 0x0 -- "unspecified, you choose" -- and
 * the last non-zero size is what stands then.
 */
static void
ta5_wl_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                          int32_t width, int32_t height,
                          struct wl_array *states)
{
   struct ta5_surface *surf = (struct ta5_surface *)data;

   (void)toplevel;
   (void)states;
   if (width > 0 && height > 0) {
      surf->wl_width = (uint32_t)width;
      surf->wl_height = (uint32_t)height;
   }
}

static void
ta5_wl_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
   struct ta5_surface *surf = (struct ta5_surface *)data;

   (void)toplevel;
   /* The window is going away.  Nothing to do here yet: the swapchain will
    * fail on its next present and the application will see it. */
   surf->wl_frame_done = true;
}

static void
ta5_wl_toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                                 int32_t width, int32_t height)
{
   (void)data;
   (void)toplevel;
   (void)width;
   (void)height;
}

static const struct xdg_toplevel_listener ta5_wl_toplevel_listener = {
   ta5_wl_toplevel_configure,
   ta5_wl_toplevel_close,
   ta5_wl_toplevel_configure_bounds,
};

static void
ta5_wl_surface_enter(void *data, struct wl_surface *wl_surface,
                     struct wl_output *output)
{
   (void)data;
   (void)wl_surface;
   (void)output;
}

static void
ta5_wl_surface_leave(void *data, struct wl_surface *wl_surface,
                     struct wl_output *output)
{
   (void)data;
   (void)wl_surface;
   (void)output;
}

/*
 * Core wl_surface has no configure: enter and leave only say which outputs the
 * surface is on.  The size arrives through xdg_surface's configure, above.
 */
static const struct wl_surface_listener ta5_wl_surface_listener = {
   ta5_wl_surface_enter,
   ta5_wl_surface_leave,
};

/* ------------------------------------------------------------------ */
/* Frame callback                                                      */
/* ------------------------------------------------------------------ */

/**
 * End of a frame: the compositor has finished with the buffer.
 *
 * This is the pacing signal, so present() waits on it rather than queuing
 * frames as fast as the CPU can record them.
 */
static void
ta5_wl_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
   struct ta5_surface *surf = (struct ta5_surface *)data;

   (void)time;
   surf->wl_frame_done = true;
   wl_callback_destroy(callback);
}

static const struct wl_callback_listener ta5_wl_frame_listener = {
   ta5_wl_frame_done,
};

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkBool32 VKAPI_CALL
ta5_GetPhysicalDeviceWaylandPresentationSupportKHR(
   VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
   struct wl_display *display)
{
   /*
    * There is one queue and one display, so every queue presents to every
    * Wayland display that exists.  Whether *this* display will work is a
    * question vkCreateWaylandSurfaceKHR answers with specifics.
    */
   (void)physicalDevice;
   (void)queueFamilyIndex;
   (void)display;
   return VK_TRUE;
}

VKAPI_ATTR VkResult VKAPI_CALL
ta5_CreateWaylandSurfaceKHR(VkInstance _instance,
                            const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkSurfaceKHR *pSurface)
{
   struct ta5_instance *instance = (struct ta5_instance *)_instance;
   struct ta5_surface *surf;
   int roundtrips;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   if (!pCreateInfo || !pCreateInfo->display)
      return vk_error(&instance->vk, VK_ERROR_INITIALIZATION_FAILED);

   surf = (struct ta5_surface *)vk_zalloc(
      pAllocator, sizeof(*surf), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!surf)
      return vk_error(&instance->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_instance_init(&instance->vk, &surf->base,
                                VK_OBJECT_TYPE_SURFACE_KHR);
   surf->instance = instance;
   surf->wayland = true;

   /*
    * The wl_display is the application's, not ours: VkWaylandSurfaceCreateInfoKHR
    * hands over a live connection and the application keeps it running.  We
    * create proxies on it -- registry, compositor, xdg objects -- and destroy
    * those.  Calling wl_display_disconnect() here would hang up the
    * application's connection out from under it.
    */
   surf->wl_display = pCreateInfo->display;

   fprintf(stderr, "ta5[wl] create: display=%p\n", (void *)surf->wl_display);
   surf->wl_registry = wl_display_get_registry(surf->wl_display);
   wl_registry_add_listener(surf->wl_registry, &ta5_wl_registry_listener, surf);

   fprintf(stderr, "ta5[wl] create: registry added, roundtrip...\n");
   /* One round trip delivers every global to the listener above. */
   if (wl_display_roundtrip(surf->wl_display) < 0 || !surf->wl_compositor ||
       !surf->wl_xdg_wm_base || !surf->wl_dmabuf) {
      fprintf(stderr,
              "ta5[wl] surface: compositor is missing %s%s%s "
              "(wl_compositor=%p xdg_wm_base=%p linux_dmabuf=%p)\n",
              !surf->wl_compositor ? "wl_compositor " : "",
              !surf->wl_xdg_wm_base ? "xdg_wm_base " : "",
              !surf->wl_dmabuf ? "zwp_linux_dmabuf_v1 " : "",
              (void *)surf->wl_compositor, (void *)surf->wl_xdg_wm_base,
              (void *)surf->wl_dmabuf);
      ta5_DestroyWaylandSurface(surf, pAllocator);
      return vk_error(&instance->vk, VK_ERROR_INITIALIZATION_FAILED);
   }

   fprintf(stderr, "ta5[wl] create: globals bound\n");
   xdg_wm_base_add_listener(surf->wl_xdg_wm_base, &ta5_wl_wm_base_listener, surf);

   surf->wl_surface = wl_compositor_create_surface(surf->wl_compositor);
   wl_surface_add_listener(surf->wl_surface, &ta5_wl_surface_listener, surf);
   surf->wl_xdg_surface =
      xdg_wm_base_get_xdg_surface(surf->wl_xdg_wm_base, surf->wl_surface);
   xdg_surface_add_listener(surf->wl_xdg_surface,
                            &ta5_wl_xdg_surface_listener, surf);
   fprintf(stderr, "ta5[wl] create: xdg_surface made\n");
   surf->wl_toplevel = xdg_surface_get_toplevel(surf->wl_xdg_surface);
   xdg_toplevel_add_listener(surf->wl_toplevel, &ta5_wl_toplevel_listener, surf);

   xdg_toplevel_set_title(surf->wl_toplevel, "Adreno 5xx (ta5)");
   xdg_toplevel_set_app_id(surf->wl_toplevel, "org.mesa.ta5");

   /*
    * No set_window_geometry yet: there is nothing to set it to until the
    * compositor has said how big the window is, and this commit is what asks
    * for that.
    */
   fprintf(stderr, "ta5[wl] create: toplevel made, committing\n");
   wl_surface_commit(surf->wl_surface);

   /*
    * Wait for the first configure, so that the application sees a real extent
    * from its very first vkGetPhysicalDeviceSurfaceCapabilitiesKHR instead of
    * 0xFFFFFFFF.  Bounded: a compositor that never configures must not wedge
    * the application here, and the surface is still usable -- it just reports
    * the "extent may change" sentinel until a configure turns up.
    */
   for (roundtrips = 0; roundtrips < 8 && !surf->wl_configured; roundtrips++) {
      if (wl_display_roundtrip(surf->wl_display) < 0)
         break;
   }

   fprintf(stderr, "ta5[wl] create: configured=%d %ux%u\n",
           (int)surf->wl_configured, surf->wl_width, surf->wl_height);
   *pSurface = ta5_surface_to_handle(surf);
   return VK_SUCCESS;
}

void
ta5_DestroyWaylandSurface(struct ta5_surface *surf,
                          const VkAllocationCallbacks *pAllocator)
{
   if (!surf)
      return;

   if (surf->wl_toplevel)
      xdg_toplevel_destroy(surf->wl_toplevel);
   if (surf->wl_xdg_surface)
      xdg_surface_destroy(surf->wl_xdg_surface);
   if (surf->wl_surface)
      wl_surface_destroy(surf->wl_surface);
   if (surf->wl_dmabuf)
      zwp_linux_dmabuf_v1_destroy(surf->wl_dmabuf);
   if (surf->wl_compositor)
      wl_compositor_destroy(surf->wl_compositor);
   if (surf->wl_xdg_wm_base)
      xdg_wm_base_destroy(surf->wl_xdg_wm_base);
   if (surf->wl_registry)
      wl_registry_destroy(surf->wl_registry);
   /* The display is the application's; see ta5_CreateWaylandSurfaceKHR. */

   vk_object_base_finish(&surf->base);
   vk_free(pAllocator, surf);
}

VkResult
ta5_wayland_capabilities(struct ta5_surface *surf,
                         VkSurfaceCapabilitiesKHR *pCaps)
{
   if (!pCaps)
      return VK_ERROR_INITIALIZATION_FAILED;

   pCaps->minImageCount = 2;
   pCaps->maxImageCount = 3;

   /*
    * No configure yet means no size, and the spec's way of saying "I do not
    * know yet" is the 0xFFFFFFFF sentinel -- not a guess, which the
    * application could act on by creating a swapchain at the wrong size.
    */
   if (surf->wl_configured) {
      pCaps->currentExtent.width = surf->wl_width;
      pCaps->currentExtent.height = surf->wl_height;
   } else {
      pCaps->currentExtent.width = 0xffffffffu;
      pCaps->currentExtent.height = 0xffffffffu;
   }

   pCaps->minImageExtent = (VkExtent2D){ 1, 1 };
   pCaps->maxImageExtent = (VkExtent2D){ 4096, 4096 };
   pCaps->maxImageArrayLayers = 1;
   pCaps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   pCaps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   pCaps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   pCaps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   return VK_SUCCESS;
}

/**
 * Wait for the frame callback, with a bound.
 *
 * The callback is what makes this a FIFO present.  The bound is a backstop,
 * not pacing -- a compositor that has gone quiet should cost a second of
 * latency on the next frame, not wedge the application forever.
 */
static void
ta5_wayland_wait_frame(struct ta5_surface *surf)
{
   int waits;

   surf->wl_frame_done = false;

   for (waits = 0; waits < 1000 && !surf->wl_frame_done; waits++) {
      struct pollfd pfd;
      int ret;

      while (wl_display_prepare_read(surf->wl_display) != 0)
         if (wl_display_dispatch_pending(surf->wl_display) < 0)
            return;
      wl_display_flush(surf->wl_display);

      pfd.fd = wl_display_get_fd(surf->wl_display);
      pfd.events = POLLIN;
      pfd.revents = 0;

      ret = poll(&pfd, 1, 1000);
      if (ret <= 0) {
         wl_display_cancel_read(surf->wl_display);
         return;
      }

      if (wl_display_read_events(surf->wl_display) < 0)
         return;
      if (wl_display_dispatch_pending(surf->wl_display) < 0)
         return;
   }
}

/**
 * Present one swapchain image to a Wayland compositor, zero-copy.
 *
 * The image was rendered into a linear GEM buffer on the render node.  The
 * compositor is on the same device, so the handle goes over as a dma-buf fd --
 * the PRIME export ta5_present_image() does for KMS -- and the compositor maps
 * the very memory the GPU wrote.
 *
 * The fourcc is the DRM code the image's Vulkan format already is:
 * R8G8B8A8_UNORM is DRM_FORMAT_ABGR8888, B8G8R8A8_UNORM is ARGB8888.  No
 * modifier is added, so linux-dmabuf v1 reads it as DRM_FORMAT_MOD_LINEAR,
 * which is how ta5_CreateImage lays every image out.
 *
 * Ordering is the same as the KMS path's: ta5_QueuePresentKHR has already
 * waited for the queue, so the GPU is finished with the buffer before the
 * compositor is told to read it.
 */
/**
 * Present through wl_shm: copy the image into a memfd-backed pool and let the
 * compositor upload it.
 *
 * This exists because the dma-buf path is what is faulting phoc's GPU -- its
 * walk runs off the end on the very first import of our buffer -- and because
 * a buffer the compositor reaches over shared memory takes its GPU out of the
 * picture entirely.  TA5_WL_SHM=1 selects it; the default stays dmabuf so the
 * two can be told apart in one A/B.
 *
 * The copy is a per-row memcpy of a 360x713 RGBA image, about a megabyte, and
 * it is the whole cost: at the 60 Hz cap that is 60 MB/s of memcpy, which is
 * nothing.
 */
static VkResult
ta5_wayland_present_shm(struct ta5_swapchain *sc, uint32_t index)
{
   struct ta5_surface *surf = sc->surface;
   struct ta5_swapchain_image *img = &sc->images[index];
   struct wl_callback *callback;
   uint32_t stride = (uint32_t)img->image->row_pitch;
   uint32_t size = stride * sc->extent.height;
   uint8_t *map;
   void *src;

   if (!surf->wl_shm)
      return VK_ERROR_SURFACE_LOST_KHR;

   /*
    * The memfd, the pool and the wl_buffer are per swapchain image and live as
    * long as the image does -- created once on the image's first present, like
    * fb_id on the KMS path.
    *
    * Destroying the wl_buffer at the end of every present is what took the
    * compositor down.  The frame callback fires when the compositor wants the
    * *next* frame, which is not the same as being finished with this one:
    * wlroots keeps the committed buffer referenced by the surface state and by
    * the output until the frame after it is displayed.  Killing the protocol
    * object under it leaves its renderer sampling a texture whose backing is
    * gone, and its GPU then walks whatever the freed mapping left behind --
    * the storm that ran from iova 0 through phoc's KMS VM.
    */
   if (!img->wl_buffer) {
      int fd = memfd_create("ta5-shm", 0);

      if (fd < 0)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      if (ftruncate(fd, size)) {
         close(fd);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      img->wl_shm_pool = wl_shm_create_pool(surf->wl_shm, fd, size);
      img->wl_buffer = wl_shm_pool_create_buffer(
         img->wl_shm_pool, 0, sc->extent.width, sc->extent.height, stride,
         WL_SHM_FORMAT_ABGR8888);
      img->wl_shm_fd = fd;   /* kept open: the per-frame mmap uses it */
      if (!img->wl_buffer) {
         wl_display_flush(surf->wl_display);
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }

   map = (uint8_t *)mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_SHARED,
                         img->wl_shm_fd, 0);
   if (map == MAP_FAILED)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   src = (void *)ta5_bo_map(img->memory->bo);
   if (!src) {
      munmap(map, size);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   for (uint32_t y = 0; y < sc->extent.height; y++)
      memcpy(map + (size_t)y * stride,
             (const uint8_t *)src + (size_t)y * stride, stride);
   munmap(map, size);

   wl_surface_attach(surf->wl_surface, img->wl_buffer, 0, 0);
   wl_surface_damage_buffer(surf->wl_surface, 0, 0, sc->extent.width,
                            sc->extent.height);

   callback = wl_surface_frame(surf->wl_surface);
   if (callback)
      wl_callback_add_listener(callback, &ta5_wl_frame_listener, surf);

   wl_surface_commit(surf->wl_surface);
   wl_display_flush(surf->wl_display);

   if (getenv("TA5_TRACE_WL"))
      fprintf(stderr, "ta5[wl] shm present: %ux%u stride=%u\n",
              sc->extent.width, sc->extent.height, stride);

   if (callback)
      ta5_wayland_wait_frame(surf);

   /* The buffer is the image's, and stays until the swapchain dies. */

   /* Same stopgap rate limit as the dma-buf path. */
   {
      struct timespec now;

      clock_gettime(CLOCK_MONOTONIC, &now);
      int64_t ns = (int64_t)(now.tv_sec - ta5_last_present.tv_sec) * 1000000000 +
                   (now.tv_nsec - ta5_last_present.tv_nsec);
      if (ns < 16666666) {
         struct timespec delay;
         delay.tv_sec = 0;
         delay.tv_nsec = 16666666 - ns;
         nanosleep(&delay, NULL);
         clock_gettime(CLOCK_MONOTONIC, &ta5_last_present);
      } else {
         ta5_last_present = now;
      }
   }

   return VK_SUCCESS;
}

void
ta5_wayland_swapchain_destroy(struct ta5_swapchain *sc)
{
   for (uint32_t i = 0; i < sc->image_count; i++) {
      struct ta5_swapchain_image *img = &sc->images[i];

      if (img->wl_buffer) {
         wl_buffer_destroy(img->wl_buffer);
         img->wl_buffer = NULL;
      }
      if (img->wl_shm_pool) {
         wl_shm_pool_destroy(img->wl_shm_pool);
         img->wl_shm_pool = NULL;
      }
      if (img->wl_shm_fd >= 0) {
         close(img->wl_shm_fd);
         img->wl_shm_fd = -1;
      }
   }
}

VkResult
ta5_wayland_present(struct ta5_swapchain *sc, uint32_t index)
{
   struct ta5_surface *surf = sc->surface;
   struct ta5_swapchain_image *img = &sc->images[index];
   struct wl_buffer *buffer;
   struct wl_callback *callback;
   struct ta5_bo *bo = img->memory->bo;
   uint32_t fourcc = 0;

   if (!surf->wl_surface)
      return VK_ERROR_SURFACE_LOST_KHR;

   if (getenv("TA5_WL_SHM"))
      return ta5_wayland_present_shm(sc, index);

   if (!surf->wl_dmabuf)
      return VK_ERROR_SURFACE_LOST_KHR;

   /*
    * The wl_buffer is per swapchain image and lives as long as the image, like
    * the KMS path's fb_id.  It is NOT created and destroyed per present: the
    * frame callback fires when the compositor wants the next frame, and
    * destroying the buffer there pulled it out from under a compositor that
    * still held it, which is the storm that ran from iova 0 through phoc's
    * KMS VM and killed the compositor.  See ta5_wayland_present_shm() for the
    * longer version.
    */
   if (!img->wl_buffer) {
      int prime_fd = -1;
      struct zwp_linux_buffer_params_v1 *params;

      if (drmPrimeHandleToFD(sc->device->fd, bo->handle, DRM_CLOEXEC, &prime_fd))
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;

      fourcc = (img->image->format == VK_FORMAT_B8G8R8A8_UNORM)
                 ? DRM_FORMAT_ARGB8888
                 : DRM_FORMAT_ABGR8888;

      /*
       * One plane, at offset 0, implicit LINEAR.  create_immed() rather than
       * create() skips a round trip and means the wl_buffer exists by the time
       * attach runs.
       */
      params = zwp_linux_dmabuf_v1_create_params(surf->wl_dmabuf);
      zwp_linux_buffer_params_v1_add(params, prime_fd, 0, 0,
                                     (uint32_t)img->image->row_pitch, 0, 0);
      img->wl_buffer = zwp_linux_buffer_params_v1_create_immed(
         params, sc->extent.width, sc->extent.height, fourcc, 0);
      zwp_linux_buffer_params_v1_destroy(params);
      close(prime_fd);

      if (!img->wl_buffer) {
         wl_display_flush(surf->wl_display);
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }

   buffer = img->wl_buffer;

   wl_surface_attach(surf->wl_surface, buffer, 0, 0);
   wl_surface_damage_buffer(surf->wl_surface, 0, 0, sc->extent.width,
                            sc->extent.height);

   /* Pacing, and the point at which the compositor is finished with buffer. */
   callback = wl_surface_frame(surf->wl_surface);
   if (callback)
      wl_callback_add_listener(callback, &ta5_wl_frame_listener, surf);

   wl_surface_commit(surf->wl_surface);
   wl_display_flush(surf->wl_display);

   if (getenv("TA5_TRACE_WL"))
      fprintf(stderr,
              "ta5[wl] present: %ux%u fourcc=0x%08x stride=%u buffer=%p "
              "configured=%d\n",
              sc->extent.width, sc->extent.height, fourcc,
              (unsigned)img->image->row_pitch, (void *)buffer,
              (int)surf->wl_configured);

   if (callback)
      ta5_wayland_wait_frame(surf);

   /*
    * The wl_buffer is NOT destroyed here.  It is the image's, and it lives
    * until the swapchain does -- see the comment at the top of this function.
    */

   /*
    * STOPGAP, and it needs saying plainly: the frame callback is not pacing
    * this.  phoc answered 4,940 of them in 16 seconds -- immediate, not
    * vsynced -- which flooded it with three hundred buffer imports a second
    * and took the compositor down.  Until the reason for the immediate
    * callbacks is understood (an occluded surface is the usual one: a
    * compositor releases frame callbacks as fast as it can for a surface it is
    * not compositing), cap the present rate at 60 Hz.  This is a rate limit,
    * not pacing: the callback is still what sequences a frame, and this only
    * stops the flood.
    */
   {
      struct timespec now;

      clock_gettime(CLOCK_MONOTONIC, &now);
      int64_t ns = (int64_t)(now.tv_sec - ta5_last_present.tv_sec) * 1000000000 +
                   (now.tv_nsec - ta5_last_present.tv_nsec);
      if (ns < 16666666) {
         struct timespec delay;
         delay.tv_sec = 0;
         delay.tv_nsec = 16666666 - ns;
         nanosleep(&delay, NULL);
         clock_gettime(CLOCK_MONOTONIC, &ta5_last_present);
      } else {
         ta5_last_present = now;
      }
   }

   return VK_SUCCESS;
}

#endif /* TA5_HAVE_WAYLAND */