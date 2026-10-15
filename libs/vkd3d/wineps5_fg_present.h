/* SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
 * SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Eden 67bada77 present/{frame_gen,frame_gen_pacer}.cpp C/WSI port.
 * Included after the existing present_iteration, which remains the sole WSI
 * implementation. The queue callback captures and signals game latency;
 * this worker alone performs generation, output pacing, HDR/HUD and presents. */
/* the method the engine runs, or the one asked for (maybe auto) before one exists */
static inline const char *dxgi_vk_swap_chain_fg_method_name(const struct wineps5_fg_present *f)
{
    return wineps5_fg_method_name(f->engine ? wineps5_fg_engine_method(f->engine) : f->settings.method);
}
static void dxgi_vk_swap_chain_fg_timing(struct dxgi_vk_swap_chain *chain, uint64_t begin_ns)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    uint64_t ticks[2], mask;
    double ms;
    if (f->timestamps && VK_CALL(vkGetQueryPoolResults(chain->queue->device->vk_device,
            f->queries,0,2,sizeof(ticks),ticks,sizeof(ticks[0]),VK_QUERY_RESULT_64_BIT)) == VK_SUCCESS)
    {
        unsigned int bits = chain->queue->vkd3d_queue->timestamp_bits;
        mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
        ms = ((ticks[1] - ticks[0]) & mask) *
                chain->queue->device->device_info.properties2.properties.limits.timestampPeriod * 1e-6;
    }
    else
    {
        ms = (vkd3d_get_current_time_ns() - begin_ns) * 1e-6;
        if (f->timestamps) ERR("WINEPS5 vkd3d fg timing=cpu_submit_to_complete reason=timestamp_unavailable\n");
        f->timestamps = false;
    }
    f->gpu_sum_ms += ms; f->gpu_max_ms = fmax(f->gpu_max_ms,ms); ++f->timing_count;
}
static void dxgi_vk_swap_chain_fg_note_game_block(struct dxgi_vk_swap_chain *chain, uint64_t ns)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    if (!vkd3d_atomic_uint32_load_explicit(&f->metrics_active,vkd3d_memory_order_acquire)) return;
    spinlock_acquire(&f->block_stats_lock);
    wineps5_fg_block_note(&f->game_block,ns);
    spinlock_release(&f->block_stats_lock);
}
static void dxgi_vk_swap_chain_fg_note_callback_block(struct dxgi_vk_swap_chain *chain, uint64_t begin_ns)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    uint64_t ns;
    if (!begin_ns || !vkd3d_atomic_uint32_load_explicit(&f->metrics_active,vkd3d_memory_order_acquire)) return;
    ns = vkd3d_get_current_time_ns() - begin_ns;
    spinlock_acquire(&f->block_stats_lock);
    wineps5_fg_block_note(&f->callback_block,ns);
    spinlock_release(&f->block_stats_lock);
}
static void dxgi_vk_swap_chain_fg_capture_lock(struct wineps5_fg_present *f, uint64_t *wait_ns)
{
    uint64_t begin = vkd3d_get_current_time_ns();
    pthread_mutex_lock(&f->lock);
    *wait_ns += vkd3d_get_current_time_ns() - begin;
}
static void dxgi_vk_swap_chain_fg_note_capture_block(struct wineps5_fg_present *f,
        uint64_t lock_ns, uint64_t queue_ns, uint64_t submit_ns)
{
    spinlock_acquire(&f->block_stats_lock);
    wineps5_fg_block_note(&f->capture_lock_wait,lock_ns);
    wineps5_fg_block_note(&f->capture_queue_wait,queue_ns);
    wineps5_fg_block_note(&f->capture_submit,submit_ns);
    spinlock_release(&f->block_stats_lock);
}
static void dxgi_vk_swap_chain_fg_metrics(struct dxgi_vk_swap_chain *chain)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    uint64_t now = vkd3d_get_current_time_ns();
    struct wineps5_fg_block_stats game, callback, lock_wait, queue_wait, submit;
    char line[2048];
    if (!f->window_start_ns) f->window_start_ns = now;
    if (now - f->window_start_ns < UINT64_C(10000000000)) return;
    spinlock_acquire(&f->block_stats_lock);
    game = f->game_block; callback = f->callback_block; lock_wait = f->capture_lock_wait;
    queue_wait = f->capture_queue_wait; submit = f->capture_submit;
    f->game_block = f->callback_block = f->capture_lock_wait = f->capture_queue_wait = f->capture_submit = (struct wineps5_fg_block_stats){0};
    spinlock_release(&f->block_stats_lock);
    pthread_mutex_lock(&f->lock);
    snprintf(line,sizeof(line),"WINEPS5 vkd3d fg window_s=10 method=%s mode=between real=%"PRIu64" generated=%"PRIu64" gpu_ms_mean=%.3f gpu_ms_max=%.3f timing=%s cancelled_by_newer_real=%"PRIu64" deadline_missed=%"PRIu64" ring_full=%"PRIu64" worker_busy=%"PRIu64" pacer_skipped=%"PRIu64" present_failed=%"PRIu64" prefetch_deadline_skipped=%"PRIu64" prefetch_deadline_overrun=%"PRIu64" real_interval_ms_mean=%.3f generated_offset_ms_mean=%.3f acquire_ms_mean=%.3f acquire_ms_max=%.3f deadline_late_ms_mean=%.3f input_ready_age_ms_mean=%.3f game_present=%"PRIu64" game_block_ms_mean=%.3f game_block_ms_max=%.3f callback_block_ms_mean=%.3f callback_block_ms_max=%.3f capture_lock_ms_mean=%.3f capture_lock_ms_max=%.3f capture_queue_ms_mean=%.3f capture_queue_ms_max=%.3f capture_submit_ms_mean=%.3f capture_submit_ms_max=%.3f\n",
            dxgi_vk_swap_chain_fg_method_name(f),
            f->real_count,f->generated_count,f->timing_count ? f->gpu_sum_ms / f->timing_count : 0,
            f->gpu_max_ms,f->timestamps ? "timestamp" : "cpu_submit_to_complete",
            f->cancelled_by_newer_real,f->deadline_missed,f->ring_full,f->worker_busy,f->pacer_skipped,f->present_failed,
            f->prefetch_deadline_skipped,f->prefetch_deadline_overrun,
            f->interval_samples ? f->interval_sum_ms / f->interval_samples : 0,
            f->offset_samples ? f->deadline_offset_sum_ms / f->offset_samples : 0,
            f->acquire_samples ? f->acquire_sum_ns * 1e-6 / f->acquire_samples : 0,f->acquire_max_ns * 1e-6,
            f->deadline_late_samples ? f->deadline_late_sum_ms / f->deadline_late_samples : 0,
            f->input_ready_samples ? f->input_ready_age_sum_ms / f->input_ready_samples : 0,
            game.count,wineps5_fg_block_mean_ms(&game),game.max_ns * 1e-6,
            wineps5_fg_block_mean_ms(&callback),callback.max_ns * 1e-6,
            wineps5_fg_block_mean_ms(&lock_wait),lock_wait.max_ns * 1e-6,
            wineps5_fg_block_mean_ms(&queue_wait),queue_wait.max_ns * 1e-6,
            wineps5_fg_block_mean_ms(&submit),submit.max_ns * 1e-6);
    f->real_count = f->generated_count = f->timing_count = 0;
    f->gpu_sum_ms = f->gpu_max_ms = 0; f->window_start_ns = now;
    f->cancelled_by_newer_real = f->deadline_missed = f->ring_full = f->worker_busy = f->pacer_skipped = f->present_failed = 0;
    f->prefetch_deadline_skipped = f->prefetch_deadline_overrun = f->shared_submit_budget_ns = 0;
    f->interval_samples = f->offset_samples = f->acquire_samples = f->acquire_sum_ns = f->acquire_max_ns = 0;
    f->interval_sum_ms = f->deadline_offset_sum_ms = 0;
    f->input_ready_samples = f->deadline_late_samples = 0;
    f->deadline_late_sum_ms = f->input_ready_age_sum_ms = 0;
    pthread_mutex_unlock(&f->lock);
    /* Logging may block on console stderr; never retain the capture mutex. */
    ERR("%s",line);
}
static bool dxgi_vk_swap_chain_fg_submit_work(struct dxgi_vk_swap_chain *chain,
        const struct wineps5_fg_image *source, uint64_t count)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    struct d3d12_device *device = chain->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkCommandBufferSubmitInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = f->work_command};
    VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1, .pCommandBufferInfos = &command};
    uint64_t record_begin_ns = source ? vkd3d_get_current_time_ns() : 0, cost;
    VkQueue queue;
    VkResult vr;
    VK_CALL(vkResetFences(device->vk_device,1,&f->work_fence));
    VK_CALL(vkResetCommandBuffer(f->work_command,0));
    if (VK_CALL(vkBeginCommandBuffer(f->work_command,&begin)) < 0) return false;
    if (f->timestamps)
    {
        VK_CALL(vkCmdResetQueryPool(f->work_command,f->queries,0,2));
        VK_CALL(vkCmdWriteTimestamp2(f->work_command,VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,f->queries,0));
    }
    if (source)
        wineps5_fg_record_shared(f->engine,f->work_command,source,count,f->generations);
    if (!source || f->generations)
        wineps5_fg_record_generation(f->engine,f->work_command,count,f->generations,f->generation);
    if (f->timestamps) VK_CALL(vkCmdWriteTimestamp2(f->work_command,VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,f->queries,1));
    if (VK_CALL(vkEndCommandBuffer(f->work_command)) < 0) return false;
    queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
    f->work_submit_ns = vkd3d_get_current_time_ns();
    vr = VK_CALL(vkQueueSubmit2(queue,1,&submit,f->work_fence));
    vkd3d_queue_release(chain->queue->vkd3d_queue);
    if (vr < 0) return false;
    f->work_pending = true;
    if (source)
    {
        cost = vkd3d_get_current_time_ns() - record_begin_ns + UINT64_C(250000);
        f->shared_submit_budget_ns = max(f->shared_submit_budget_ns,cost);
    }
    return true;
}
static bool dxgi_vk_swap_chain_fg_finish_work(struct dxgi_vk_swap_chain *chain)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    struct d3d12_device *device = chain->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    /* Same-queue ordering puts the capture copy before shared work. Its fence
     * is already covered by this work fence; never wait separately on pickup. */
    if (VK_CALL(vkWaitForFences(device->vk_device,1,&f->work_fence,VK_TRUE,UINT64_MAX)) < 0) return false;
    f->work_pending = false;
    dxgi_vk_swap_chain_fg_timing(chain,f->work_submit_ns);
    return true;
}
static void dxgi_vk_swap_chain_fg_plan_job(struct wineps5_fg_present *f,
        const struct wineps5_fg_job *job, uint64_t frame_count)
{
    f->generations = wineps5_fg_plan(&f->pacer,job->time_ns,f->settings.mult);
    if (f->pacer.interval > 0) { f->interval_sum_ms += f->pacer.interval * 1000; ++f->interval_samples; }
    if (frame_count < 3) f->generations = 0;
    if (!f->generations) f->pacer_skipped += f->settings.mult - 1;
    f->generation = 0;
}
/* Called only by the same worker, after the held real blit is queued. Reserve
 * one look-ahead source; the old capture reader has already finished. Do not
 * change chain->request or wait for GPU work before the old real deadline. */
static void dxgi_vk_swap_chain_fg_prefetch(struct dxgi_vk_swap_chain *chain,
        uint64_t deadline, bool *budget_skipped)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    struct wineps5_fg_job job;
    uint64_t now, cost;
    pthread_mutex_lock(&f->lock);
    if (!wineps5_fg_prefetch_eligible(f->pending,f->prefetched,f->work_pending,
            f->stop || f->failed || f->needs_rebuild))
    {
        pthread_mutex_unlock(&f->lock); return;
    }
    now = vkd3d_get_current_time_ns(); /* Budget against time after locking. */
    cost = f->shared_submit_budget_ns ? f->shared_submit_budget_ns : UINT64_C(1000000);
    if (!wineps5_fg_prefetch_fits(now,deadline,cost))
    {
        if (!*budget_skipped) { ++f->prefetch_deadline_skipped; *budget_skipped = true; }
        pthread_mutex_unlock(&f->lock); return;
    }
    job = f->pending_job; f->pending = false; f->active_slot = job.slot;
    pthread_mutex_unlock(&f->lock);
    job.pickup_ns = now; /* Report later, never block the held real on census logging. */
    dxgi_vk_swap_chain_fg_plan_job(f,&job,f->frame_count + 1);
    if (!dxgi_vk_swap_chain_fg_submit_work(chain,&f->captures[job.slot],f->frame_count + 1))
    {
        pthread_mutex_lock(&f->lock); f->failed = true; pthread_mutex_unlock(&f->lock);
        ERR("WINEPS5 vkd3d fg status=SubmissionFailed method=%s stage=prefetch\n",
                dxgi_vk_swap_chain_fg_method_name(f));
        return;
    }
    if (vkd3d_get_current_time_ns() > deadline + UINT64_C(1000000)) ++f->prefetch_deadline_overrun;
    f->prefetched_job = job; f->prefetched = true;
}
/* Called under the capture lock. A queued CPU Present is not a usable newer
 * source until its copy fence completes. Never cancel on an unready job. */
static bool dxgi_vk_swap_chain_fg_newer_ready(struct dxgi_vk_swap_chain *chain)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    return f->pending && VK_CALL(vkGetFenceStatus(chain->queue->device->vk_device,
            f->copy_fences[f->pending_job.slot])) == VK_SUCCESS;
}
/* No queue callback ever sleeps here. Check newer real work at <=1 ms cadence;
 * a newer job cancels the remaining generated outputs, not the game's latency
 * signal. The real wait can prepare one next job, but its WSI outputs remain
 * after this real frame. Generation waits never advance the next pair. */
static bool dxgi_vk_swap_chain_fg_wait_output(struct dxgi_vk_swap_chain *chain, uint64_t deadline, bool generated)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    uint64_t now;
    bool cancel, newer_ready, budget_skipped = false;
    for (;;)
    {
        pthread_mutex_lock(&f->lock);
        newer_ready = generated && dxgi_vk_swap_chain_fg_newer_ready(chain);
        cancel = f->stop || newer_ready;
        if (!f->stop && newer_ready)
            f->cancelled_by_newer_real += f->generations - f->generation;
        pthread_mutex_unlock(&f->lock);
        if (cancel) return false;
        now = vkd3d_get_current_time_ns();
        if (now >= deadline) return true;
        if (!generated)
        {
            dxgi_vk_swap_chain_fg_prefetch(chain,deadline,&budget_skipped);
            now = vkd3d_get_current_time_ns();
            if (now >= deadline) return true;
        }
        dxgi_vk_swap_chain_platform_sleep_for_ns(&chain->frame_rate_limit.sleep_state,
                min(deadline - now,UINT64_C(1000000)));
    }
}
static bool dxgi_vk_swap_chain_fg_expired(struct wineps5_fg_present *f,
        uint64_t now, uint64_t deadline, unsigned int remaining)
{
    if (now <= deadline) return false;
    f->deadline_missed += remaining;
    f->deadline_late_samples += remaining;
    f->deadline_late_sum_ms += (now - deadline) * 1e-6 * remaining;
    return true;
}
static void *dxgi_vk_swap_chain_fg_thread(void *opaque)
{
    struct dxgi_vk_swap_chain *chain = opaque;
    struct wineps5_fg_present *f = &chain->present.fg;
    struct wineps5_fg_job job;
    struct wineps5_fg_schedule schedule;
    uint64_t step, deadline, before, ready_ns, first_deadline, now;
    bool metadata_changed, rebuild, prepared;
    unsigned int generations, g;
    for (;;)
    {
        pthread_mutex_lock(&f->lock);
        while (!f->pending && !f->prefetched && !f->stop) condvar_reltime_wait(&f->cond,&f->lock);
        if (!f->pending && !f->prefetched && f->stop) { pthread_mutex_unlock(&f->lock); break; }
        prepared = f->prefetched;
        if (prepared) { job = f->prefetched_job; f->prefetched = false; }
        else { job = f->pending_job; f->pending = false; }
        f->active_slot = job.slot;
        pthread_mutex_unlock(&f->lock);
        if (!prepared)
        {
            job.pickup_ns = vkd3d_get_current_time_ns();
            dxgi_vk_swap_chain_fg_plan_job(f,&job,f->frame_count);
            /* Queue generation at pickup, before metadata/WSI work or waits. */
            if (!dxgi_vk_swap_chain_fg_submit_work(chain,&f->captures[job.slot],f->frame_count))
            {
                ERR("WINEPS5 vkd3d fg status=SubmissionFailed method=%s\n",dxgi_vk_swap_chain_fg_method_name(f));
                pthread_mutex_lock(&f->lock); f->failed = true; pthread_mutex_unlock(&f->lock); break;
            }
        }
        vkd3d_ps5_gpu_census_fg_stage(WINEPS5_FG_STAGE_WORKER_PICKUP,job.request.present_time_ns,job.pickup_ns);
        generations = f->generations; f->generation = 0;
        f->present_deadline_ns = 0;
        metadata_changed = memcmp(&chain->request.dxgi_hdr_metadata,&job.request.dxgi_hdr_metadata,
                sizeof(job.request.dxgi_hdr_metadata)) != 0;
        chain->request = job.request;
        if (metadata_changed || chain->request.modifies_hdr_metadata) dxgi_vk_swap_chain_set_hdr_metadata(chain);
        if (!dxgi_vk_swap_chain_fg_finish_work(chain))
        {
            ERR("WINEPS5 vkd3d fg status=SubmissionFailed method=%s\n",dxgi_vk_swap_chain_fg_method_name(f));
            pthread_mutex_lock(&f->lock); f->failed = true; pthread_mutex_unlock(&f->lock);
            break;
        }
        /* Eden presents generated outputs first, then holds real N. The input
         * pair only becomes usable after its queued GPU work completes, not
         * when the CPU capture callback timestamps/enqueues its copy. */
        ready_ns = vkd3d_get_current_time_ns();
        vkd3d_ps5_gpu_census_fg_stage(WINEPS5_FG_STAGE_SHARED_COMPLETE,job.request.present_time_ns,ready_ns);
        f->input_ready_age_sum_ms += (ready_ns - job.time_ns) * 1e-6; ++f->input_ready_samples;
        schedule = wineps5_fg_pair_schedule(ready_ns,(uint64_t)(f->pacer.interval * 1e9),generations);
        step = schedule.step; first_deadline = schedule.first; deadline = schedule.real;
        for (g = 0; g < generations; ++g)
        {
            uint64_t generated_deadline = first_deadline + g * step;
            f->generation = g;
            f->deadline_offset_sum_ms += (generated_deadline - ready_ns) * 1e-6;
            ++f->offset_samples;
            if (!dxgi_vk_swap_chain_fg_wait_output(chain,0,true)) break;
            /* First generation is already in the shared submission. */
            if (g && (!dxgi_vk_swap_chain_fg_submit_work(chain,NULL,f->frame_count) ||
                    !dxgi_vk_swap_chain_fg_finish_work(chain)))
            {
                pthread_mutex_lock(&f->lock); f->failed = true; pthread_mutex_unlock(&f->lock);
                ERR("WINEPS5 vkd3d fg status=SubmissionFailed method=%s\n",dxgi_vk_swap_chain_fg_method_name(f)); break;
            }
            now = vkd3d_get_current_time_ns();
            if (dxgi_vk_swap_chain_fg_expired(f,now,deadline,generations - g)) break;
            if (!dxgi_vk_swap_chain_fg_wait_output(chain,generated_deadline,true)) break;
            now = vkd3d_get_current_time_ns();
            if (dxgi_vk_swap_chain_fg_expired(f,now,deadline,generations - g)) break;
            f->source = wineps5_fg_output(f->engine,g);
            before = chain->present.internal_blit_count;
            dxgi_vk_swap_chain_present_iteration(chain,job.count,0,true);
            if (chain->present.internal_blit_count == before) { ++f->present_failed; break; }
            if (f->output_success)
            {
                ++f->generated_count;
            }
            else ++f->present_failed;
        }
        /* Stop drains a final real frame, with no extra pacing delay. */
        pthread_mutex_lock(&f->lock);
        if (f->stop || dxgi_vk_swap_chain_fg_newer_ready(chain)) deadline = vkd3d_get_current_time_ns();
        rebuild = f->needs_rebuild || f->failed;
        pthread_mutex_unlock(&f->lock);
        if (rebuild) break;
        /* Acquire and submit the real blit now. The sole WSI implementation
         * waits at QueuePresent, allowing this GPU work to overlap the hold. */
        f->present_deadline_ns = deadline;
        f->held_real = *wineps5_fg_input(f->engine,f->frame_count);
        f->source = &f->held_real;
        before = chain->present.internal_blit_count;
        dxgi_vk_swap_chain_present_iteration(chain,job.count,0,false);
        f->present_deadline_ns = 0;
        if (chain->present.internal_blit_count != before)
        {
            if (f->output_success)
            {
                ++f->real_count;
                /* Output timestamps are sampled centrally at successful QueuePresent. */
            }
            else ++f->present_failed;
        }
        else ++f->present_failed;
        ++f->frame_count;
        dxgi_vk_swap_chain_fg_metrics(chain);
        pthread_mutex_lock(&f->lock);
        if (!f->prefetched) f->active_slot = UINT32_MAX;
        pthread_mutex_unlock(&f->lock);
        pthread_mutex_lock(&f->lock); rebuild = f->needs_rebuild || f->failed; pthread_mutex_unlock(&f->lock);
        if (rebuild) break;
    }
    f->source = NULL;
    return NULL;
}
/* Pause still drains in-flight GPU users, but retains the expensive engine,
 * shader pipelines, image ring and command resources for the next auto start. */
static void dxgi_vk_swap_chain_fg_pause(struct dxgi_vk_swap_chain *chain)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    struct d3d12_device *device = chain->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    unsigned int i;
    if (!f->running && !f->engine && !f->copy_pool && !f->work_pool) return;
    vkd3d_atomic_uint32_store_explicit(&f->metrics_active,0,vkd3d_memory_order_release);
    if (f->running)
    {
        pthread_mutex_lock(&f->lock); f->stop = true; condvar_reltime_signal(&f->cond); pthread_mutex_unlock(&f->lock);
        pthread_join(f->thread,NULL);
        pthread_mutex_destroy(&f->lock); condvar_reltime_destroy(&f->cond);
        f->running = false;
        dxgi_vk_swap_chain_drain_internal_blit_semaphore(chain,chain->present.internal_blit_count);
    }
    if (f->work_pending)
    {
        VK_CALL(vkWaitForFences(device->vk_device,1,&f->work_fence,VK_TRUE,UINT64_MAX));
        f->work_pending = false;
    }
    for (i = 0; i < 3; ++i)
    {
        if (f->copy_fences[i]) VK_CALL(vkWaitForFences(device->vk_device,1,&f->copy_fences[i],VK_TRUE,UINT64_MAX));
    }
    f->source = NULL; f->stop = f->pending = f->prefetched = false;
    f->frame_count = f->last_output_ns = f->present_deadline_ns = 0;
    f->work_submit_ns = f->shared_submit_budget_ns = 0;
    f->pacer = (struct wineps5_fg_pacer){0};
}

static void dxgi_vk_swap_chain_fg_stop(struct dxgi_vk_swap_chain *chain)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    struct d3d12_device *device = chain->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    unsigned int i;
    dxgi_vk_swap_chain_fg_pause(chain);
    for (i = 0; i < 3; ++i)
    {
        VK_CALL(vkDestroyFence(device->vk_device,f->copy_fences[i],NULL)); f->copy_fences[i] = VK_NULL_HANDLE;
        wineps5_fg_image_destroy(device,&f->captures[i]);
    }
    VK_CALL(vkDestroyFence(device->vk_device,f->work_fence,NULL)); f->work_fence = VK_NULL_HANDLE;
    VK_CALL(vkDestroyCommandPool(device->vk_device,f->copy_pool,NULL)); f->copy_pool = VK_NULL_HANDLE;
    VK_CALL(vkDestroyCommandPool(device->vk_device,f->work_pool,NULL)); f->work_pool = VK_NULL_HANDLE;
    VK_CALL(vkDestroyQueryPool(device->vk_device,f->queries,NULL)); f->queries = VK_NULL_HANDLE;
    wineps5_fg_destroy(f->engine); f->engine = NULL;
    f->source = NULL; f->stop = f->pending = f->prefetched = false;
    f->frame_count = f->last_output_ns = f->present_deadline_ns = 0;
    f->work_submit_ns = f->shared_submit_budget_ns = 0;
    f->needs_rebuild = false;
    f->pacer = (struct wineps5_fg_pacer){0};
}
static const char *dxgi_vk_swap_chain_fg_start(struct dxgi_vk_swap_chain *chain,
        const struct dxgi_vk_swap_chain_present_request *request)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    struct d3d12_device *device = chain->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct d3d12_resource *resource = chain->user.backbuffers[request->user_index];
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = chain->queue->vkd3d_queue->vk_family_index};
    VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkQueryPoolCreateInfo queries = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,.queryCount = 2};
    const char *status;
    unsigned int i;
    if (f->engine) goto resume;
    f->extent = (VkExtent2D){chain->desc.Width,chain->desc.Height}; f->format = resource->format->vk_format;
    if (!(chain->queue->vkd3d_queue->vk_queue_flags & VK_QUEUE_COMPUTE_BIT)) return "DeviceUnsupported";
    status = wineps5_fg_create(device,f->extent,f->format,&f->settings,&f->engine);
    if (strcmp(status,"Ok")) return status;
    status = "ResourceCreationFailed";
    if (VK_CALL(vkCreateCommandPool(device->vk_device,&pool,NULL,&f->copy_pool)) < 0 ||
            VK_CALL(vkCreateCommandPool(device->vk_device,&pool,NULL,&f->work_pool)) < 0) goto fail;
    command.commandPool = f->copy_pool; command.commandBufferCount = 3;
    if (VK_CALL(vkAllocateCommandBuffers(device->vk_device,&command,f->copy_commands)) < 0) goto fail;
    command.commandPool = f->work_pool; command.commandBufferCount = 1;
    if (VK_CALL(vkAllocateCommandBuffers(device->vk_device,&command,&f->work_command)) < 0 ||
            VK_CALL(vkCreateFence(device->vk_device,&fence,NULL,&f->work_fence)) < 0) goto fail;
    for (i = 0; i < 3; ++i)
        if (!wineps5_fg_image_create(device,f->extent,f->format,&f->captures[i]) ||
                VK_CALL(vkCreateFence(device->vk_device,&fence,NULL,&f->copy_fences[i])) < 0) goto fail;
    f->timestamps = chain->queue->vkd3d_queue->timestamp_bits != 0;
    if (f->timestamps && VK_CALL(vkCreateQueryPool(device->vk_device,&queries,NULL,&f->queries)) < 0) f->timestamps = false;
resume:
    f->active_slot = UINT32_MAX; f->last_request = *request;
    f->window_start_ns = vkd3d_get_current_time_ns();
    f->real_count = f->generated_count = f->timing_count = 0; f->gpu_sum_ms = f->gpu_max_ms = 0;
    f->cancelled_by_newer_real = f->deadline_missed = f->ring_full = f->worker_busy = f->pacer_skipped = f->present_failed = 0;
    f->interval_samples = f->offset_samples = f->acquire_samples = f->acquire_sum_ns = f->acquire_max_ns = 0;
    f->interval_sum_ms = f->deadline_offset_sum_ms = 0;
    f->input_ready_samples = f->deadline_late_samples = 0;
    f->prefetch_deadline_skipped = f->prefetch_deadline_overrun = f->shared_submit_budget_ns = 0;
    f->deadline_late_sum_ms = f->input_ready_age_sum_ms = 0;
    spinlock_acquire(&f->block_stats_lock);
    f->game_block = f->callback_block = f->capture_lock_wait = f->capture_queue_wait = f->capture_submit = (struct wineps5_fg_block_stats){0};
    spinlock_release(&f->block_stats_lock);
    pthread_mutex_init(&f->lock,NULL); condvar_reltime_init(&f->cond);
    f->running = true;
    if (pthread_create(&f->thread,NULL,dxgi_vk_swap_chain_fg_thread,chain))
    {
        f->running = false; pthread_mutex_destroy(&f->lock); condvar_reltime_destroy(&f->cond);
        status = "ThreadCreationFailed"; goto fail;
    }
    vkd3d_atomic_uint32_store_explicit(&f->metrics_active,1,vkd3d_memory_order_release);
    ERR("WINEPS5 vkd3d fg timing=%s\n",f->timestamps ? "timestamp" : "cpu_submit_to_complete");
    ERR("WINEPS5 vkd3d fg outputs=queue_present_accepted displayed=unavailable replaced=unavailable correlate=PS5_VIDEOOUT_STATS\n");
    return "Ok";
fail:
    dxgi_vk_swap_chain_fg_stop(chain); return status;
}
static void dxgi_vk_swap_chain_fg_update(struct dxgi_vk_swap_chain *chain, uint64_t present_ns)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    struct wineps5_fg_settings settings = wineps5_fg_defaults();
    uint64_t now = vkd3d_get_current_time_ns();
    bool was_auto, was_running;
    char text[256];
    size_t size;
    FILE *file;
    if (f->settings.automatic) wineps5_fg_auto_note(&f->auto_control,present_ns);
    if (now < f->next_check_ns) return;
    f->next_check_ns = now + UINT64_C(1000000000);
    if ((file = fopen("C:\\wineps5-fg.txt","rb")))
    {
        size = fread(text,1,sizeof(text),file);
        if (!ferror(file) && size < sizeof(text) && !memchr(text,0,size))
        {
            text[size] = 0;
            if (!wineps5_fg_parse(text,&settings)) settings = wineps5_fg_defaults();
        }
        fclose(file);
    }
    if (!f->initialized || settings.enabled != f->settings.enabled || settings.mult != f->settings.mult ||
            settings.flow != f->settings.flow || settings.automatic != f->settings.automatic ||
            settings.on_below != f->settings.on_below || settings.off_above != f->settings.off_above ||
            settings.method != f->settings.method || settings.mode != f->settings.mode)
    {
        was_auto = f->settings.automatic; was_running = f->running;
        if (!f->failed && !f->needs_rebuild && wineps5_fg_reuse_engine(&f->settings,&settings))
            dxgi_vk_swap_chain_fg_pause(chain);
        else dxgi_vk_swap_chain_fg_stop(chain);
        f->settings = settings; f->initialized = true; f->failed = false;
        if (was_auto != settings.automatic)
        {
            f->auto_control = (struct wineps5_fg_auto){0};
            f->auto_reported_on = false;
            if (settings.automatic)
            {
                wineps5_fg_auto_note(&f->auto_control,present_ns);
                if (was_running) f->auto_control.last_change_ns = now;
                ERR("WINEPS5 vkd3d fg auto state=off game_fps=0.0 reason=warmup switch_ms=%.3f\n",
                        (vkd3d_get_current_time_ns() - now) * 1e-6);
            }
        }
        ERR("WINEPS5 vkd3d fg enabled=%u mult=%u flow=%.2f source=file auto=%u on_below=%.1f off_above=%.1f method=%s mode=between requested_mode=%s\n",
                settings.enabled,settings.mult,settings.flow,settings.automatic,settings.on_below,settings.off_above,
                wineps5_fg_method_name(settings.method),wineps5_fg_mode_name(settings.mode));
        if (!wineps5_fg_mode_supported(settings.method,settings.mode))
            ERR("WINEPS5 vkd3d fg mode=ahead unsupported method=%s using=between\n",wineps5_fg_method_name(settings.method));
        if (!settings.enabled && !settings.automatic)
            ERR("WINEPS5 vkd3d fg status=Disabled method=%s\n",wineps5_fg_method_name(settings.method));
    }
    if (f->settings.automatic && !f->failed && wineps5_fg_auto_decide(&f->auto_control,&f->settings,now) &&
            !f->auto_control.enabled)
    {
        uint64_t switch_start = vkd3d_get_current_time_ns();
        dxgi_vk_swap_chain_fg_pause(chain);
        f->auto_reported_on = false;
        ERR("WINEPS5 vkd3d fg auto state=off game_fps=%.1f reason=rate_above switch_ms=%.3f\n",
                f->auto_control.game_fps,(vkd3d_get_current_time_ns() - switch_start) * 1e-6);
    }
}
/* Return true if capture mode owns this Present, including a mailbox drop when
 * all fixed slots are still on GPU. Never wait for copy completion on the game
 * callback, and never allocate commands/resources per frame. */
static bool dxgi_vk_swap_chain_fg_capture(struct dxgi_vk_swap_chain *chain,
        const struct dxgi_vk_swap_chain_present_request *request, uint64_t count, unsigned int *captured)
{
    struct wineps5_fg_present *f = &chain->present.fg;
    struct d3d12_device *device = chain->queue->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct d3d12_resource *resource = chain->user.backbuffers[request->user_index];
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkCommandBufferSubmitInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    VkSemaphoreSubmitInfo complete = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = chain->present.vk_complete_semaphore,.value = count,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,.pCommandBufferInfos = &command,
        .signalSemaphoreInfoCount = 1,.pSignalSemaphoreInfos = &complete};
    VkQueue queue;
    VkResult vr;
    uint64_t now = vkd3d_get_current_time_ns(), lock_ns = 0, queue_ns = 0, submit_ns = 0, before;
    unsigned int slot;
    const char *status;
    bool failed, rebuild;
    *captured = UINT32_MAX;
    dxgi_vk_swap_chain_fg_update(chain,request->present_time_ns);
    if (!(f->settings.automatic ? f->auto_control.enabled : f->settings.enabled) || (!f->running && f->failed)) return false;
    /* A paused engine must be invalidated too, before it can be resumed. */
    if (f->engine && (f->extent.width != chain->desc.Width || f->extent.height != chain->desc.Height ||
            f->format != resource->format->vk_format ||
            dxgi_vk_swap_chain_request_needs_swapchain_recreation(chain,request,&f->last_request)))
        dxgi_vk_swap_chain_fg_stop(chain);
    if (f->running)
    {
        dxgi_vk_swap_chain_fg_capture_lock(f,&lock_ns);
        failed = f->failed; rebuild = f->needs_rebuild; pthread_mutex_unlock(&f->lock);
        if (failed || rebuild)
        {
            dxgi_vk_swap_chain_fg_stop(chain);
            if (failed) return false;
        }
    }
    if (!f->running)
    {
        uint64_t switch_start = vkd3d_get_current_time_ns();
        if (dxgi_vk_swap_chain_request_needs_swapchain_recreation(chain,request,&chain->request))
            chain->present.force_swapchain_recreation = true;
        chain->request = *request;
        /* Capture toggling does not change the WSI extent/format/colour space.
         * Honour existing recreation requests, without manufacturing a new one. */
        dxgi_vk_swap_chain_present_recreate_swapchain_if_required(chain);
        status = dxgi_vk_swap_chain_fg_start(chain,request);
        ERR("WINEPS5 vkd3d fg status=%s method=%s requested=%s mode=between\n",status,
                wineps5_fg_method_name(f->engine ? wineps5_fg_engine_method(f->engine) : f->settings.method),
                wineps5_fg_method_name(f->settings.method));
        if (strcmp(status,"Ok"))
        {
            f->failed = true;
            if (f->settings.automatic)
            {
                f->auto_control.enabled = false;
                if (f->auto_reported_on)
                    ERR("WINEPS5 vkd3d fg auto state=off game_fps=%.1f reason=unavailable switch_ms=%.3f\n",
                            f->auto_control.game_fps,(vkd3d_get_current_time_ns() - switch_start) * 1e-6);
                f->auto_reported_on = false;
            }
            return false;
        }
        if (f->settings.automatic && !f->auto_reported_on)
        {
            f->auto_reported_on = true;
            ERR("WINEPS5 vkd3d fg auto state=on game_fps=%.1f reason=rate_below switch_ms=%.3f\n",
                    f->auto_control.game_fps,(vkd3d_get_current_time_ns() - switch_start) * 1e-6);
        }
    }
    if (vkd3d_atomic_uint32_load_explicit(&resource->initial_layout_transition,vkd3d_memory_order_relaxed)) return true;
    dxgi_vk_swap_chain_fg_capture_lock(f,&lock_ns);
    wineps5_hud_note(&chain->present.hud.timing,now);
    f->last_request = *request;
    for (slot = 0; slot < 3; ++slot)
        if (slot != f->active_slot && (!f->pending || slot != f->pending_job.slot) &&
                VK_CALL(vkGetFenceStatus(device->vk_device,f->copy_fences[slot])) == VK_SUCCESS) break;
    if (slot == 3)
    {
        ++f->ring_full; pthread_mutex_unlock(&f->lock);
        dxgi_vk_swap_chain_fg_note_capture_block(f,lock_ns,0,0); return true;
    }
    /* This spare slot has no active/pending job referencing it, and capture
     * callbacks are serial. The worker cannot select it until we publish it.
     * Do not retain the shared mutex while recording or acquiring VkQueue. */
    pthread_mutex_unlock(&f->lock);
    VK_CALL(vkResetCommandBuffer(f->copy_commands[slot],0));
    vr = VK_CALL(vkBeginCommandBuffer(f->copy_commands[slot],&begin));
    if (vr == VK_SUCCESS)
    {
        wineps5_fg_copy(device,f->copy_commands[slot],resource->res.vk_image,resource->common_layout,&f->captures[slot]);
        vr = VK_CALL(vkEndCommandBuffer(f->copy_commands[slot]));
    }
    if (vr == VK_SUCCESS)
    {
        command.commandBuffer = f->copy_commands[slot];
        VK_CALL(vkResetFences(device->vk_device,1,&f->copy_fences[slot]));
        before = vkd3d_get_current_time_ns();
        queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
        queue_ns = vkd3d_get_current_time_ns() - before;
        before = vkd3d_get_current_time_ns();
        /* Copy completion AND game latency are one submission. No older FG
         * batch can slip between the copy and a separately submitted signal. */
        vr = VK_CALL(vkQueueSubmit2(queue,1,&submit,f->copy_fences[slot]));
        submit_ns = vkd3d_get_current_time_ns() - before;
        vkd3d_queue_release(chain->queue->vkd3d_queue);
    }
    if (vr == VK_SUCCESS)
        vkd3d_ps5_gpu_census_fg_stage(WINEPS5_FG_STAGE_CAPTURE_SUBMIT,request->present_time_ns,
                vkd3d_get_current_time_ns());
    dxgi_vk_swap_chain_fg_capture_lock(f,&lock_ns);
    if (vr < 0)
    {
        VK_CALL(vkDestroyFence(device->vk_device,f->copy_fences[slot],NULL)); f->copy_fences[slot] = VK_NULL_HANDLE;
        f->failed = true;
        pthread_mutex_unlock(&f->lock);
        dxgi_vk_swap_chain_fg_note_capture_block(f,lock_ns,queue_ns,submit_ns);
        ERR("WINEPS5 vkd3d fg status=CaptureFailed method=%s vr=%d\n",dxgi_vk_swap_chain_fg_method_name(f),vr);
        return true;
    }
    if (f->pending) ++f->worker_busy;
    f->pending_job = (struct wineps5_fg_job){*request,slot,count,now,0};
    *captured = slot; f->pending = true;
    condvar_reltime_signal(&f->cond); pthread_mutex_unlock(&f->lock);
    dxgi_vk_swap_chain_fg_note_capture_block(f,lock_ns,queue_ns,submit_ns);
    return true;
}
