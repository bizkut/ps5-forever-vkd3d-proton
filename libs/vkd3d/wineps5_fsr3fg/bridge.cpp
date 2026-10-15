/* SPDX-License-Identifier: GPL-3.0-or-later
 * Thin adapter; shader bytes are unchanged. Host changes are documented in F1.
 * Each call binds this engine's dispatch table, never another VkDevice/queue. */
#define VK_NO_PROTOTYPES
#include "bridge.h"
#include <cstdio>
#include <new>
struct fsr3_dispatch
{
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkBindBufferMemory vkBindBufferMemory;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdClearColorImage vkCmdClearColorImage;
    PFN_vkCmdDispatch vkCmdDispatch;
    PFN_vkCmdFillBuffer vkCmdFillBuffer;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkCreateComputePipelines vkCreateComputePipelines;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkCreateImage vkCreateImage;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkCreateQueryPool vkCreateQueryPool;
    PFN_vkCreateSampler vkCreateSampler;
    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkDestroyImageView vkDestroyImageView;
    PFN_vkDestroyPipeline vkDestroyPipeline;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkDestroyQueryPool vkDestroyQueryPool;
    PFN_vkDestroySampler vkDestroySampler;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkUnmapMemory vkUnmapMemory;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
};
static thread_local fsr3_dispatch *fsr3_procs;
#define vkAllocateDescriptorSets fsr3_procs->vkAllocateDescriptorSets
#define vkAllocateMemory fsr3_procs->vkAllocateMemory
#define vkBindBufferMemory fsr3_procs->vkBindBufferMemory
#define vkBindImageMemory fsr3_procs->vkBindImageMemory
#define vkCmdBindDescriptorSets fsr3_procs->vkCmdBindDescriptorSets
#define vkCmdBindPipeline fsr3_procs->vkCmdBindPipeline
#define vkCmdClearColorImage fsr3_procs->vkCmdClearColorImage
#define vkCmdDispatch fsr3_procs->vkCmdDispatch
#define vkCmdFillBuffer fsr3_procs->vkCmdFillBuffer
#define vkCmdPipelineBarrier fsr3_procs->vkCmdPipelineBarrier
#define vkCmdResetQueryPool fsr3_procs->vkCmdResetQueryPool
#define vkCreateBuffer fsr3_procs->vkCreateBuffer
#define vkCreateComputePipelines fsr3_procs->vkCreateComputePipelines
#define vkCreateDescriptorPool fsr3_procs->vkCreateDescriptorPool
#define vkCreateDescriptorSetLayout fsr3_procs->vkCreateDescriptorSetLayout
#define vkCreateImage fsr3_procs->vkCreateImage
#define vkCreateImageView fsr3_procs->vkCreateImageView
#define vkCreatePipelineLayout fsr3_procs->vkCreatePipelineLayout
#define vkCreateQueryPool fsr3_procs->vkCreateQueryPool
#define vkCreateSampler fsr3_procs->vkCreateSampler
#define vkCreateShaderModule fsr3_procs->vkCreateShaderModule
#define vkDestroyBuffer fsr3_procs->vkDestroyBuffer
#define vkDestroyDescriptorPool fsr3_procs->vkDestroyDescriptorPool
#define vkDestroyDescriptorSetLayout fsr3_procs->vkDestroyDescriptorSetLayout
#define vkDestroyImage fsr3_procs->vkDestroyImage
#define vkDestroyImageView fsr3_procs->vkDestroyImageView
#define vkDestroyPipeline fsr3_procs->vkDestroyPipeline
#define vkDestroyPipelineLayout fsr3_procs->vkDestroyPipelineLayout
#define vkDestroyQueryPool fsr3_procs->vkDestroyQueryPool
#define vkDestroySampler fsr3_procs->vkDestroySampler
#define vkDestroyShaderModule fsr3_procs->vkDestroyShaderModule
#define vkFreeMemory fsr3_procs->vkFreeMemory
#define vkGetBufferMemoryRequirements fsr3_procs->vkGetBufferMemoryRequirements
#define vkGetDeviceProcAddr fsr3_procs->vkGetDeviceProcAddr
#define vkGetImageMemoryRequirements fsr3_procs->vkGetImageMemoryRequirements
#define vkGetPhysicalDeviceMemoryProperties fsr3_procs->vkGetPhysicalDeviceMemoryProperties
#define vkGetPhysicalDeviceProperties fsr3_procs->vkGetPhysicalDeviceProperties
#define vkGetQueryPoolResults fsr3_procs->vkGetQueryPoolResults
#define vkMapMemory fsr3_procs->vkMapMemory
#define vkUnmapMemory fsr3_procs->vkUnmapMemory
#define vkUpdateDescriptorSets fsr3_procs->vkUpdateDescriptorSets
#include "ps5_framegen.cpp"
struct wineps5_fsr3
{
    fsr3_dispatch procs{};
    ps5::framegen::interpolator interpolator;
};
class dispatch_scope
{
    fsr3_dispatch *previous;
public:
    explicit dispatch_scope(wineps5_fsr3 *fg) : previous(fsr3_procs) { fsr3_procs = &fg->procs; }
    ~dispatch_scope() { fsr3_procs = previous; }
};
#undef vkAllocateDescriptorSets
#undef vkAllocateMemory
#undef vkBindBufferMemory
#undef vkBindImageMemory
#undef vkCmdBindDescriptorSets
#undef vkCmdBindPipeline
#undef vkCmdClearColorImage
#undef vkCmdDispatch
#undef vkCmdFillBuffer
#undef vkCmdPipelineBarrier
#undef vkCmdResetQueryPool
#undef vkCreateBuffer
#undef vkCreateComputePipelines
#undef vkCreateDescriptorPool
#undef vkCreateDescriptorSetLayout
#undef vkCreateImage
#undef vkCreateImageView
#undef vkCreatePipelineLayout
#undef vkCreateQueryPool
#undef vkCreateSampler
#undef vkCreateShaderModule
#undef vkDestroyBuffer
#undef vkDestroyDescriptorPool
#undef vkDestroyDescriptorSetLayout
#undef vkDestroyImage
#undef vkDestroyImageView
#undef vkDestroyPipeline
#undef vkDestroyPipelineLayout
#undef vkDestroyQueryPool
#undef vkDestroySampler
#undef vkDestroyShaderModule
#undef vkFreeMemory
#undef vkGetBufferMemoryRequirements
#undef vkGetDeviceProcAddr
#undef vkGetImageMemoryRequirements
#undef vkGetPhysicalDeviceMemoryProperties
#undef vkGetPhysicalDeviceProperties
#undef vkGetQueryPoolResults
#undef vkMapMemory
#undef vkUnmapMemory
#undef vkUpdateDescriptorSets
extern "C" struct wineps5_fsr3 *wineps5_fsr3_create(VkPhysicalDevice physical, VkDevice device,
        PFN_vkGetDeviceProcAddr get_proc, PFN_vkGetPhysicalDeviceMemoryProperties get_memory,
        PFN_vkGetPhysicalDeviceProperties get_properties, uint32_t width, uint32_t height,
        VkFormat format, char *error, unsigned int error_size)
{
    auto *fg = new (std::nothrow) wineps5_fsr3;
    if (!fg) { std::snprintf(error, error_size, "OutOfMemory"); return nullptr; }
    fg->procs.vkGetDeviceProcAddr = get_proc;
    fg->procs.vkGetPhysicalDeviceMemoryProperties = get_memory;
    fg->procs.vkGetPhysicalDeviceProperties = get_properties;
    fg->procs.vkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(get_proc(device, "vkAllocateDescriptorSets"));
    fg->procs.vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(get_proc(device, "vkAllocateMemory"));
    fg->procs.vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(get_proc(device, "vkBindBufferMemory"));
    fg->procs.vkBindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(get_proc(device, "vkBindImageMemory"));
    fg->procs.vkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(get_proc(device, "vkCmdBindDescriptorSets"));
    fg->procs.vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(get_proc(device, "vkCmdBindPipeline"));
    fg->procs.vkCmdClearColorImage = reinterpret_cast<PFN_vkCmdClearColorImage>(get_proc(device, "vkCmdClearColorImage"));
    fg->procs.vkCmdDispatch = reinterpret_cast<PFN_vkCmdDispatch>(get_proc(device, "vkCmdDispatch"));
    fg->procs.vkCmdFillBuffer = reinterpret_cast<PFN_vkCmdFillBuffer>(get_proc(device, "vkCmdFillBuffer"));
    fg->procs.vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get_proc(device, "vkCmdPipelineBarrier"));
    fg->procs.vkCmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(get_proc(device, "vkCmdResetQueryPool"));
    fg->procs.vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(get_proc(device, "vkCreateBuffer"));
    fg->procs.vkCreateComputePipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(get_proc(device, "vkCreateComputePipelines"));
    fg->procs.vkCreateDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(get_proc(device, "vkCreateDescriptorPool"));
    fg->procs.vkCreateDescriptorSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(get_proc(device, "vkCreateDescriptorSetLayout"));
    fg->procs.vkCreateImage = reinterpret_cast<PFN_vkCreateImage>(get_proc(device, "vkCreateImage"));
    fg->procs.vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(get_proc(device, "vkCreateImageView"));
    fg->procs.vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(get_proc(device, "vkCreatePipelineLayout"));
    fg->procs.vkCreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(get_proc(device, "vkCreateQueryPool"));
    fg->procs.vkCreateSampler = reinterpret_cast<PFN_vkCreateSampler>(get_proc(device, "vkCreateSampler"));
    fg->procs.vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(get_proc(device, "vkCreateShaderModule"));
    fg->procs.vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(get_proc(device, "vkDestroyBuffer"));
    fg->procs.vkDestroyDescriptorPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(get_proc(device, "vkDestroyDescriptorPool"));
    fg->procs.vkDestroyDescriptorSetLayout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(get_proc(device, "vkDestroyDescriptorSetLayout"));
    fg->procs.vkDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(get_proc(device, "vkDestroyImage"));
    fg->procs.vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(get_proc(device, "vkDestroyImageView"));
    fg->procs.vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(get_proc(device, "vkDestroyPipeline"));
    fg->procs.vkDestroyPipelineLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(get_proc(device, "vkDestroyPipelineLayout"));
    fg->procs.vkDestroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(get_proc(device, "vkDestroyQueryPool"));
    fg->procs.vkDestroySampler = reinterpret_cast<PFN_vkDestroySampler>(get_proc(device, "vkDestroySampler"));
    fg->procs.vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(get_proc(device, "vkDestroyShaderModule"));
    fg->procs.vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(get_proc(device, "vkFreeMemory"));
    fg->procs.vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_proc(device, "vkGetBufferMemoryRequirements"));
    fg->procs.vkGetImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(get_proc(device, "vkGetImageMemoryRequirements"));
    fg->procs.vkGetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(get_proc(device, "vkGetQueryPoolResults"));
    fg->procs.vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(get_proc(device, "vkMapMemory"));
    fg->procs.vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(get_proc(device, "vkUnmapMemory"));
    fg->procs.vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(get_proc(device, "vkUpdateDescriptorSets"));
    if (!fg->procs.vkAllocateDescriptorSets ||
            !fg->procs.vkAllocateMemory ||
            !fg->procs.vkBindBufferMemory ||
            !fg->procs.vkBindImageMemory ||
            !fg->procs.vkCmdBindDescriptorSets ||
            !fg->procs.vkCmdBindPipeline ||
            !fg->procs.vkCmdClearColorImage ||
            !fg->procs.vkCmdDispatch ||
            !fg->procs.vkCmdFillBuffer ||
            !fg->procs.vkCmdPipelineBarrier ||
            !fg->procs.vkCmdResetQueryPool ||
            !fg->procs.vkCreateBuffer ||
            !fg->procs.vkCreateComputePipelines ||
            !fg->procs.vkCreateDescriptorPool ||
            !fg->procs.vkCreateDescriptorSetLayout ||
            !fg->procs.vkCreateImage ||
            !fg->procs.vkCreateImageView ||
            !fg->procs.vkCreatePipelineLayout ||
            !fg->procs.vkCreateQueryPool ||
            !fg->procs.vkCreateSampler ||
            !fg->procs.vkCreateShaderModule ||
            !fg->procs.vkDestroyBuffer ||
            !fg->procs.vkDestroyDescriptorPool ||
            !fg->procs.vkDestroyDescriptorSetLayout ||
            !fg->procs.vkDestroyImage ||
            !fg->procs.vkDestroyImageView ||
            !fg->procs.vkDestroyPipeline ||
            !fg->procs.vkDestroyPipelineLayout ||
            !fg->procs.vkDestroyQueryPool ||
            !fg->procs.vkDestroySampler ||
            !fg->procs.vkDestroyShaderModule ||
            !fg->procs.vkFreeMemory ||
            !fg->procs.vkGetBufferMemoryRequirements ||
            !fg->procs.vkGetDeviceProcAddr ||
            !fg->procs.vkGetImageMemoryRequirements ||
            !fg->procs.vkGetPhysicalDeviceMemoryProperties ||
            !fg->procs.vkGetPhysicalDeviceProperties ||
            !fg->procs.vkGetQueryPoolResults ||
            !fg->procs.vkMapMemory ||
            !fg->procs.vkUnmapMemory ||
            !fg->procs.vkUpdateDescriptorSets)
    {
        std::snprintf(error, error_size, "DeviceUnsupported");
        delete fg;
        return nullptr;
    }
    dispatch_scope scope(fg);
    try
    {
        std::string detail;
        if (!fg->interpolator.create(physical, device, width, height, detail, format))
        {
            std::snprintf(error, error_size, "%s", detail.c_str());
            delete fg;
            return nullptr;
        }
    }
    catch (const std::bad_alloc &)
    {
        std::snprintf(error, error_size, "OutOfMemory");
        delete fg;
        return nullptr;
    }
    return fg;
}
extern "C" void wineps5_fsr3_destroy(wineps5_fsr3 *fg)
{
    if (!fg) return;
    dispatch_scope scope(fg);
    delete fg;
}
extern "C" void wineps5_fsr3_prepare(wineps5_fsr3 *fg, VkCommandBuffer cmd)
{
    dispatch_scope scope(fg);
    fg->interpolator.prepare(cmd);
}
extern "C" VkImage wineps5_fsr3_input(wineps5_fsr3 *fg) { return fg->interpolator.frame_image(); }
extern "C" VkImageView wineps5_fsr3_input_view(wineps5_fsr3 *fg) { return fg->interpolator.frame_view(); }
extern "C" VkImage wineps5_fsr3_output(wineps5_fsr3 *fg) { return fg->interpolator.output_image(); }
extern "C" void wineps5_fsr3_record(wineps5_fsr3 *fg, VkCommandBuffer cmd, unsigned int generations)
{
    dispatch_scope scope(fg);
    fg->interpolator.set_generated(generations);
    fg->interpolator.record(cmd, fg->interpolator.frames() == 0);
    /* Keep history current even if the pacer cancels later outputs. The
     * interpolator remembers record()'s parity for interpolate_next(). */
    fg->interpolator.advance();
}
extern "C" void wineps5_fsr3_generate(wineps5_fsr3 *fg, VkCommandBuffer cmd, unsigned int generation)
{
    dispatch_scope scope(fg);
    if (generation)
        fg->interpolator.interpolate_next(cmd, generation);
}
