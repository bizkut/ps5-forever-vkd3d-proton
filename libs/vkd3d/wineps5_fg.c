/* SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
 * SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Eden 67bada77 mapping (all source files under src/video_core):
 * frame_gen/lossless_dll.cpp ReadShaderResources -> fg_load_modules
 * frame_gen/lsfg_translate.cpp AdoptSpirvModule -> wineps5_fg_adopt (fg.h)
 * renderer_vulkan/present/lsfg_common.cpp LsfgImage/Pass/Resources/Barriers
 *   -> wineps5_fg_image_create / fg_pipeline / fg_constants / fg_dispatch
 * present/lsfg_{mipmaps,alpha,beta,gamma,delta,generate}.cpp constructors
 *   -> fg_build_chain; Dispatch methods -> record_shared/record_generation
 * present/lsfg_chain.cpp -> wineps5_fg_engine (same seven-level stage graph)
 * present/frame_gen.cpp -> present-worker glue in wineps5_fg_present.h
 * present/frame_gen_pacer.cpp -> wineps5_fg_plan (fixed-multiplier mode).
 * ProsperoEden tools/prepare-vulkan-port.py:915-1000 -> feature/storage gates
 * and explicit status markers. No proprietary DLL/shader bytes are embedded.
 * FSR3 backend: swordpdf/ps5-framegen
 * 64362101757cdd6f7df9eeb984cb53f03f1fbfca, as vendored by
 * PSSwanStation ps5-port f05cd30550ab. GPL-3.0-or-later host; MIT AMD
 * FidelityFX SDK 2.3.0 shaders, with notices/rebuild sources in wineps5_fsr3fg.
 */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d_private.h"
#include "wineps5_fg_vk.h"
#include "wineps5_fsr3fg/bridge.h"
#include <stdio.h>

#define FG_LEVELS 7
#define FG_BINDINGS 12
#define FG_IMAGES 256
#define FG_STAGES 128
struct fg_pipeline
{
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    unsigned int bindings;
    VkDescriptorType types[FG_BINDINGS];
};
struct fg_binding
{
    VkDescriptorType type;
    struct wineps5_fg_image *images[3];
    VkSampler sampler;
    unsigned int constants_kind;
};
struct fg_stage
{
    unsigned int shader, slots, histories, shift;
    VkExtent2D extent;
    VkDescriptorSet sets[6][3];
    struct fg_binding bindings[FG_BINDINGS];
};
struct fg_alpha { struct wineps5_fg_image *a, *b, *c[2], *out[3][2]; };
struct fg_motion { struct wineps5_fg_image *a[3], *b[2], *out[2]; };
struct wineps5_fg_engine
{
    struct d3d12_device *device;
    struct wineps5_fsr3 *fsr3;
    struct wineps5_fg_image fsr3_input, fsr3_output;
    VkDescriptorPool pool;
    struct fg_pipeline pipelines[25];
    VkSampler samplers[3];
    VkBuffer constants;
    VkDeviceMemory constants_memory;
    VkDeviceSize constants_stride;
    struct wineps5_fg_image images[FG_IMAGES];
    unsigned int image_count, stage_count;
    struct fg_stage stages[FG_STAGES];
    struct wineps5_fg_image *frames[2], *output, *mips[7];
    struct fg_alpha alpha[7];
    struct fg_motion gamma[7], delta[3];
    struct wineps5_fg_image *beta_a[2], *beta_b[2], *flow[6];
    struct fg_stage *mipmap, *alpha_stage[7][4], *beta_stage[5], *gamma_stage[7][5], *delta_stage[3][10], *generate;
    float flow_scale;
};
struct fg_constants
{
    uint32_t input_offset[2], first_iter, first_iter_s, advanced_color_kind, hdr_support;
    float resolution_inv_scale, timestamp, ui_threshold;
    uint32_t padding[3];
};
_Static_assert(sizeof(struct fg_constants) == 48, "Lossless uniform ABI");

static bool fg_allocate(struct d3d12_device *device, const VkMemoryRequirements *r,
        VkMemoryPropertyFlags flags, VkDeviceMemory *memory)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r->size};
    unsigned int i;
    for (i = 0; i < device->memory_properties.memoryTypeCount; ++i)
        if ((r->memoryTypeBits & (1u << i)) &&
                (device->memory_properties.memoryTypes[i].propertyFlags & flags) == flags) break;
    if (i == device->memory_properties.memoryTypeCount) return false;
    alloc.memoryTypeIndex = i;
    return VK_CALL(vkAllocateMemory(device->vk_device, &alloc, NULL, memory)) == VK_SUCCESS;
}
bool wineps5_fg_image_create(struct d3d12_device *device, VkExtent2D extent, VkFormat format,
        struct wineps5_fg_image *image)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkBindImageMemoryInfo bind = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    VkMemoryRequirements requirements;
    image->extent.width = extent.width ? extent.width : 1;
    image->extent.height = extent.height ? extent.height : 1;
    ci.extent = (VkExtent3D){image->extent.width, image->extent.height, 1};
    if (VK_CALL(vkCreateImage(device->vk_device, &ci, NULL, &image->image)) < 0) return false;
    VK_CALL(vkGetImageMemoryRequirements(device->vk_device, image->image, &requirements));
    if (!fg_allocate(device, &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image->memory)) goto fail;
    bind.image = image->image; bind.memory = image->memory;
    if (VK_CALL(vkBindImageMemory2(device->vk_device, 1, &bind)) < 0) goto fail;
    view.image = image->image;
    if (VK_CALL(vkCreateImageView(device->vk_device, &view, NULL, &image->view)) < 0) goto fail;
    return true;
fail:
    wineps5_fg_image_destroy(device, image); return false;
}
void wineps5_fg_image_destroy(struct d3d12_device *device, struct wineps5_fg_image *image)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VK_CALL(vkDestroyImageView(device->vk_device, image->view, NULL));
    VK_CALL(vkDestroyImage(device->vk_device, image->image, NULL));
    VK_CALL(vkFreeMemory(device->vk_device, image->memory, NULL));
    memset(image, 0, sizeof(*image));
}
static struct wineps5_fg_image *fg_image(struct wineps5_fg_engine *e, VkExtent2D extent, VkFormat format)
{
    struct wineps5_fg_image *image;
    if (e->image_count == FG_IMAGES) return NULL;
    image = &e->images[e->image_count++];
    return wineps5_fg_image_create(e->device, extent, format, image) ? image : NULL;
}
static VkImageMemoryBarrier2 fg_barrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
        VkAccessFlags2 src, VkAccessFlags2 dst)
{
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return b;
}
void wineps5_fg_copy(struct d3d12_device *device, VkCommandBuffer cmd, VkImage source,
        VkImageLayout source_layout, struct wineps5_fg_image *destination)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkImageMemoryBarrier2 b[2];
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = b};
    VkImageCopy2 copy = {.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {destination->extent.width, destination->extent.height, 1}};
    VkCopyImageInfo2 copy_info = {.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
        .srcImage = source, .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = destination->image, .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1, .pRegions = &copy};
    b[0] = fg_barrier(source, source_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    b[1] = fg_barrier(destination->image, destination->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            destination->layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VK_CALL(vkCmdPipelineBarrier2(cmd, &dependency));
    VK_CALL(vkCmdCopyImage2(cmd, &copy_info));
    b[0] = fg_barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_layout,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    b[1] = fg_barrier(destination->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    VK_CALL(vkCmdPipelineBarrier2(cmd, &dependency));
    destination->layout = VK_IMAGE_LAYOUT_GENERAL;
}

static const char *fg_load_modules(struct wineps5_fg_engine *e, VkShaderModule modules[25])
{
    const struct vkd3d_vk_device_procs *vk_procs = &e->device->vk_procs;
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    struct wineps5_fg_blob image, resource;
    const char *status = "UnreadableFile";
    unsigned char *bytes = NULL;
    uint32_t *words = NULL;
    long size;
    unsigned int i;
    FILE *file = fopen("C:\\Lossless.dll", "rb");
    if (!file) return "NotInstalled";
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) goto done;
    if (!(bytes = vkd3d_malloc(size))) { status = "OutOfMemory"; goto done; }
    if (fread(bytes, 1, size, file) != (size_t)size) goto done;
    image = (struct wineps5_fg_blob){bytes, (size_t)size};
    for (i = 0; i < 25; ++i)
    {
        if (!wineps5_fg_resource(image, wineps5_fg_shader_id(i), &resource)) { status = "MissingShaders"; goto done; }
        if (resource.size % 4) { status = "TranslationFailed"; goto done; }
        if (!(words = vkd3d_malloc(resource.size))) { status = "OutOfMemory"; goto done; }
        memcpy(words, resource.data, resource.size);
        if (!wineps5_fg_adopt(words, resource.size / 4)) { status = "TranslationFailed"; goto done; }
        ci.codeSize = resource.size; ci.pCode = words;
        if (VK_CALL(vkCreateShaderModule(e->device->vk_device, &ci, NULL, &modules[i])) < 0)
        { status = "ShaderModuleFailed"; goto done; }
        vkd3d_free(words); words = NULL;
    }
    status = "Ok";
done:
    vkd3d_free(words); vkd3d_free(bytes); fclose(file); return status;
}
static bool fg_constants_init(struct wineps5_fg_engine *e)
{
    const struct vkd3d_vk_device_procs *vk_procs = &e->device->vk_procs;
    VkDevice vk_device = e->device->vk_device;
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBindBufferMemoryInfo bind = {.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};
    VkMemoryRequirements r;
    unsigned char *mapped;
    unsigned int i, j;
    struct fg_constants c = {.ui_threshold = 0.5f, .resolution_inv_scale = 1.0f / e->flow_scale};
    e->constants_stride = align64(sizeof(struct fg_constants),
            max((VkDeviceSize)1,e->device->device_info.properties2.properties.limits.minUniformBufferOffsetAlignment));
    ci.size = 20 * e->constants_stride;
    if (VK_CALL(vkCreateBuffer(vk_device, &ci, NULL, &e->constants)) < 0) return false;
    VK_CALL(vkGetBufferMemoryRequirements(vk_device, e->constants, &r));
    if (!fg_allocate(e->device, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &e->constants_memory)) return false;
    bind.buffer = e->constants; bind.memory = e->constants_memory;
    if (VK_CALL(vkBindBufferMemory2(vk_device, 1, &bind)) < 0 ||
            VK_CALL(vkMapMemory(vk_device, e->constants_memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped)) < 0) return false;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 6; ++j)
        {
            c.first_iter = i == 1; c.first_iter_s = i == 2;
            c.timestamp = wineps5_fg_timestamp(j);
            memcpy(mapped + (i * 6 + j) * e->constants_stride, &c, sizeof(c));
        }
    c.first_iter = c.first_iter_s = 0; c.timestamp = 0;
    memcpy(mapped + 18 * e->constants_stride, &c, sizeof(c));
    c.timestamp = 0.5f; memcpy(mapped + 19 * e->constants_stride, &c, sizeof(c));
    VK_CALL(vkUnmapMemory(vk_device, e->constants_memory));
    return true;
}
static struct fg_binding fg_sample(struct wineps5_fg_image *image)
{
    struct fg_binding b = {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .images = {image, image, image}}; return b;
}
static struct fg_binding fg_store(struct wineps5_fg_image *image)
{
    struct fg_binding b = fg_sample(image); b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; return b;
}
static struct fg_binding fg_history(struct wineps5_fg_image *images[3][2], unsigned int component, unsigned int offset)
{
    struct fg_binding b = {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    unsigned int i;
    for (i = 0; i < 3; ++i) b.images[i] = images[(i + offset) % 3][component];
    return b;
}
static struct fg_binding fg_history_store(struct wineps5_fg_image *images[3][2], unsigned int component)
{
    struct fg_binding b = fg_history(images, component, 0); b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; return b;
}
static struct fg_binding fg_sampler(struct wineps5_fg_engine *e, unsigned int index)
{
    struct fg_binding b = {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .sampler = e->samplers[index]}; return b;
}
static struct fg_binding fg_uniform(unsigned int kind)
{
    struct fg_binding b = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .constants_kind = kind}; return b;
}
static struct fg_binding fg_frame(struct wineps5_fg_engine *e, unsigned int offset)
{
    struct fg_binding b = {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    unsigned int i;
    for (i = 0; i < 3; ++i) b.images[i] = e->frames[(i + offset) % 2];
    return b;
}
static bool fg_pipeline_init(struct wineps5_fg_engine *e, unsigned int index,
        VkShaderModule module, const struct fg_binding *bindings, unsigned int count)
{
    const struct vkd3d_vk_device_procs *vk_procs = &e->device->vk_procs;
    struct fg_pipeline *p = &e->pipelines[index];
    VkDescriptorSetLayoutBinding b[FG_BINDINGS];
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = count, .pBindings = b};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &p->set_layout};
    VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = module, .pName = "main"}};
    unsigned int i;
    if (p->pipeline) return true;
    p->bindings = count;
    for (i = 0; i < count; ++i)
    {
        p->types[i] = bindings[i].type;
        b[i] = (VkDescriptorSetLayoutBinding){i, bindings[i].type, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    }
    if (VK_CALL(vkCreateDescriptorSetLayout(e->device->vk_device, &ci, NULL, &p->set_layout)) < 0 ||
            VK_CALL(vkCreatePipelineLayout(e->device->vk_device, &layout, NULL, &p->layout)) < 0) return false;
    pipeline.layout = p->layout;
    return vkd3d_ps5_create_compute_pipeline(e->device, VK_NULL_HANDLE, &pipeline, &p->pipeline, false, false, NULL) == VK_SUCCESS;
}
static struct fg_stage *fg_stage(struct wineps5_fg_engine *e, VkShaderModule modules[25], unsigned int id,
        VkExtent2D extent, unsigned int shift, unsigned int slots, unsigned int histories,
        const struct fg_binding *bindings, unsigned int count)
{
    const struct vkd3d_vk_device_procs *vk_procs = &e->device->vk_procs;
    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = e->pool, .descriptorSetCount = 1};
    VkWriteDescriptorSet writes[FG_BINDINGS];
    VkDescriptorImageInfo images[FG_BINDINGS];
    VkDescriptorBufferInfo buffers[FG_BINDINGS];
    struct fg_pipeline *p;
    struct fg_stage *stage;
    unsigned int i, s, h, index = id < 280 ? id - 255 : id - 278;
    if (e->stage_count == FG_STAGES || !fg_pipeline_init(e, index, modules[index], bindings, count)) return NULL;
    p = &e->pipelines[index];
    stage = &e->stages[e->stage_count++];
    stage->shader = index; stage->extent = extent; stage->shift = shift; stage->slots = slots; stage->histories = histories;
    memcpy(stage->bindings, bindings, count * sizeof(*bindings));
    alloc.pSetLayouts = &p->set_layout;
    for (s = 0; s < slots; ++s)
        for (h = 0; h < histories; ++h)
        {
            if (VK_CALL(vkAllocateDescriptorSets(e->device->vk_device, &alloc, &stage->sets[s][h])) < 0) return NULL;
            for (i = 0; i < count; ++i)
            {
                writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = stage->sets[s][h], .dstBinding = i, .descriptorCount = 1, .descriptorType = bindings[i].type};
                if (bindings[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                {
                    unsigned int kind = bindings[i].constants_kind;
                    buffers[i] = (VkDescriptorBufferInfo){e->constants, (kind < 3 ? kind * 6 + s : kind == 3 ? 18 : 19) * e->constants_stride, 48};
                    writes[i].pBufferInfo = &buffers[i];
                }
                else
                {
                    struct wineps5_fg_image *image = bindings[i].images[h];
                    images[i] = (VkDescriptorImageInfo){bindings[i].sampler, image ? image->view : VK_NULL_HANDLE,
                        image ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED};
                    writes[i].pImageInfo = &images[i];
                }
            }
            VK_CALL(vkUpdateDescriptorSets(e->device->vk_device, count, writes, 0, NULL));
        }
    return stage;
}
#define S(n) fg_sampler(e, n)
#define U(n) fg_uniform(n)
#define I(i) fg_sample(i)
#define O(i) fg_store(i)
#define H(i,c,o) fg_history(i,c,o)
#define W(i,c) fg_history_store(i,c)
#define STAGE(dst,id,extent,shift,slots,hist,...) do { \
    struct fg_binding args[] = {__VA_ARGS__}; \
    if (!((dst) = fg_stage(e, modules, id, extent, shift, slots, hist, args, ARRAY_SIZE(args)))) return false; \
} while (0)
#define IMAGE(dst,extent,format) do { if (!((dst) = fg_image(e, extent, format))) return false; } while (0)
static VkExtent2D fg_half(VkExtent2D x) { return (VkExtent2D){(x.width + 1) / 2, (x.height + 1) / 2}; }
static bool fg_build_chain(struct wineps5_fg_engine *e, VkShaderModule modules[25], VkExtent2D extent, VkFormat format)
{
    VkExtent2D flow_extent = {(uint32_t)(extent.width * e->flow_scale), (uint32_t)(extent.height * e->flow_scale)}, x, half, quarter;
    unsigned int i, j, h, level;
    const unsigned int alpha_ids[4] = {290,291,292,293}, beta_ids[5] = {298,299,300,301,302};
    const unsigned int gamma_ids[5] = {280,282,283,284,285}, delta_ids[10] = {280,286,287,288,289,281,294,295,296,297};
    struct wineps5_fg_image *pg, *pd1, *pd2;
    for (i = 0; i < 2; ++i) IMAGE(e->frames[i], extent, format);
    IMAGE(e->output, extent, format);
    for (i = 0; i < 7; ++i)
    {
        x = (VkExtent2D){flow_extent.width >> i, flow_extent.height >> i};
        IMAGE(e->mips[i], x, VK_FORMAT_R8_UNORM);
        half = fg_half(e->mips[i]->extent); quarter = fg_half(half);
        IMAGE(e->alpha[i].a, half, VK_FORMAT_R8G8B8A8_UNORM);
        IMAGE(e->alpha[i].b, half, VK_FORMAT_R8G8B8A8_UNORM);
        for (j = 0; j < 2; ++j)
        {
            IMAGE(e->alpha[i].c[j], quarter, VK_FORMAT_R8G8B8A8_UNORM);
            for (h = 0; h < 3; ++h) IMAGE(e->alpha[i].out[h][j], quarter, VK_FORMAT_R8G8B8A8_UNORM);
        }
        STAGE(e->alpha_stage[i][0],alpha_ids[0],half,3,1,1,S(0),I(e->mips[i]),O(e->alpha[i].a));
        STAGE(e->alpha_stage[i][1],alpha_ids[1],half,3,1,1,S(0),I(e->alpha[i].a),O(e->alpha[i].b));
        STAGE(e->alpha_stage[i][2],alpha_ids[2],quarter,3,1,1,S(0),I(e->alpha[i].b),O(e->alpha[i].c[0]),O(e->alpha[i].c[1]));
        STAGE(e->alpha_stage[i][3],alpha_ids[3],quarter,3,1,3,S(0),I(e->alpha[i].c[0]),I(e->alpha[i].c[1]),W(e->alpha[i].out,0),W(e->alpha[i].out,1));
    }
    STAGE(e->mipmap,255,flow_extent,6,1,2,U(3),S(0),fg_frame(e,0),O(e->mips[0]),O(e->mips[1]),O(e->mips[2]),O(e->mips[3]),O(e->mips[4]),O(e->mips[5]),O(e->mips[6]));
    x = e->alpha[0].out[0][0]->extent;
    for (i = 0; i < 2; ++i)
    { IMAGE(e->beta_a[i],x,VK_FORMAT_R8G8B8A8_UNORM); IMAGE(e->beta_b[i],x,VK_FORMAT_R8G8B8A8_UNORM); }
    for (i = 0; i < 6; ++i) IMAGE(e->flow[i],((VkExtent2D){x.width >> i,x.height >> i}),VK_FORMAT_R8_UNORM);
    STAGE(e->beta_stage[0],beta_ids[0],x,3,1,3,S(1),H(e->alpha[0].out,0,1),H(e->alpha[0].out,1,1),H(e->alpha[0].out,0,2),H(e->alpha[0].out,1,2),H(e->alpha[0].out,0,0),H(e->alpha[0].out,1,0),O(e->beta_a[0]),O(e->beta_a[1]));
    for (i = 1; i < 4; ++i)
    {
        struct wineps5_fg_image **a = i & 1 ? e->beta_a : e->beta_b, **b = i & 1 ? e->beta_b : e->beta_a;
        STAGE(e->beta_stage[i],beta_ids[i],x,3,1,1,S(0),I(a[0]),I(a[1]),O(b[0]),O(b[1]));
    }
    STAGE(e->beta_stage[4],beta_ids[4],x,5,1,1,U(4),S(0),I(e->beta_b[0]),I(e->beta_b[1]),O(e->flow[0]),O(e->flow[1]),O(e->flow[2]),O(e->flow[3]),O(e->flow[4]),O(e->flow[5]));
    for (i = 0; i < 7; ++i)
    {
        struct fg_motion *m = &e->gamma[i];
        level = 6 - i; x = e->alpha[level].out[0][0]->extent;
        pg = i ? e->gamma[i - 1].out[0] : NULL;
        for (j = 0; j < 3; ++j) IMAGE(m->a[j],x,VK_FORMAT_R8G8B8A8_UNORM);
        for (j = 0; j < 2; ++j) IMAGE(m->b[j],x,VK_FORMAT_R8G8B8A8_UNORM);
        IMAGE(m->out[0],x,VK_FORMAT_R16G16B16A16_SFLOAT);
        STAGE(e->gamma_stage[i][0],gamma_ids[0],x,3,6,3,U(i ? 0 : 1),S(1),S(2),H(e->alpha[level].out,0,2),H(e->alpha[level].out,1,2),H(e->alpha[level].out,0,0),H(e->alpha[level].out,1,0),I(pg),O(m->a[0]),O(m->a[1]),O(m->a[2]));
        STAGE(e->gamma_stage[i][1],gamma_ids[1],x,3,1,1,S(0),I(m->a[0]),I(m->a[1]),I(m->a[2]),O(m->b[0]),O(m->b[1]));
        STAGE(e->gamma_stage[i][2],gamma_ids[2],x,3,1,1,S(0),I(m->b[0]),I(m->b[1]),O(m->a[0]),O(m->a[1]));
        STAGE(e->gamma_stage[i][3],gamma_ids[3],x,3,1,1,S(0),I(m->a[0]),I(m->a[1]),O(m->b[0]),O(m->b[1]));
        STAGE(e->gamma_stage[i][4],gamma_ids[4],x,3,6,1,U(i ? 0 : 1),S(0),S(2),I(m->b[0]),I(m->b[1]),I(pg),I(e->flow[level < 6 ? level : 5]),O(m->out[0]));
        if (i < 4) continue;
        j = i - 4; m = &e->delta[j];
        pg = j ? e->gamma[i - 1].out[0] : NULL;
        pd1 = j ? e->delta[j - 1].out[0] : NULL; pd2 = j ? e->delta[j - 1].out[1] : NULL;
        for (h = 0; h < 3; ++h) IMAGE(m->a[h],x,VK_FORMAT_R8G8B8A8_UNORM);
        for (h = 0; h < 2; ++h) { IMAGE(m->b[h],x,VK_FORMAT_R8G8B8A8_UNORM); IMAGE(m->out[h],x,VK_FORMAT_R16G16B16A16_SFLOAT); }
        STAGE(e->delta_stage[j][0],delta_ids[0],x,3,6,3,U(j ? 0 : 2),S(1),S(2),H(e->alpha[level].out,0,2),H(e->alpha[level].out,1,2),H(e->alpha[level].out,0,0),H(e->alpha[level].out,1,0),I(pg),O(m->a[0]),O(m->a[1]),O(m->a[2]));
        STAGE(e->delta_stage[j][1],delta_ids[1],x,3,1,1,S(0),I(m->a[0]),I(m->a[1]),I(m->a[2]),O(m->b[0]),O(m->b[1]));
        STAGE(e->delta_stage[j][2],delta_ids[2],x,3,1,1,S(0),I(m->b[0]),I(m->b[1]),O(m->a[0]),O(m->a[1]));
        STAGE(e->delta_stage[j][3],delta_ids[3],x,3,1,1,S(0),I(m->a[0]),I(m->a[1]),O(m->b[0]),O(m->b[1]));
        STAGE(e->delta_stage[j][4],delta_ids[4],x,3,6,1,U(j ? 0 : 2),S(0),S(2),I(m->b[0]),I(m->b[1]),I(pg),I(e->flow[level]),O(m->out[0]));
        STAGE(e->delta_stage[j][5],delta_ids[5],x,3,6,3,U(j ? 0 : 2),S(1),S(2),H(e->alpha[level].out,0,2),H(e->alpha[level].out,1,2),H(e->alpha[level].out,0,0),H(e->alpha[level].out,1,0),I(pg),I(pd1),O(m->b[0]));
        STAGE(e->delta_stage[j][6],delta_ids[6],x,3,1,1,S(0),I(m->b[0]),O(m->a[0]));
        STAGE(e->delta_stage[j][7],delta_ids[7],x,3,1,1,S(0),I(m->a[0]),O(m->b[0]));
        STAGE(e->delta_stage[j][8],delta_ids[8],x,3,1,1,S(0),I(m->b[0]),O(m->a[0]));
        STAGE(e->delta_stage[j][9],delta_ids[9],x,3,6,1,U(j ? 0 : 2),S(0),S(2),I(m->a[0]),I(pd2),O(m->out[1]));
    }
    /* One output image suffices: the worker completes/recycles each generation
     * before recording the next, exactly like Eden's reused motion temporaries. */
    STAGE(e->generate,256,extent,4,6,2,U(0),S(0),S(2),fg_frame(e,1),fg_frame(e,0),I(e->gamma[6].out[0]),I(e->delta[2].out[0]),I(e->delta[2].out[1]),O(e->output));
    return true;
}
#undef STAGE
#undef IMAGE
#undef S
#undef U
#undef I
#undef O
#undef H
#undef W

const char *wineps5_fg_create(struct d3d12_device *device, VkExtent2D extent, VkFormat format,
        const struct wineps5_fg_settings *settings, struct wineps5_fg_engine **engine)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4096},{VK_DESCRIPTOR_TYPE_SAMPLER,4096},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,8192},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,8192}};
    VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2048, .poolSizeCount = ARRAY_SIZE(sizes), .pPoolSizes = sizes};
    VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .maxLod = VK_LOD_CLAMP_NONE};
    VkFormatProperties properties;
    VkFormat formats[] = {format,VK_FORMAT_R8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT};
    VkShaderModule modules[25] = {0};
    struct wineps5_fg_engine *e;
    const char *status;
    unsigned int i;
    *engine = NULL;
    if (settings->method == WINEPS5_FG_METHOD_AUTO)
    {
        /* Lossless when the user's DLL gives its shaders; FSR3 needs no file */
        struct wineps5_fg_settings chosen = *settings;
        chosen.method = WINEPS5_FG_METHOD_LOSSLESS;
        status = wineps5_fg_create(device, extent, format, &chosen, engine);
        if (strcmp(status, "NotInstalled") && strcmp(status, "UnreadableFile") &&
                strcmp(status, "MissingShaders") && strcmp(status, "TranslationFailed") &&
                strcmp(status, "ShaderModuleFailed"))
            return status;
        ERR("WINEPS5 vkd3d fg method=auto lossless=%s using=fsr3\n", status);
        chosen.method = WINEPS5_FG_METHOD_FSR3;
        return wineps5_fg_create(device, extent, format, &chosen, engine);
    }
    if (settings->method == WINEPS5_FG_METHOD_FSR3)
    {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        char detail[256];
        VkFormat work_formats[] = {VK_FORMAT_R8_UINT,VK_FORMAT_R16G16_SINT,
            VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R32_UINT,VK_FORMAT_R8G8B8A8_UNORM};
        VkFormatFeatureFlags required;
        if (!device->device_info.features2.features.shaderStorageImageExtendedFormats) return "DeviceUnsupported";
        device->vkd3d_instance->vk_procs.vkGetPhysicalDeviceFormatProperties(device->vk_physical_device,format,&properties);
        required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((properties.optimalTilingFeatures & required) != required) return "DeviceUnsupported";
        for (i = 0; i < ARRAY_SIZE(work_formats); ++i)
        {
            required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if (work_formats[i] == VK_FORMAT_R32_UINT) required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
            if (work_formats[i] == VK_FORMAT_R8G8B8A8_UNORM)
                required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            device->vkd3d_instance->vk_procs.vkGetPhysicalDeviceFormatProperties(device->vk_physical_device,work_formats[i],&properties);
            if ((properties.optimalTilingFeatures & required) != required) return "DeviceUnsupported";
        }
        if (!(e = vkd3d_calloc(1, sizeof(*e)))) return "OutOfMemory";
        e->device = device;
        e->fsr3 = wineps5_fsr3_create(device->vk_physical_device,device->vk_device,
                device->vkd3d_instance->vk_procs.vkGetDeviceProcAddr,
                device->vkd3d_instance->vk_procs.vkGetPhysicalDeviceMemoryProperties,
                device->vkd3d_instance->vk_procs.vkGetPhysicalDeviceProperties,
                extent.width,extent.height,format,detail,sizeof(detail));
        if (!e->fsr3)
        {
            ERR("WINEPS5 vkd3d fg method=fsr3 detail=%s\n",detail);
            vkd3d_free(e); return "ResourceCreationFailed";
        }
        e->fsr3_input.extent = e->fsr3_output.extent = extent;
        e->fsr3_input.layout = e->fsr3_output.layout = VK_IMAGE_LAYOUT_GENERAL;
        e->fsr3_output.image = wineps5_fsr3_output(e->fsr3);
        view.image = e->fsr3_output.image;
        if (VK_CALL(vkCreateImageView(device->vk_device,&view,NULL,&e->fsr3_output.view)) < 0)
        {
            wineps5_fsr3_destroy(e->fsr3); vkd3d_free(e); return "ResourceCreationFailed";
        }
        *engine = e; return "Ok";
    }
    if (!device->device_info.vulkan_1_2_features.vulkanMemoryModel ||
            !device->device_info.vulkan_1_2_features.shaderFloat16 ||
            !device->device_info.robustness2_features.nullDescriptor) return "DeviceUnsupported";
    for (i = 0; i < ARRAY_SIZE(formats); ++i)
    {
        device->vkd3d_instance->vk_procs.vkGetPhysicalDeviceFormatProperties(device->vk_physical_device, formats[i], &properties);
        if ((properties.optimalTilingFeatures & (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) !=
                (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) return "DeviceUnsupported";
    }
    if (!(e = vkd3d_calloc(1, sizeof(*e)))) return "OutOfMemory";
    e->device = device; e->flow_scale = settings->flow;
    status = fg_load_modules(e, modules);
    if (strcmp(status, "Ok")) goto done;
    status = "ResourceCreationFailed";
    if (VK_CALL(vkCreateDescriptorPool(device->vk_device, &pool, NULL, &e->pool)) < 0 || !fg_constants_init(e)) goto done;
    for (i = 0; i < 3; ++i)
    {
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = i == 2 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler.borderColor = i == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        sampler.compareOp = i == 2 ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_NEVER;
        if (VK_CALL(vkCreateSampler(device->vk_device, &sampler, NULL, &e->samplers[i])) < 0) goto done;
    }
    if (!fg_build_chain(e, modules, extent, format)) goto done;
    *engine = e; status = "Ok";
done:
    for (i = 0; i < 25; ++i) VK_CALL(vkDestroyShaderModule(device->vk_device, modules[i], NULL));
    if (!*engine) wineps5_fg_destroy(e);
    return status;
}
enum wineps5_fg_method wineps5_fg_engine_method(const struct wineps5_fg_engine *e)
{
    return e->fsr3 ? WINEPS5_FG_METHOD_FSR3 : WINEPS5_FG_METHOD_LOSSLESS;
}
void wineps5_fg_destroy(struct wineps5_fg_engine *e)
{
    const struct vkd3d_vk_device_procs *vk_procs;
    VkDevice device;
    unsigned int i;
    if (!e) return;
    vk_procs = &e->device->vk_procs; device = e->device->vk_device;
    if (e->fsr3)
    {
        VK_CALL(vkDestroyImageView(device,e->fsr3_output.view,NULL));
        wineps5_fsr3_destroy(e->fsr3); vkd3d_free(e); return;
    }
    VK_CALL(vkDestroyDescriptorPool(device,e->pool,NULL));
    for (i = 0; i < 25; ++i)
    {
        VK_CALL(vkDestroyPipeline(device,e->pipelines[i].pipeline,NULL));
        VK_CALL(vkDestroyPipelineLayout(device,e->pipelines[i].layout,NULL));
        VK_CALL(vkDestroyDescriptorSetLayout(device,e->pipelines[i].set_layout,NULL));
    }
    for (i = 0; i < 3; ++i) VK_CALL(vkDestroySampler(device,e->samplers[i],NULL));
    VK_CALL(vkDestroyBuffer(device,e->constants,NULL));
    VK_CALL(vkFreeMemory(device,e->constants_memory,NULL));
    for (i = 0; i < e->image_count; ++i) wineps5_fg_image_destroy(e->device,&e->images[i]);
    vkd3d_free(e);
}
static void fg_dispatch(struct wineps5_fg_engine *e, struct fg_stage *stage, VkCommandBuffer cmd,
        uint64_t count, unsigned int slot)
{
    const struct vkd3d_vk_device_procs *vk_procs = &e->device->vk_procs;
    struct fg_pipeline *p = &e->pipelines[stage->shader];
    unsigned int h = count % stage->histories, i, n = 0;
    VkImageMemoryBarrier2 barriers[FG_BINDINGS];
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pImageMemoryBarriers = barriers};
    for (i = 0; i < p->bindings; ++i)
    {
        struct wineps5_fg_image *image = stage->bindings[i].images[h];
        if (!image) continue;
        barriers[n++] = fg_barrier(image->image, image->layout, VK_IMAGE_LAYOUT_GENERAL,
                image->layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                stage->bindings[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT);
        image->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    dependency.imageMemoryBarrierCount = n;
    VK_CALL(vkCmdPipelineBarrier2(cmd, &dependency));
    VK_CALL(vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->pipeline));
    VK_CALL(vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->layout,0,1,
            &stage->sets[slot % stage->slots][h],0,NULL));
    VK_CALL(vkCmdDispatch(cmd,(stage->extent.width + (1u << stage->shift) - 1) >> stage->shift,
            (stage->extent.height + (1u << stage->shift) - 1) >> stage->shift,1));
}
const struct wineps5_fg_image *wineps5_fg_input(struct wineps5_fg_engine *e, uint64_t count)
{ return e->fsr3 ? &e->fsr3_input : e->frames[count % 2]; }
const struct wineps5_fg_image *wineps5_fg_output(struct wineps5_fg_engine *e, unsigned int generation)
{ (void)generation; return e->fsr3 ? &e->fsr3_output : e->output; }
void wineps5_fg_record_shared(struct wineps5_fg_engine *e, VkCommandBuffer cmd,
        const struct wineps5_fg_image *source, uint64_t count, unsigned int generations)
{
    unsigned int i, stage;
    if (e->fsr3)
    {
        wineps5_fsr3_prepare(e->fsr3,cmd);
        e->fsr3_input.image = wineps5_fsr3_input(e->fsr3);
        e->fsr3_input.view = wineps5_fsr3_input_view(e->fsr3);
        wineps5_fg_copy(e->device,cmd,source->image,VK_IMAGE_LAYOUT_GENERAL,&e->fsr3_input);
        wineps5_fsr3_record(e->fsr3,cmd,generations);
        return;
    }
    wineps5_fg_copy(e->device,cmd,source->image,VK_IMAGE_LAYOUT_GENERAL,e->frames[count % 2]);
    fg_dispatch(e,e->mipmap,cmd,count,0);
    for (stage = 0; stage < 4; ++stage)
        for (i = 0; i < 7; ++i) fg_dispatch(e,e->alpha_stage[i][stage],cmd,count,0);
    /* Beta's recurrence needs all three alpha history slots initialized. */
    if (count < 2) return;
    for (stage = 0; stage < 5; ++stage) fg_dispatch(e,e->beta_stage[stage],cmd,count,0);
}
void wineps5_fg_record_generation(struct wineps5_fg_engine *e, VkCommandBuffer cmd,
        uint64_t count, unsigned int generations, unsigned int generation)
{
    const struct vkd3d_vk_device_procs *vk_procs = &e->device->vk_procs;
    unsigned int i, stage, slot = wineps5_fg_slot(generations,generation);
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    if (e->fsr3)
    {
        wineps5_fsr3_generate(e->fsr3,cmd,generation);
        return;
    }
    for (i = 0; i < 7; ++i)
    {
        for (stage = 0; stage < 5; ++stage) fg_dispatch(e,e->gamma_stage[i][stage],cmd,count,slot);
        if (i >= 4) for (stage = 0; stage < 10; ++stage) fg_dispatch(e,e->delta_stage[i - 4][stage],cmd,count,slot);
    }
    fg_dispatch(e,e->generate,cmd,count,slot);
    VK_CALL(vkCmdPipelineBarrier2(cmd, &dependency));
}
