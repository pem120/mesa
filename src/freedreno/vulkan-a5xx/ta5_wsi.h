/*
 * Window system integration: public surface for the a5xx Vulkan driver.
 *
 * Copyright © 2026 Adreno 5xx Vulkan driver authors
 * SPDX-License-Identifier: MIT
 *
 * What used to be here is gone: struct ta5_surface, struct ta5_swapchain and
 * the wayland forward declarations that went with them.  Surfaces, swapchains,
 * acquire and present are libvulkan_wsi's now, reached through the common
 * entrypoints, exactly as turnip does it -- see ta5_wsi.cc for why.
 *
 * ta5_wsi_init() / ta5_wsi_finish() are declared in ta5_instance.h, next to
 * the struct wsi_device they operate on.
 */

#ifndef TA5_WSI_H
#define TA5_WSI_H

#include <stdint.h>

#include <vulkan/vulkan.h>

struct ta5_physical_device;

#endif /* TA5_WSI_H */