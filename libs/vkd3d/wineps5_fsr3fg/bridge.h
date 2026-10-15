/* SPDX-License-Identifier: GPL-3.0-or-later
 * ps5-framegen 64362101757cdd6f7df9eeb984cb53f03f1fbfca C adapter. */
#ifndef WINEPS5_FSR3_BRIDGE_H
#define WINEPS5_FSR3_BRIDGE_H
#include <vulkan/vulkan_core.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct wineps5_fsr3;
struct wineps5_fsr3 *wineps5_fsr3_create(VkPhysicalDevice physical, VkDevice device,
        PFN_vkGetDeviceProcAddr get_proc, PFN_vkGetPhysicalDeviceMemoryProperties get_memory,
        PFN_vkGetPhysicalDeviceProperties get_properties, uint32_t width, uint32_t height,
        VkFormat format, char *error, unsigned int error_size);
void wineps5_fsr3_destroy(struct wineps5_fsr3 *fg);
void wineps5_fsr3_prepare(struct wineps5_fsr3 *fg, VkCommandBuffer cmd);
VkImage wineps5_fsr3_input(struct wineps5_fsr3 *fg);
VkImageView wineps5_fsr3_input_view(struct wineps5_fsr3 *fg);
VkImage wineps5_fsr3_output(struct wineps5_fsr3 *fg);
void wineps5_fsr3_record(struct wineps5_fsr3 *fg, VkCommandBuffer cmd, unsigned int generations);
void wineps5_fsr3_generate(struct wineps5_fsr3 *fg, VkCommandBuffer cmd, unsigned int generation);
#ifdef __cplusplus
}
#endif
#endif
