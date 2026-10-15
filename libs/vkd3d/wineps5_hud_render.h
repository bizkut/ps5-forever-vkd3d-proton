/* Included by swapchain.c after dxgi_vk_swap_chain: present-thread owned HUD. */
static void dxgi_vk_swap_chain_hud_cleanup(struct dxgi_vk_swap_chain *chain)
{
    struct wineps5_hud *h = &chain->present.hud;
    struct d3d12_device *device = chain->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    if (h->mapped) VK_CALL(vkUnmapMemory(device->vk_device, h->memory));
    VK_CALL(vkDestroyBuffer(device->vk_device, h->buffer, NULL));
    VK_CALL(vkFreeMemory(device->vk_device, h->memory, NULL));
    VK_CALL(vkDestroyPipeline(device->vk_device, h->pipeline, NULL));
    VK_CALL(vkDestroyPipelineLayout(device->vk_device, h->layout, NULL));
    h->mapped = NULL; h->buffer = VK_NULL_HANDLE; h->memory = VK_NULL_HANDLE;
    h->pipeline = VK_NULL_HANDLE; h->layout = VK_NULL_HANDLE;
}

static bool dxgi_vk_swap_chain_hud_init(struct dxgi_vk_swap_chain *chain)
{
    struct wineps5_hud *h = &chain->present.hud;
    struct d3d12_device *device = chain->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkDevice vk_device = device->vk_device;
    VkBufferCreateInfo buffer = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkBindBufferMemoryInfo bind = { .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO };
    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties properties;
    VkPipelineLayoutCreateInfo layout = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
    VkShaderModuleCreateInfo module = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule modules[2] = {0};
    VkPipelineShaderStageCreateInfo stages[2] = {{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }};
    VkVertexInputBindingDescription binding = {0, sizeof(struct wineps5_hud_quad), VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct wineps5_hud_quad, rect)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(struct wineps5_hud_quad, color)},
        {2, 0, VK_FORMAT_R32G32_UINT, offsetof(struct wineps5_hud_quad, mask)}};
    VkPipelineVertexInputStateCreateInfo vertex = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo viewport = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo samples = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend_attachment = { .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo blend = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &blend_attachment };
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dynamic_states };
    VkPipelineRenderingCreateInfo rendering = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1, .pColorAttachmentFormats = &chain->present.backbuffer_format };
    VkGraphicsPipelineCreateInfo pipeline = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &rendering, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
            .pInputAssemblyState = &assembly, .pViewportState = &viewport, .pRasterizationState = &raster,
            .pMultisampleState = &samples, .pColorBlendState = &blend, .pDynamicState = &dynamic };
    unsigned int i;
    if (h->pipeline && h->format == chain->present.backbuffer_format) return true;
    if (h->pipeline) dxgi_vk_swap_chain_hud_cleanup(chain);
    buffer.size = sizeof(struct wineps5_hud_quad) * WINEPS5_HUD_QUADS * DXGI_MAX_SWAP_CHAIN_BUFFERS;
    buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (VK_CALL(vkCreateBuffer(vk_device, &buffer, NULL, &h->buffer)) < 0) goto fail;
    VK_CALL(vkGetBufferMemoryRequirements(vk_device, h->buffer, &requirements));
    device->vkd3d_instance->vk_procs.vkGetPhysicalDeviceMemoryProperties(device->vk_physical_device, &properties);
    for (i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    if (i == properties.memoryTypeCount) goto fail;
    alloc.allocationSize = requirements.size; alloc.memoryTypeIndex = i;
    if (VK_CALL(vkAllocateMemory(vk_device, &alloc, NULL, &h->memory)) < 0) goto fail;
    bind.buffer = h->buffer; bind.memory = h->memory;
    if (VK_CALL(vkBindBufferMemory2(vk_device, 1, &bind)) < 0 ||
            VK_CALL(vkMapMemory(vk_device, h->memory, 0, VK_WHOLE_SIZE, 0, (void **)&h->mapped)) < 0) goto fail;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    if (VK_CALL(vkCreatePipelineLayout(vk_device, &layout, NULL, &h->layout)) < 0) goto fail;
    module.pCode = vs_wineps5_hud; module.codeSize = sizeof(vs_wineps5_hud);
    if (VK_CALL(vkCreateShaderModule(vk_device, &module, NULL, &modules[0])) < 0) goto fail;
    module.pCode = fs_wineps5_hud; module.codeSize = sizeof(fs_wineps5_hud);
    if (VK_CALL(vkCreateShaderModule(vk_device, &module, NULL, &modules[1])) < 0) goto fail;
    for (i = 0; i < 2; ++i) { stages[i].module = modules[i]; stages[i].pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    vertex.vertexBindingDescriptionCount = 1; vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = 3; vertex.pVertexAttributeDescriptions = attributes;
    pipeline.layout = h->layout;
    if (vkd3d_ps5_create_graphics_pipeline(device, VK_NULL_HANDLE, &pipeline, &h->pipeline, false, false, NULL) < 0) goto fail;
    for (i = 0; i < 2; ++i) VK_CALL(vkDestroyShaderModule(vk_device, modules[i], NULL));
    h->format = chain->present.backbuffer_format;
    return true;
fail:
    for (i = 0; i < 2; ++i) VK_CALL(vkDestroyShaderModule(vk_device, modules[i], NULL));
    dxgi_vk_swap_chain_hud_cleanup(chain);
    ERR("WINEPS5 vkd3d hud resource initialization failed\n");
    return false;
}

static void dxgi_vk_swap_chain_hud_update(struct dxgi_vk_swap_chain *chain)
{
    struct wineps5_hud *h = &chain->present.hud;
    struct wineps5_hud_settings settings = {false, 0, 2};
    const char *positions[] = {"tl", "tr", "bl", "br"};
    uint64_t now = vkd3d_get_current_time_ns();
    char text[128];
    size_t size;
    FILE *file;
    if (!chain->present.fg.running) wineps5_hud_note(&h->timing, now);
    if (now < h->next_check_ns) return;
    h->next_check_ns = now + UINT64_C(1000000000);
    if ((file = fopen("C:\\wineps5-hud.txt", "rb")))
    {
        size = fread(text, 1, sizeof(text), file);
        if (!ferror(file) && size < sizeof(text) && !memchr(text, 0, size))
        {
            text[size] = 0;
            if (!wineps5_hud_parse(text, &settings)) settings = (struct wineps5_hud_settings){false, 0, 2};
        }
        fclose(file);
    }
    if (!h->initialized || settings.enabled != h->settings.enabled || settings.pos != h->settings.pos || settings.scale != h->settings.scale)
        ERR("WINEPS5 vkd3d hud enabled=%u pos=%s scale=%u source=file\n", settings.enabled, positions[settings.pos], settings.scale);
    h->settings = settings; h->initialized = true;
    if (settings.enabled)
    {
        if (chain->present.fg.running) pthread_mutex_lock(&chain->present.fg.lock);
        h->metrics = wineps5_hud_measure(&h->timing);
        h->output_metrics = chain->present.fg.running ? wineps5_hud_measure(&h->output_timing) : h->metrics;
        if (chain->present.fg.running) pthread_mutex_unlock(&chain->present.fg.lock);
    }
}

static void wineps5_hud_add(struct wineps5_hud_quad *q, unsigned int *count,
        float x, float y, float w, float height, float white, float alpha, uint64_t mask)
{
    struct wineps5_hud_quad *p;
    if (*count >= WINEPS5_HUD_QUADS) return;
    p = &q[(*count)++];
    p->rect[0] = x; p->rect[1] = y; p->rect[2] = w; p->rect[3] = height;
    p->color[0] = p->color[1] = p->color[2] = white; p->color[3] = alpha;
    p->mask[0] = mask; p->mask[1] = mask >> 32;
}

static void dxgi_vk_swap_chain_hud_draw(struct dxgi_vk_swap_chain *chain, VkCommandBuffer cmd,
        unsigned int index, const VkRenderingInfo *base)
{
    struct wineps5_hud *h = &chain->present.hud;
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    const struct wineps5_hud_metrics *metrics = &h->metrics;
    struct vkd3d_ps5_gpu_snapshot gpu;
    struct wineps5_hud_quad *q;
    VkRenderingInfo rendering = *base;
    VkRenderingAttachmentInfo attachment = *base->pColorAttachments;
    float extent[2] = {chain->present.backbuffer_width, chain->present.backbuffer_height};
    VkViewport viewport = {0, 0, extent[0], extent[1], 0, 1};
    VkDeviceSize offset = index * WINEPS5_HUD_QUADS * sizeof(*q);
    unsigned int count = 0, i, row = 0, col = 0, s = h->settings.scale;
    bool hdr = chain->present.backbuffer_color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    /* PQ(203 nits), precomputed to avoid transcendental math per present. */
    float white = hdr ? 0.58068888f : 1.0f;
    float width = 252 * s, height = 144 * s;
    float x = (h->settings.pos & 1) ? fmaxf(0, extent[0] - width - 12 * s) : 12 * s;
    float y = (h->settings.pos & 2) ? fmaxf(0, extent[1] - height - 12 * s) : 12 * s;
    const char *mode;
    const char *fg_mode = chain->present.fg.settings.automatic ?
            (chain->present.fg.running ? "AUTO ON" : "AUTO OFF") : (chain->present.fg.running ? "ON" : "OFF");
    /* the method running, or the one asked for (AUTO) before an engine exists */
    enum wineps5_fg_method fg_method = chain->present.fg.engine ?
            wineps5_fg_engine_method(chain->present.fg.engine) : chain->present.fg.settings.method;
    char text[320], display_latency[32];
    if (!dxgi_vk_swap_chain_hud_init(chain)) return;
    q = h->mapped + index * WINEPS5_HUD_QUADS;
    vkd3d_ps5_gpu_census_snapshot(&gpu);
    if (gpu.display_latency_samples)
        snprintf(display_latency,sizeof(display_latency),"%.1f MS",(double)gpu.display_latency_ms_mean);
    else
        snprintf(display_latency,sizeof(display_latency),"N/A");
    switch (chain->present.selected_present_mode)
    {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: mode = "IMMEDIATE"; break;
        case VK_PRESENT_MODE_MAILBOX_KHR: mode = "MAILBOX"; break;
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: mode = "FIFO RELAXED"; break;
        case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR: mode = "FIFO LATEST"; break;
        default: mode = "FIFO"; break;
    }
    snprintf(text, sizeof(text), "FPS %.1f  GAME %.1f\nFRAME %.2f MS  1%% LOW %.1f\nGPU DIRECT %.1f%%\nLATENCY WAIT %.1f MS / %.1f S\n%u X %u  %s\nHDR %s  DISPLAY %s\nSUBMIT %.1f MS\nFG %s BETWEEN %s", h->output_metrics.fps,
            metrics->fps, metrics->mean_ms,
            metrics->low_fps, (double)gpu.direct_busy_pct, (double)gpu.latency_wait_ms, (double)gpu.window_s,
            chain->present.backbuffer_width, chain->present.backbuffer_height, mode, hdr ? "ON" : "OFF",
            display_latency,
            (double)gpu.present_latency_ms_mean,
            fg_method == WINEPS5_FG_METHOD_FSR3 ? "FSR3" : fg_method == WINEPS5_FG_METHOD_AUTO ? "AUTO" : "LOSSLESS",
            fg_mode);
    wineps5_hud_add(q, &count, x, y, width, height, 0, 0.72f, UINT64_MAX);
    for (i = 0; text[i]; ++i)
    {
        if (text[i] == '\n') { ++row; col = 0; continue; }
        if (text[i] != ' ') wineps5_hud_add(q, &count, x + (6 + col * 6) * s,
                y + (6 + row * 10) * s, 5 * s, 7 * s, white, 1, wineps5_hud_glyph(text[i]));
        ++col;
    }
    /* 120 samples, fixed 0..50 ms vertical scale, oldest at left. */
    if (chain->present.fg.running) pthread_mutex_lock(&chain->present.fg.lock);
    for (i = 0; i < 120 && i + 1 < h->timing.count; ++i)
    {
        float ms = wineps5_hud_interval(&h->timing, i) * 1e-6;
        float bar = fminf(ms / 50.0f, 1.0f) * 36 * s;
        wineps5_hud_add(q, &count, x + (6 + (119 - i) * 2) * s, y + 138 * s - bar,
                s, fmaxf(bar, 1), white, 1, UINT64_MAX);
    }
    if (chain->present.fg.running) pthread_mutex_unlock(&chain->present.fg.lock);
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    rendering.pColorAttachments = &attachment;
    VK_CALL(vkCmdBeginRendering(cmd, &rendering));
    VK_CALL(vkCmdSetViewport(cmd, 0, 1, &viewport));
    VK_CALL(vkCmdSetScissor(cmd, 0, 1, &rendering.renderArea));
    VK_CALL(vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, h->pipeline));
    VK_CALL(vkCmdBindVertexBuffers(cmd, 0, 1, &h->buffer, &offset));
    VK_CALL(vkCmdPushConstants(cmd, h->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(extent), extent));
    VK_CALL(vkCmdDraw(cmd, 6, count, 0, 0));
    VK_CALL(vkCmdEndRendering(cmd));
}
