/* SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
 * SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Eden 67bada77 renderer_vulkan/present/lsfg_{common,chain}.h C port. */
#ifndef WINEPS5_FG_VK_H
#define WINEPS5_FG_VK_H
#include "wineps5_fg.h"
struct wineps5_fg_image
{
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkExtent2D extent;
    VkImageLayout layout;
};
struct wineps5_fg_engine;
const char *wineps5_fg_create(struct d3d12_device *device, VkExtent2D extent, VkFormat format,
        const struct wineps5_fg_settings *settings, struct wineps5_fg_engine **engine);
void wineps5_fg_destroy(struct wineps5_fg_engine *engine);
/* the method an engine runs: settings may ask for WINEPS5_FG_METHOD_AUTO */
enum wineps5_fg_method wineps5_fg_engine_method(const struct wineps5_fg_engine *engine);
const struct wineps5_fg_image *wineps5_fg_input(struct wineps5_fg_engine *engine, uint64_t count);
const struct wineps5_fg_image *wineps5_fg_output(struct wineps5_fg_engine *engine, unsigned int generation);
bool wineps5_fg_image_create(struct d3d12_device *device, VkExtent2D extent, VkFormat format,
        struct wineps5_fg_image *image);
void wineps5_fg_image_destroy(struct d3d12_device *device, struct wineps5_fg_image *image);
void wineps5_fg_copy(struct d3d12_device *device, VkCommandBuffer cmd, VkImage source,
        VkImageLayout source_layout, struct wineps5_fg_image *destination);
void wineps5_fg_record_shared(struct wineps5_fg_engine *engine, VkCommandBuffer cmd,
        const struct wineps5_fg_image *source, uint64_t count, unsigned int generations);
void wineps5_fg_record_generation(struct wineps5_fg_engine *engine, VkCommandBuffer cmd,
        uint64_t count, unsigned int generations, unsigned int generation);
#endif
