/*
 * Copyright 2022 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#ifdef _WIN32
#include "vkd3d_win32.h"
#endif
#include "vkd3d_private.h"
#include "vkd3d_timestamp_profiler.h"
#include <math.h>
#include <stdio.h>

#include "wineps5_hud.h"
#include "wineps5_fg_vk.h"
#include "vs_wineps5_hud.h"
#include "fs_wineps5_hud.h"

struct wineps5_hud_quad
{
    float rect[4], color[4];
    uint32_t mask[2];
};

struct wineps5_hud
{
    struct wineps5_hud_settings settings;
    struct wineps5_hud_timing timing;
    struct wineps5_hud_metrics metrics;
    struct wineps5_hud_timing output_timing;
    struct wineps5_hud_metrics output_metrics;
    uint64_t next_check_ns;
    bool initialized;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkFormat format;
    VkBuffer buffer;
    VkDeviceMemory memory;
    struct wineps5_hud_quad *mapped;
};
static inline bool vkd3d_swapchain_present_mode_parse(const char *string, VkPresentModeKHR *present_mode)
{
    struct present_mode_entry
    {
        const char *name;
        VkPresentModeKHR value;
    };

    static const struct present_mode_entry present_mode_table[] =
    {
        {"IMMEDIATE", VK_PRESENT_MODE_IMMEDIATE_KHR},
        {"MAILBOX", VK_PRESENT_MODE_MAILBOX_KHR},
        {"FIFO", VK_PRESENT_MODE_FIFO_KHR},
        {"FIFO_RELAXED", VK_PRESENT_MODE_FIFO_RELAXED_KHR},
        {"FIFO_LATEST_READY", VK_PRESENT_MODE_FIFO_LATEST_READY_KHR},
    };

    for (size_t i = 0; i < ARRAY_SIZE(present_mode_table); i++)
    {
        if (strcmp(string, present_mode_table[i].name) == 0)
        {
            *present_mode = present_mode_table[i].value;
            return true;
        }
    }

    return false;
}

static inline bool present_mode_pacing_should_wait(VkPresentModeKHR present_mode)
{
    /* FIFO_LATEST_READY is intentionally excluded. Unlike FIFO and FIFO_RELAXED,
     * it does not block on Vsync boundaries and present-wait would not provide
     * meaningful backpressure. When VK_EXT_present_timing becomes available,
     * VkPresentTimingInfoEXT.targetTime would be the appropriate pacing mechanism.
     */
    return present_mode == VK_PRESENT_MODE_FIFO_KHR ||
           present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
}

static inline struct dxgi_vk_swap_chain_factory *impl_from_IDXGIVkSwapChainFactory(IDXGIVkSwapChainFactory *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_vk_swap_chain_factory, IDXGIVkSwapChainFactory_iface);
}

static ULONG STDMETHODCALLTYPE dxgi_vk_swap_chain_factory_AddRef(IDXGIVkSwapChainFactory *iface)
{
    struct dxgi_vk_swap_chain_factory *chain = impl_from_IDXGIVkSwapChainFactory(iface);
    TRACE("iface %p\n", iface);
    return ID3D12CommandQueue_AddRef(&chain->queue->ID3D12CommandQueue_iface);
}

static ULONG STDMETHODCALLTYPE dxgi_vk_swap_chain_factory_Release(IDXGIVkSwapChainFactory *iface)
{
    struct dxgi_vk_swap_chain_factory *chain = impl_from_IDXGIVkSwapChainFactory(iface);
    TRACE("iface %p\n", iface);
    return ID3D12CommandQueue_Release(&chain->queue->ID3D12CommandQueue_iface);
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_factory_QueryInterface(IDXGIVkSwapChainFactory *iface, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain_factory *chain = impl_from_IDXGIVkSwapChainFactory(iface);
    TRACE("iface %p\n", iface);
    return ID3D12CommandQueue_QueryInterface(&chain->queue->ID3D12CommandQueue_iface, riid, object);
}

struct low_latency_state
{
    bool mode;
    bool boost;
    uint32_t minimum_interval_us;
};

struct anti_lag_state
{
    bool mode;
    uint32_t max_fps;
};

struct dxgi_vk_swap_chain_present_request
{
    uint64_t begin_frame_time_ns;
    uint64_t present_time_ns;
    uint32_t user_index;
    uint32_t target_min_image_count;
    DXGI_FORMAT dxgi_format;
    DXGI_COLOR_SPACE_TYPE dxgi_color_space_type;
    DXGI_VK_HDR_METADATA dxgi_hdr_metadata;
    uint32_t swap_interval;
    uint64_t low_latency_frame_id;
    uint64_t hdr_auto_sdr_writer_hash;
    uint64_t hdr_auto_hdr_writer_hash;
    uint64_t hdr_auto_last_writer_hash;
    union
    {
        struct low_latency_state requested_low_latency_state;
        struct anti_lag_state requested_anti_lag_state;
    };
    bool low_latency_update_requested;
    bool modifies_hdr_metadata;
};

/* Queue callback owns capture; the paced worker exclusively owns WSI when on.
 * One pending job is coalesced, never a FIFO of delayed generated frames. */
struct wineps5_fg_job
{
    struct dxgi_vk_swap_chain_present_request request;
    unsigned int slot;
    uint64_t count, time_ns, pickup_ns;
};
struct wineps5_fg_present
{
    struct wineps5_fg_settings settings;
    struct wineps5_fg_auto auto_control;
    bool auto_reported_on;
    struct wineps5_fg_engine *engine;
    struct wineps5_fg_image captures[3];
    VkCommandPool copy_pool, work_pool;
    VkCommandBuffer copy_commands[3], work_command;
    VkFence copy_fences[3], work_fence;
    VkQueryPool queries;
    VkFormat format;
    VkExtent2D extent;
    pthread_t thread;
    pthread_mutex_t lock;
    condvar_reltime_t cond;
    spinlock_t block_stats_lock;
    uint32_t metrics_active;
    struct wineps5_fg_block_stats game_block, callback_block, capture_lock_wait, capture_queue_wait, capture_submit;
    struct wineps5_fg_job pending_job, prefetched_job;
    struct dxgi_vk_swap_chain_present_request last_request;
    struct wineps5_fg_pacer pacer;
    const struct wineps5_fg_image *source;
    struct wineps5_fg_image held_real;
    uint64_t next_check_ns, frame_count, last_output_ns;
    uint64_t present_deadline_ns, work_submit_ns, shared_submit_budget_ns;
    uint64_t prefetch_deadline_skipped, prefetch_deadline_overrun;
    uint64_t window_start_ns, real_count, generated_count, timing_count;
    uint64_t cancelled_by_newer_real, deadline_missed, ring_full, worker_busy, pacer_skipped, present_failed;
    uint64_t interval_samples, offset_samples, acquire_samples, acquire_sum_ns, acquire_max_ns;
    uint64_t input_ready_samples, deadline_late_samples;
    double interval_sum_ms, deadline_offset_sum_ms;
    double deadline_late_sum_ms, input_ready_age_sum_ms;
    double gpu_sum_ms, gpu_max_ms;
    unsigned int active_slot, generations, generation;
    bool initialized, running, stop, pending, prefetched, timestamps, failed, needs_rebuild, output_success, work_pending;
};

struct present_wait_entry
{
    uint64_t id;
    uint64_t present_count;
    uint64_t begin_frame_time_ns;
    uint64_t fg_present_ns;
};

#if defined(_WIN32)
typedef UINT (WINAPI *PFN_NtDelayExecution) (BOOL, LARGE_INTEGER*);
typedef UINT (WINAPI *PFN_NtQueryTimerResolution) (ULONG*, ULONG*, ULONG*);
typedef UINT (WINAPI *PFN_NtSetTimerResolution) (ULONG, BOOL, ULONG*);
#endif

struct platform_sleep_state
{
#if defined(_WIN32)
    PFN_NtDelayExecution NtDelayExecution;
    PFN_NtQueryTimerResolution NtQueryTimerResolution;
    PFN_NtSetTimerResolution NtSetTimerResolution;
#endif

    uint64_t sleep_threshold_ns;
};

/* PS5: Process-local live configuration for HDR10 presentation transforms. */
enum dxgi_vk_hdr_source
{
    DXGI_VK_HDR_SOURCE_UNINITIALIZED,
    DXGI_VK_HDR_SOURCE_OFF,
    DXGI_VK_HDR_SOURCE_ENV,
    DXGI_VK_HDR_SOURCE_FILE,
};

enum dxgi_vk_hdr_expansion_key
{
    DXGI_VK_HDR_EXPANSION_KEY_Y,
    DXGI_VK_HDR_EXPANSION_KEY_MAX,
};

struct dxgi_vk_hdr_settings
{
    enum vkd3d_swapchain_hdr_mode mode;
    float paper_nits;
    float peak_nits;
    float expansion;
    float saturation;
    float gamma;
    enum dxgi_vk_hdr_expansion_key expansion_key;
    float hdr_cap_nits;
    uint32_t sdr_shader_count;
    uint32_t hdr_shader_count;
    uint64_t sdr_shaders[VKD3D_HDR_AUTO_SHADER_COUNT];
    uint64_t hdr_shaders[VKD3D_HDR_AUTO_SHADER_COUNT];
};

struct dxgi_vk_hdr_push_constants
{
    float paper_nits;
    float peak_nits;
    float expansion;
    float saturation;
    float gamma;
    float knee_nits;
    float knee_pq;
    float cap_pq;
    float use_luminance;
};

#define DXGI_VK_HDR_AUTO_STABLE_PRESENTS 5
#define DXGI_VK_HDR_AUTO_UNKNOWN_WRITER_COUNT 32

static uint32_t vkd3d_hdr_auto_enabled;
static uint32_t vkd3d_hdr_auto_sdr_shader_count;
static uint32_t vkd3d_hdr_auto_hdr_shader_count;
static uint64_t vkd3d_hdr_auto_sdr_shaders[VKD3D_HDR_AUTO_SHADER_COUNT];
static uint64_t vkd3d_hdr_auto_hdr_shaders[VKD3D_HDR_AUTO_SHADER_COUNT];

enum vkd3d_hdr_auto_match vkd3d_hdr_auto_classify_shader(uint64_t shader_hash)
{
    uint32_t count;
    unsigned int i;

    if (!vkd3d_atomic_uint32_load_explicit(
            &vkd3d_hdr_auto_enabled, vkd3d_memory_order_acquire))
        return VKD3D_HDR_AUTO_DISABLED;

    count = vkd3d_atomic_uint32_load_explicit(
            &vkd3d_hdr_auto_hdr_shader_count, vkd3d_memory_order_relaxed);
    for (i = 0; i < count; i++)
    {
        if (vkd3d_atomic_uint64_load_explicit(
                &vkd3d_hdr_auto_hdr_shaders[i],
                vkd3d_memory_order_relaxed) == shader_hash)
            return VKD3D_HDR_AUTO_HDR;
    }

    count = vkd3d_atomic_uint32_load_explicit(
            &vkd3d_hdr_auto_sdr_shader_count, vkd3d_memory_order_relaxed);
    for (i = 0; i < count; i++)
    {
        if (vkd3d_atomic_uint64_load_explicit(
                &vkd3d_hdr_auto_sdr_shaders[i],
                vkd3d_memory_order_relaxed) == shader_hash)
            return VKD3D_HDR_AUTO_SDR;
    }

    return VKD3D_HDR_AUTO_UNKNOWN;
}

static void dxgi_vk_swap_chain_configure_hdr_auto(
        const struct dxgi_vk_hdr_settings *settings)
{
    unsigned int i;

    vkd3d_atomic_uint32_store_explicit(
            &vkd3d_hdr_auto_enabled, 0, vkd3d_memory_order_release);
    for (i = 0; i < VKD3D_HDR_AUTO_SHADER_COUNT; i++)
    {
        vkd3d_atomic_uint64_store_explicit(&vkd3d_hdr_auto_sdr_shaders[i],
                i < settings->sdr_shader_count ? settings->sdr_shaders[i] : 0,
                vkd3d_memory_order_relaxed);
        vkd3d_atomic_uint64_store_explicit(&vkd3d_hdr_auto_hdr_shaders[i],
                i < settings->hdr_shader_count ? settings->hdr_shaders[i] : 0,
                vkd3d_memory_order_relaxed);
    }
    vkd3d_atomic_uint32_store_explicit(&vkd3d_hdr_auto_sdr_shader_count,
            settings->sdr_shader_count, vkd3d_memory_order_relaxed);
    vkd3d_atomic_uint32_store_explicit(&vkd3d_hdr_auto_hdr_shader_count,
            settings->hdr_shader_count, vkd3d_memory_order_relaxed);
    vkd3d_atomic_uint32_store_explicit(&vkd3d_hdr_auto_enabled,
            settings->mode == VKD3D_SWAPCHAIN_HDR_MODE_AUTO,
            vkd3d_memory_order_release);
}

struct dxgi_vk_swap_chain
{
    IDXGIVkSwapChain2 IDXGIVkSwapChain_iface;
    struct d3d12_command_queue *queue;

    LONG refcount;
    LONG internal_refcount;
    DXGI_SWAP_CHAIN_DESC1 desc;

    vkd3d_native_sync_handle frame_latency_event;
    vkd3d_native_sync_handle frame_latency_event_internal;
    vkd3d_native_sync_handle present_request_done_event;
    bool outstanding_present_request;
    uint32_t frame_latency_event_internal_wait_counts;

    /* PS5: Always-on counters for diagnosing lost frame-latency wakeups. */
    struct
    {
        uint64_t present_count;
        uint64_t last_present_time_ns;
        uint64_t release_semaphore_count;
        uint64_t release_semaphore_fail_count;
        uint64_t internal_release_semaphore_count;
        uint64_t internal_release_semaphore_fail_count;
        uint64_t set_event_count;
        uint64_t set_event_fail_count;
        uint32_t pending_latency_count;
        uint32_t present_done_pending;
        uint32_t reported;
    } stall;

    UINT frame_latency;
    UINT frame_latency_internal;
    VkSurfaceKHR vk_surface;

    struct low_latency_state requested_low_latency_state;
    bool low_latency_update_requested;

    bool debug_latency;
    bool swapchain_maintenance1;

    struct
    {
        /* When resizing user buffers or emit commands internally,
         * we need to make sure all pending blits have completed on GPU. */
        VkSemaphore vk_internal_blit_semaphore;
        VkSemaphore vk_complete_semaphore;
        uint64_t internal_blit_count;

        /* PresentID is used depending on features and if we're really presenting on-screen. */
        uint64_t present_id;
        bool present_id_valid;

        /* Atomically updated after a PRESENT queue command has processed. Used to atomically check if
         * all outstanding present events have completed on CPU timeline. */
        UINT64 present_count;

        /* For blits. Use simple VkFences since we have to use binary semaphores with WSI release anyways.
         * We don't need to wait on these fences on main thread. */
        VkCommandPool vk_blit_command_pool;
        VkCommandBuffer vk_blit_command_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        uint64_t backbuffer_blit_timelines[DXGI_MAX_SWAP_CHAIN_BUFFERS];

        VkSwapchainKHR vk_swapchain;
        VkImage vk_backbuffer_images[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        VkImageView vk_backbuffer_image_views[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        VkSemaphore vk_release_semaphores[DXGI_MAX_SWAP_CHAIN_BUFFERS];

        VkSemaphore vk_acquire_semaphore[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        bool acquire_semaphore_signalled[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        uint64_t acquire_semaphore_consumed_at_blit[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        uint32_t acquire_semaphore_index;

        VkFence vk_swapchain_fences[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        bool vk_swapchain_fences_signalled[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        uint32_t swapchain_fence_index;

        uint32_t current_backbuffer_index;
        uint32_t backbuffer_width;
        uint32_t backbuffer_height;
        uint32_t backbuffer_count;
        VkFormat backbuffer_format;
        /* PS5: The actual Vulkan color space, not merely the DXGI request. */
        VkColorSpaceKHR backbuffer_color_space;

        struct vkd3d_swapchain_info pipeline;

        /* PS5: Live HDR10 transform state. Pipelines are format-specific and
         * created lazily, while parsed configuration survives WSI recreation. */
        struct
        {
            struct vkd3d_swapchain_info pipeline;
            struct dxgi_vk_hdr_push_constants push;
            struct dxgi_vk_hdr_push_constants auto_cap_push;
            struct dxgi_vk_hdr_settings env;
            struct dxgi_vk_hdr_settings effective;
            uint64_t next_file_check_ns;
            enum dxgi_vk_hdr_source source;
            enum vkd3d_swapchain_hdr_mode pipeline_mode;
            bool env_valid;
            bool pipeline_attempted;
            enum vkd3d_hdr_auto_match auto_content;
            enum vkd3d_hdr_auto_match auto_candidate;
            uint32_t auto_candidate_count;
            uint32_t auto_unknown_writer_count;
            uint64_t auto_candidate_writer;
            uint64_t auto_unknown_writers[DXGI_VK_HDR_AUTO_UNKNOWN_WRITER_COUNT];
        } hdr;
        struct wineps5_hud hud;
        struct wineps5_fg_present fg;

        uint32_t is_occlusion_state; /* Updated atomically. */

        /* State tracking in present tasks on how to deal with swapchain recreation. */
        bool force_swapchain_recreation;
        bool is_surface_lost;

        VkPresentModeKHR unlocked_present_mode;
        VkPresentModeKHR selected_present_mode;
        bool compatible_unlocked_present_mode;
        bool override_present_mode;

        /* Info about the current low latency state of the swapchain */
        uint32_t low_latency_present_mode_count;
        VkPresentModeKHR low_latency_present_modes[16];

        pthread_mutex_t low_latency_swapchain_lock;
        pthread_mutex_t low_latency_state_update_lock;

        VkSemaphore low_latency_sem;
        uint64_t low_latency_sem_value;

        struct low_latency_state low_latency_state;
    } present;

    struct dxgi_vk_swap_chain_present_request request, request_ring[DXGI_MAX_SWAP_CHAIN_BUFFERS];

    struct
    {
        struct d3d12_resource *backbuffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        VkImageView vk_image_views[DXGI_MAX_SWAP_CHAIN_BUFFERS];
        uint64_t blit_count;
        uint64_t present_count;
        UINT index;

        DXGI_COLOR_SPACE_TYPE dxgi_color_space_type;
        DXGI_VK_HDR_METADATA dxgi_hdr_metadata;
        bool modifies_hdr_metadata;
        uint64_t begin_frame_time_ns;
    } user;

    struct
    {
        spinlock_t lock;

        uint64_t count;
        uint64_t time;
    } frame_statistics;

    struct
    {
        pthread_mutex_t lock;

        bool enable;
        bool has_user_override;
        uint64_t target_interval_ns;
        uint64_t next_deadline_ns;
        uint64_t heuristic_frame_time_ns;
        uint32_t heuristic_frame_count;

        struct platform_sleep_state sleep_state;
    } frame_rate_limit;

    struct
    {
        VkSurfaceFormatKHR *formats;
		size_t formats_size;
        uint32_t format_count;
        pthread_mutex_t lock;
    } properties;

    struct
    {
        pthread_t thread;
        struct present_wait_entry *wait_queue;
        size_t wait_queue_size;
        size_t wait_queue_count;
        struct wineps5_display_latency_ring display_latency;
        condvar_reltime_t cond;
        pthread_mutex_t lock;
        bool supports_present_wait;
        bool skip_waits;
    } wait_thread;
};
#include "wineps5_hud_render.h"

static void dxgi_vk_swap_chain_fg_stop(struct dxgi_vk_swap_chain *chain);
static void dxgi_vk_swap_chain_fg_note_game_block(struct dxgi_vk_swap_chain *chain, uint64_t ns);
static void dxgi_vk_swap_chain_platform_sleep_for_ns(struct platform_sleep_state *sleep_state, uint64_t duration_ns);
static bool dxgi_vk_swap_chain_fg_wait_output(struct dxgi_vk_swap_chain *chain, uint64_t deadline, bool generated);

#define DXGI_VK_STALL_TIMEOUT_NS (5ull * 1000ull * 1000ull * 1000ull)

static void dxgi_vk_swap_chain_note_present(struct dxgi_vk_swap_chain *chain)
{
    uint64_t now;

    now = vkd3d_get_current_time_ns();
    vkd3d_atomic_uint64_store_explicit(&chain->stall.last_present_time_ns,
            now, vkd3d_memory_order_relaxed);
    vkd3d_atomic_uint64_store_explicit(
            &chain->queue->device->stall_last_present_time_ns,
            now, vkd3d_memory_order_relaxed);
    vkd3d_atomic_uint64_increment(&chain->stall.present_count,
            vkd3d_memory_order_relaxed);
    vkd3d_atomic_uint32_store_explicit(&chain->stall.reported, 0,
            vkd3d_memory_order_relaxed);

    if (vkd3d_native_sync_handle_is_valid(chain->frame_latency_event))
    {
        vkd3d_atomic_uint32_increment(&chain->stall.pending_latency_count,
                vkd3d_memory_order_relaxed);
    }
}

static void dxgi_vk_swap_chain_check_latency_stall(
        struct dxgi_vk_swap_chain *chain)
{
    uint64_t now, last_present, age;
    uint32_t pending, internal_waits, present_done;

    pending = vkd3d_atomic_uint32_load_explicit(
            &chain->stall.pending_latency_count, vkd3d_memory_order_relaxed);
    internal_waits = vkd3d_atomic_uint32_load_explicit(
            &chain->frame_latency_event_internal_wait_counts,
            vkd3d_memory_order_relaxed);
    present_done = vkd3d_atomic_uint32_load_explicit(
            &chain->stall.present_done_pending, vkd3d_memory_order_relaxed);
    if (!pending && !internal_waits && !present_done)
        return;

    now = vkd3d_get_current_time_ns();
    last_present = vkd3d_atomic_uint64_load_explicit(
            &chain->stall.last_present_time_ns, vkd3d_memory_order_relaxed);
    age = last_present && now >= last_present ? now - last_present : 0;
    if (age < DXGI_VK_STALL_TIMEOUT_NS ||
            vkd3d_atomic_uint32_compare_exchange(&chain->stall.reported, 0, 1,
            vkd3d_memory_order_relaxed, vkd3d_memory_order_relaxed))
        return;

    ERR("WINEPS5 vkd3d stall kind=latency chain=%p pending=%u"
            " internal_waits=%u present_done=%u presents=%"PRIu64
            " release_semaphore=%"PRIu64" release_fail=%"PRIu64
            " internal_release_semaphore=%"PRIu64" internal_release_fail=%"PRIu64
            " set_event=%"PRIu64" set_event_fail=%"PRIu64
            " age_ms=%"PRIu64" last_present_ns=%"PRIu64"\n",
            chain, pending, internal_waits, present_done,
            vkd3d_atomic_uint64_load_explicit(&chain->stall.present_count,
                    vkd3d_memory_order_relaxed),
            vkd3d_atomic_uint64_load_explicit(
                    &chain->stall.release_semaphore_count,
                    vkd3d_memory_order_relaxed),
            vkd3d_atomic_uint64_load_explicit(
                    &chain->stall.release_semaphore_fail_count,
                    vkd3d_memory_order_relaxed),
            vkd3d_atomic_uint64_load_explicit(
                    &chain->stall.internal_release_semaphore_count,
                    vkd3d_memory_order_relaxed),
            vkd3d_atomic_uint64_load_explicit(
                    &chain->stall.internal_release_semaphore_fail_count,
                    vkd3d_memory_order_relaxed),
            vkd3d_atomic_uint64_load_explicit(&chain->stall.set_event_count,
                    vkd3d_memory_order_relaxed),
            vkd3d_atomic_uint64_load_explicit(&chain->stall.set_event_fail_count,
                    vkd3d_memory_order_relaxed),
            age / 1000000, last_present);
}

static bool dxgi_vk_swap_chain_hdr_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static void dxgi_vk_swap_chain_hdr_settings_off(struct dxgi_vk_hdr_settings *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->mode = VKD3D_SWAPCHAIN_HDR_MODE_OFF;
    settings->saturation = 1.0f;
    settings->gamma = 2.2f;
}

static bool dxgi_vk_swap_chain_hdr_parse_float(const char *text, float *value)
{
    char *end;
    float parsed;

    parsed = strtof(text, &end);
    if (end == text || *end || !isfinite(parsed))
        return false;
    *value = parsed;
    return true;
}

static bool dxgi_vk_swap_chain_hdr_parse_shader_list(const char *text,
        uint64_t shaders[VKD3D_HDR_AUTO_SHADER_COUNT], uint32_t *shader_count)
{
    const char *p = text;
    uint32_t count = 0;

    while (*p)
    {
        const char *comma = strchr(p, ',');
        size_t length = comma ? (size_t)(comma - p) : strlen(p);
        uint64_t hash = 0;
        unsigned int i, j;

        if (count == VKD3D_HDR_AUTO_SHADER_COUNT || length != 16)
            return false;
        for (i = 0; i < length; i++)
        {
            unsigned int digit;
            char c = p[i];

            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (c >= 'a' && c <= 'f')
                digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                digit = c - 'A' + 10;
            else
                return false;
            hash = (hash << 4) | digit;
        }
        if (!hash)
            return false;
        for (j = 0; j < count; j++)
            if (shaders[j] == hash)
                return false;
        shaders[count++] = hash;

        if (!comma)
            break;
        p = comma + 1;
        if (!*p)
            return false;
    }

    if (!count)
        return false;
    *shader_count = count;
    return true;
}

static bool dxgi_vk_swap_chain_hdr_shader_lists_disjoint(
        const struct dxgi_vk_hdr_settings *settings)
{
    unsigned int i, j;

    for (i = 0; i < settings->sdr_shader_count; i++)
        for (j = 0; j < settings->hdr_shader_count; j++)
            if (settings->sdr_shaders[i] == settings->hdr_shaders[j])
                return false;
    return true;
}

static bool dxgi_vk_swap_chain_parse_hdr_settings(char *text,
        struct dxgi_vk_hdr_settings *settings)
{
    enum
    {
        HDR_SEEN_MODE = 1u << 0,
        HDR_SEEN_PAPER = 1u << 1,
        HDR_SEEN_PEAK = 1u << 2,
        HDR_SEEN_EXPAND = 1u << 3,
        HDR_SEEN_SAT = 1u << 4,
        HDR_SEEN_GAMMA = 1u << 5,
        HDR_SEEN_MAX = 1u << 6,
        HDR_SEEN_SDR_SHADERS = 1u << 7,
        HDR_SEEN_HDR_SHADERS = 1u << 8,
        HDR_SEEN_HDR_CAP = 1u << 9,
        HDR_SEEN_KEY = 1u << 10,
    };
    float max_nits = 0.0f;
    uint32_t seen = 0;
    char *p = text;

    memset(settings, 0, sizeof(*settings));
    settings->paper_nits = 203.0f;
    settings->peak_nits = 637.0f;
    settings->expansion = 1.0f;
    settings->saturation = 1.0f;
    settings->gamma = 2.2f;

    for (;;)
    {
        char *token, *value;

        while (dxgi_vk_swap_chain_hdr_space(*p))
            p++;
        if (!*p)
            break;

        token = p;
        while (*p && !dxgi_vk_swap_chain_hdr_space(*p))
            p++;
        if (*p)
            *p++ = '\0';

        if (!(value = strchr(token, '=')) || value == token || !value[1] ||
                strchr(value + 1, '='))
            return false;
        *value++ = '\0';

        if (!strcmp(token, "mode"))
        {
            if (seen & HDR_SEEN_MODE)
                return false;
            seen |= HDR_SEEN_MODE;
            if (!strcmp(value, "off"))
                settings->mode = VKD3D_SWAPCHAIN_HDR_MODE_OFF;
            else if (!strcmp(value, "cap"))
                settings->mode = VKD3D_SWAPCHAIN_HDR_MODE_CAP;
            else if (!strcmp(value, "sdr"))
                settings->mode = VKD3D_SWAPCHAIN_HDR_MODE_SDR;
            else if (!strcmp(value, "auto"))
                settings->mode = VKD3D_SWAPCHAIN_HDR_MODE_AUTO;
            else
                return false;
        }
        else if (!strcmp(token, "paper"))
        {
            if (seen & HDR_SEEN_PAPER ||
                    !dxgi_vk_swap_chain_hdr_parse_float(value, &settings->paper_nits))
                return false;
            seen |= HDR_SEEN_PAPER;
        }
        else if (!strcmp(token, "peak"))
        {
            if (seen & HDR_SEEN_PEAK ||
                    !dxgi_vk_swap_chain_hdr_parse_float(value, &settings->peak_nits))
                return false;
            seen |= HDR_SEEN_PEAK;
        }
        else if (!strcmp(token, "expand"))
        {
            if (seen & HDR_SEEN_EXPAND ||
                    !dxgi_vk_swap_chain_hdr_parse_float(value, &settings->expansion))
                return false;
            seen |= HDR_SEEN_EXPAND;
        }
        else if (!strcmp(token, "sat"))
        {
            if (seen & HDR_SEEN_SAT ||
                    !dxgi_vk_swap_chain_hdr_parse_float(value, &settings->saturation))
                return false;
            seen |= HDR_SEEN_SAT;
        }
        else if (!strcmp(token, "gamma"))
        {
            if (seen & HDR_SEEN_GAMMA)
                return false;
            if (!strcmp(value, "srgb"))
                settings->gamma = 0.0f;
            else if (!dxgi_vk_swap_chain_hdr_parse_float(value, &settings->gamma))
                return false;
            seen |= HDR_SEEN_GAMMA;
        }
        else if (!strcmp(token, "max"))
        {
            if (seen & HDR_SEEN_MAX ||
                    !dxgi_vk_swap_chain_hdr_parse_float(value, &max_nits))
                return false;
            seen |= HDR_SEEN_MAX;
        }
        else if (!strcmp(token, "sdr_shaders"))
        {
            if (seen & HDR_SEEN_SDR_SHADERS ||
                    !dxgi_vk_swap_chain_hdr_parse_shader_list(value,
                    settings->sdr_shaders, &settings->sdr_shader_count))
                return false;
            seen |= HDR_SEEN_SDR_SHADERS;
        }
        else if (!strcmp(token, "hdr_shaders"))
        {
            if (seen & HDR_SEEN_HDR_SHADERS ||
                    !dxgi_vk_swap_chain_hdr_parse_shader_list(value,
                    settings->hdr_shaders, &settings->hdr_shader_count))
                return false;
            seen |= HDR_SEEN_HDR_SHADERS;
        }
        else if (!strcmp(token, "hdr_cap"))
        {
            if (seen & HDR_SEEN_HDR_CAP ||
                    !dxgi_vk_swap_chain_hdr_parse_float(value, &settings->hdr_cap_nits))
                return false;
            seen |= HDR_SEEN_HDR_CAP;
        }
        else if (!strcmp(token, "key"))
        {
            if (seen & HDR_SEEN_KEY)
                return false;
            if (!strcmp(value, "y"))
                settings->expansion_key = DXGI_VK_HDR_EXPANSION_KEY_Y;
            else if (!strcmp(value, "max"))
                settings->expansion_key = DXGI_VK_HDR_EXPANSION_KEY_MAX;
            else
                return false;
            seen |= HDR_SEEN_KEY;
        }
        else
        {
            return false;
        }
    }

    if (!(seen & HDR_SEEN_MODE))
        return false;

    if (settings->mode == VKD3D_SWAPCHAIN_HDR_MODE_OFF)
    {
        if (seen != HDR_SEEN_MODE)
            return false;
        dxgi_vk_swap_chain_hdr_settings_off(settings);
        return true;
    }

    if (settings->mode == VKD3D_SWAPCHAIN_HDR_MODE_CAP)
    {
        if (seen != (HDR_SEEN_MODE | HDR_SEEN_MAX) ||
                max_nits < 80.0f || max_nits > 10000.0f)
            return false;
        dxgi_vk_swap_chain_hdr_settings_off(settings);
        settings->mode = VKD3D_SWAPCHAIN_HDR_MODE_CAP;
        settings->peak_nits = max_nits;
        return true;
    }

    if ((seen & HDR_SEEN_MAX) ||
            (settings->mode != VKD3D_SWAPCHAIN_HDR_MODE_AUTO &&
            (seen & (HDR_SEEN_SDR_SHADERS | HDR_SEEN_HDR_SHADERS | HDR_SEEN_HDR_CAP))) ||
            (settings->mode == VKD3D_SWAPCHAIN_HDR_MODE_AUTO &&
            (((seen & (HDR_SEEN_SDR_SHADERS | HDR_SEEN_HDR_SHADERS)) !=
            (HDR_SEEN_SDR_SHADERS | HDR_SEEN_HDR_SHADERS)) ||
            !dxgi_vk_swap_chain_hdr_shader_lists_disjoint(settings))) ||
            settings->paper_nits < 40.0f || settings->paper_nits > 1000.0f ||
            settings->peak_nits < 80.0f || settings->peak_nits > 10000.0f ||
            settings->peak_nits < settings->paper_nits ||
            settings->expansion < 0.0f || settings->expansion > 1.0f ||
            settings->saturation < 0.0f || settings->saturation > 2.0f ||
            (settings->gamma != 0.0f &&
            (settings->gamma < 1.0f || settings->gamma > 3.0f)) ||
            (settings->hdr_cap_nits != 0.0f &&
            (settings->hdr_cap_nits < 80.0f || settings->hdr_cap_nits > 10000.0f)))
        return false;

    return true;
}

static bool dxgi_vk_swap_chain_read_hdr_settings_file(
        struct dxgi_vk_hdr_settings *settings)
{
    char text[512];
    size_t size;
    FILE *file;
    bool valid;

    if (!(file = fopen("C:\\wineps5-hdr.txt", "rb")))
        return false;

    size = fread(text, 1, sizeof(text), file);
    valid = !ferror(file) && size < sizeof(text) && !memchr(text, '\0', size);
    fclose(file);
    if (!valid)
        return false;

    text[size] = '\0';
    return dxgi_vk_swap_chain_parse_hdr_settings(text, settings);
}

static float dxgi_vk_swap_chain_hdr_pq_encode(float nits)
{
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    float p = powf(nits / 10000.0f, m1);

    return powf((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

static void dxgi_vk_swap_chain_prepare_sdr_push(
        const struct dxgi_vk_hdr_settings *settings,
        struct dxgi_vk_hdr_push_constants *push)
{
    float cap_pq;

    memset(push, 0, sizeof(*push));
    push->paper_nits = settings->paper_nits;
    push->peak_nits = settings->peak_nits;
    push->expansion = settings->expansion;
    push->saturation = settings->saturation;
    push->gamma = settings->gamma;
    push->use_luminance =
            settings->expansion_key == DXGI_VK_HDR_EXPANSION_KEY_Y ? 1.0f : 0.0f;
    push->knee_nits = 0.75f * settings->paper_nits;
    cap_pq = dxgi_vk_swap_chain_hdr_pq_encode(settings->peak_nits);
    push->cap_pq = floorf(cap_pq * 1023.0f) / 1023.0f;
}

static void dxgi_vk_swap_chain_prepare_cap_push(float peak_nits,
        struct dxgi_vk_hdr_push_constants *push)
{
    float cap_pq;

    memset(push, 0, sizeof(*push));
    push->peak_nits = peak_nits;
    push->saturation = 1.0f;
    push->gamma = 2.2f;
    push->knee_nits = 0.75f * peak_nits;
    push->knee_pq = dxgi_vk_swap_chain_hdr_pq_encode(push->knee_nits);
    cap_pq = dxgi_vk_swap_chain_hdr_pq_encode(peak_nits);
    push->cap_pq = floorf(cap_pq * 1023.0f) / 1023.0f;
}

static const char *dxgi_vk_swap_chain_hdr_source_name(enum dxgi_vk_hdr_source source)
{
    switch (source)
    {
        case DXGI_VK_HDR_SOURCE_ENV:
            return "env";
        case DXGI_VK_HDR_SOURCE_FILE:
            return "file";
        default:
            return "off";
    }
}

static const char *dxgi_vk_swap_chain_hdr_mode_name(enum vkd3d_swapchain_hdr_mode mode)
{
    switch (mode)
    {
        case VKD3D_SWAPCHAIN_HDR_MODE_CAP:
            return "cap";
        case VKD3D_SWAPCHAIN_HDR_MODE_SDR:
            return "sdr";
        case VKD3D_SWAPCHAIN_HDR_MODE_AUTO:
            return "auto";
        default:
            return "off";
    }
}

static bool dxgi_vk_swap_chain_hdr_settings_equal(
        const struct dxgi_vk_hdr_settings *a, const struct dxgi_vk_hdr_settings *b)
{
    return a->mode == b->mode &&
            a->paper_nits == b->paper_nits &&
            a->peak_nits == b->peak_nits &&
            a->expansion == b->expansion &&
            a->saturation == b->saturation &&
            a->gamma == b->gamma &&
            a->expansion_key == b->expansion_key &&
            a->hdr_cap_nits == b->hdr_cap_nits &&
            a->sdr_shader_count == b->sdr_shader_count &&
            a->hdr_shader_count == b->hdr_shader_count &&
            !memcmp(a->sdr_shaders, b->sdr_shaders, sizeof(a->sdr_shaders)) &&
            !memcmp(a->hdr_shaders, b->hdr_shaders, sizeof(a->hdr_shaders));
}

static void dxgi_vk_swap_chain_update_hdr_settings(struct dxgi_vk_swap_chain *chain)
{
    struct dxgi_vk_hdr_settings file_settings, settings, old_settings;
    enum dxgi_vk_hdr_source old_source, source;
    char env[512];
    uint64_t now;

    now = vkd3d_get_current_time_ns();
    if (chain->present.hdr.source != DXGI_VK_HDR_SOURCE_UNINITIALIZED &&
            now < chain->present.hdr.next_file_check_ns)
        return;
    chain->present.hdr.next_file_check_ns = now + 1000000000ull;

    old_source = chain->present.hdr.source;
    old_settings = chain->present.hdr.effective;
    if (old_source == DXGI_VK_HDR_SOURCE_UNINITIALIZED &&
            vkd3d_get_env_var("WINEPS5_HDR_MODE", env, sizeof(env)))
    {
        chain->present.hdr.env_valid =
                dxgi_vk_swap_chain_parse_hdr_settings(env, &chain->present.hdr.env);
    }

    if (dxgi_vk_swap_chain_read_hdr_settings_file(&file_settings))
    {
        settings = file_settings;
        source = DXGI_VK_HDR_SOURCE_FILE;
    }
    else if (chain->present.hdr.env_valid)
    {
        settings = chain->present.hdr.env;
        source = DXGI_VK_HDR_SOURCE_ENV;
    }
    else
    {
        dxgi_vk_swap_chain_hdr_settings_off(&settings);
        source = DXGI_VK_HDR_SOURCE_OFF;
    }

    chain->present.hdr.source = source;
    chain->present.hdr.effective = settings;
    if (source != old_source ||
            !dxgi_vk_swap_chain_hdr_settings_equal(&settings, &old_settings))
    {
        if (settings.mode == VKD3D_SWAPCHAIN_HDR_MODE_CAP)
        {
            dxgi_vk_swap_chain_prepare_cap_push(
                    settings.peak_nits, &chain->present.hdr.push);
        }
        else if (settings.mode == VKD3D_SWAPCHAIN_HDR_MODE_SDR ||
                settings.mode == VKD3D_SWAPCHAIN_HDR_MODE_AUTO)
        {
            dxgi_vk_swap_chain_prepare_sdr_push(
                    &settings, &chain->present.hdr.push);
        }
        else
        {
            memset(&chain->present.hdr.push, 0, sizeof(chain->present.hdr.push));
        }

        if (settings.mode == VKD3D_SWAPCHAIN_HDR_MODE_AUTO &&
                settings.hdr_cap_nits != 0.0f)
        {
            dxgi_vk_swap_chain_prepare_cap_push(
                    settings.hdr_cap_nits, &chain->present.hdr.auto_cap_push);
        }
        else
        {
            memset(&chain->present.hdr.auto_cap_push, 0,
                    sizeof(chain->present.hdr.auto_cap_push));
        }

        dxgi_vk_swap_chain_configure_hdr_auto(&settings);
        chain->present.hdr.auto_content = VKD3D_HDR_AUTO_UNKNOWN;
        chain->present.hdr.auto_candidate = VKD3D_HDR_AUTO_UNKNOWN;
        chain->present.hdr.auto_candidate_count = 0;
        chain->present.hdr.auto_candidate_writer = 0;

        ERR("WINEPS5 vkd3d hdr mode=%s paper=%.6g peak=%.6g expand=%.6g sat=%.6g key=%s source=%s\n",
                dxgi_vk_swap_chain_hdr_mode_name(settings.mode),
                (double)settings.paper_nits, (double)settings.peak_nits,
                (double)settings.expansion, (double)settings.saturation,
                settings.expansion_key == DXGI_VK_HDR_EXPANSION_KEY_Y ? "y" : "max",
                dxgi_vk_swap_chain_hdr_source_name(source));
    }
}

static void dxgi_vk_swap_chain_hdr_auto_log_unknown(
        struct dxgi_vk_swap_chain *chain, uint64_t writer)
{
    unsigned int i;

    if (!writer)
        return;
    for (i = 0; i < chain->present.hdr.auto_unknown_writer_count; i++)
        if (chain->present.hdr.auto_unknown_writers[i] == writer)
            return;
    if (chain->present.hdr.auto_unknown_writer_count ==
            ARRAY_SIZE(chain->present.hdr.auto_unknown_writers))
        return;

    chain->present.hdr.auto_unknown_writers[
            chain->present.hdr.auto_unknown_writer_count++] = writer;
    ERR("WINEPS5 vkd3d hdr auto unknown_writer=%016"PRIx64"\n", writer);
}

static enum vkd3d_hdr_auto_match dxgi_vk_swap_chain_hdr_auto_observe(
        struct dxgi_vk_swap_chain *chain, uint64_t *writer)
{
    /* PS5: Match any configured output writer seen in the frame, rather than
     * the literal last writer, since WoW can draw UI after the world output.
     * If a transition frame contains both classes, real PQ wins safely. */
    if (chain->request.hdr_auto_hdr_writer_hash)
    {
        *writer = chain->request.hdr_auto_hdr_writer_hash;
        return VKD3D_HDR_AUTO_HDR;
    }
    if (chain->request.hdr_auto_sdr_writer_hash)
    {
        *writer = chain->request.hdr_auto_sdr_writer_hash;
        return VKD3D_HDR_AUTO_SDR;
    }

    *writer = chain->request.hdr_auto_last_writer_hash;
    dxgi_vk_swap_chain_hdr_auto_log_unknown(chain, *writer);
    return VKD3D_HDR_AUTO_UNKNOWN;
}

static enum vkd3d_hdr_auto_match dxgi_vk_swap_chain_hdr_auto_update(
        struct dxgi_vk_swap_chain *chain,
        enum vkd3d_hdr_auto_match observed, uint64_t writer)
{
    if (observed == VKD3D_HDR_AUTO_UNKNOWN)
    {
        chain->present.hdr.auto_candidate = VKD3D_HDR_AUTO_UNKNOWN;
        chain->present.hdr.auto_candidate_count = 0;
        chain->present.hdr.auto_candidate_writer = 0;
        return VKD3D_HDR_AUTO_UNKNOWN;
    }

    if (observed == chain->present.hdr.auto_content)
    {
        chain->present.hdr.auto_candidate = VKD3D_HDR_AUTO_UNKNOWN;
        chain->present.hdr.auto_candidate_count = 0;
        chain->present.hdr.auto_candidate_writer = 0;
        return observed;
    }

    if (observed != chain->present.hdr.auto_candidate)
    {
        chain->present.hdr.auto_candidate = observed;
        chain->present.hdr.auto_candidate_count = 1;
    }
    else
    {
        chain->present.hdr.auto_candidate_count++;
    }
    chain->present.hdr.auto_candidate_writer = writer;

    if (chain->present.hdr.auto_candidate_count >=
            DXGI_VK_HDR_AUTO_STABLE_PRESENTS)
    {
        chain->present.hdr.auto_content = observed;
        ERR("WINEPS5 vkd3d hdr auto content=%s writer=%016"PRIx64"\n",
                observed == VKD3D_HDR_AUTO_SDR ? "sdr" : "hdr",
                chain->present.hdr.auto_candidate_writer);
        chain->present.hdr.auto_candidate = VKD3D_HDR_AUTO_UNKNOWN;
        chain->present.hdr.auto_candidate_count = 0;
        chain->present.hdr.auto_candidate_writer = 0;
    }

    return chain->present.hdr.auto_content;
}

static void dxgi_vk_swap_chain_drain_internal_blit_semaphore(struct dxgi_vk_swap_chain *chain, uint64_t value);

static void dxgi_vk_swap_chain_wait_acquire_semaphore(struct dxgi_vk_swap_chain *chain,
        VkSemaphore vk_semaphore, bool blocking)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkSemaphoreSubmitInfo signal_info;
    VkSemaphoreSubmitInfo wait_info;
    VkSubmitInfo2 submit_info;
    VkQueue vk_queue;
    VkResult vr;

    memset(&submit_info, 0, sizeof(submit_info));
    memset(&wait_info, 0, sizeof(wait_info));
    memset(&signal_info, 0, sizeof(signal_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit_info.waitSemaphoreInfoCount = 1;
    submit_info.pWaitSemaphoreInfos = &wait_info;

    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_info.semaphore = vk_semaphore;
    wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;

    submit_info.pSignalSemaphoreInfos = &signal_info;
    submit_info.signalSemaphoreInfoCount = 1;
    signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_info.semaphore = chain->present.vk_internal_blit_semaphore;
    signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signal_info.value = ++chain->present.internal_blit_count;

    vk_queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
    vr = VK_CALL(vkQueueSubmit2(vk_queue, 1, &submit_info, VK_NULL_HANDLE));
    if (vr < 0)
    {
        ERR("Failed to submit, vr %d\n", vr);
        VKD3D_DEVICE_REPORT_FAULT_AND_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);
    }
    vkd3d_queue_release(chain->queue->vkd3d_queue);

    if (vr == VK_SUCCESS && blocking)
        dxgi_vk_swap_chain_drain_internal_blit_semaphore(chain, chain->present.internal_blit_count);
}

static void dxgi_vk_swap_chain_ensure_unsignaled_swapchain_fence(struct dxgi_vk_swap_chain *chain, uint32_t index)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkFenceCreateInfo fence_info;
    VkResult vr;

    if (chain->present.vk_swapchain_fences[index])
    {
        if (chain->present.vk_swapchain_fences_signalled[index])
        {
            vr = VK_CALL(vkWaitForFences(chain->queue->device->vk_device,
                    1, &chain->present.vk_swapchain_fences[index], VK_TRUE, UINT64_MAX));

            if (vr)
            {
                ERR("Failed to wait for fences, vr %d\n", vr);
            }
            else
            {
                VK_CALL(vkResetFences(chain->queue->device->vk_device,
                        1, &chain->present.vk_swapchain_fences[index]));
                chain->present.vk_swapchain_fences_signalled[index] = false;
            }
        }
    }
    else
    {
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.pNext = NULL;
        fence_info.flags = 0;
        vr = VK_CALL(vkCreateFence(chain->queue->device->vk_device,
                &fence_info, NULL, &chain->present.vk_swapchain_fences[index]));
        if (vr)
            ERR("Failed to create swapchain fence, vr %d.\n", vr);
    }
}

static void dxgi_vk_swap_chain_drain_swapchain_fences(struct dxgi_vk_swap_chain *chain)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_swapchain_fences); i++)
        if (chain->present.vk_swapchain_fences_signalled[i])
            dxgi_vk_swap_chain_ensure_unsignaled_swapchain_fence(chain, i);
}

static void dxgi_vk_swap_chain_wait_semaphore(struct dxgi_vk_swap_chain *chain,
        VkSemaphore vk_timeline, uint64_t value)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkSemaphoreWaitInfo wait_info;
    VkResult vr;

    if (!value)
        return;

    memset(&wait_info, 0, sizeof(wait_info));
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.pSemaphores = &vk_timeline;
    wait_info.pValues = &value;
    wait_info.semaphoreCount = 1;
    vr = VK_CALL(vkWaitSemaphores(chain->queue->device->vk_device, &wait_info, UINT64_MAX));
    if (vr)
        ERR("Failed to wait for present semaphore, vr %d.\n", vr);
}

static void dxgi_vk_swap_chain_drain_complete_semaphore(struct dxgi_vk_swap_chain *chain, uint64_t value)
{
    dxgi_vk_swap_chain_wait_semaphore(chain, chain->present.vk_complete_semaphore, value);
}

static void dxgi_vk_swap_chain_drain_internal_blit_semaphore(struct dxgi_vk_swap_chain *chain, uint64_t value)
{
    dxgi_vk_swap_chain_wait_semaphore(chain, chain->present.vk_internal_blit_semaphore, value);
}

static void dxgi_vk_swap_chain_drain_user_images(struct dxgi_vk_swap_chain *chain)
{
    dxgi_vk_swap_chain_drain_complete_semaphore(chain, chain->user.blit_count);
    dxgi_vk_swap_chain_fg_stop(chain);
}

static void dxgi_vk_swap_chain_drain_queue(struct dxgi_vk_swap_chain *chain)
{
    unsigned int i;

    /* This functions as a DRAIN of the D3D12 queue.
     * All CPU operations that were queued must have been submitted to Vulkan now.
     * We intend to submit directly to the queue now and timeline signals must come in the proper order. */
    if (vkd3d_acquire_vk_queue(&chain->queue->ID3D12CommandQueue_iface))
        vkd3d_release_vk_queue(&chain->queue->ID3D12CommandQueue_iface);
    dxgi_vk_swap_chain_fg_stop(chain);

    /* If we have a lingering semaphore acquire that never went anywhere, ensure it is waited on. */
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_acquire_semaphore); i++)
        if (chain->present.vk_acquire_semaphore[i] && chain->present.acquire_semaphore_signalled[i])
            dxgi_vk_swap_chain_wait_acquire_semaphore(chain, chain->present.vk_acquire_semaphore[i], false);

    /* Wait for pending blits to complete on the GPU */
    dxgi_vk_swap_chain_drain_complete_semaphore(chain, chain->user.present_count);

    if (chain->swapchain_maintenance1)
        dxgi_vk_swap_chain_drain_swapchain_fences(chain);
}

static void dxgi_vk_swap_chain_push_present_id(struct dxgi_vk_swap_chain *chain,
        uint64_t present_count, uint64_t present_id, uint64_t begin_frame_time_ns, uint64_t fg_present_ns)
{
    struct present_wait_entry *entry;
    pthread_mutex_lock(&chain->wait_thread.lock);
    vkd3d_array_reserve((void **)&chain->wait_thread.wait_queue, &chain->wait_thread.wait_queue_size,
            chain->wait_thread.wait_queue_count + 1, sizeof(*chain->wait_thread.wait_queue));
    entry = &chain->wait_thread.wait_queue[chain->wait_thread.wait_queue_count++];
    entry->id = present_id;
    entry->present_count = present_count;
    entry->begin_frame_time_ns = begin_frame_time_ns;
    entry->fg_present_ns = fg_present_ns;
    condvar_reltime_signal(&chain->wait_thread.cond);
    pthread_mutex_unlock(&chain->wait_thread.lock);
}

static void dxgi_vk_swap_chain_push_display_latency(struct dxgi_vk_swap_chain *chain, uint64_t id)
{
    pthread_mutex_lock(&chain->wait_thread.lock);
    if (!chain->wait_thread.skip_waits &&
            wineps5_display_latency_push(&chain->wait_thread.display_latency,id,chain->request.present_time_ns,
                    chain->present.fg.running))
        condvar_reltime_signal(&chain->wait_thread.cond);
    pthread_mutex_unlock(&chain->wait_thread.lock);
}

static void dxgi_vk_swap_chain_cleanup_frame_rate_limiter(struct dxgi_vk_swap_chain *chain)
{
    pthread_mutex_destroy(&chain->frame_rate_limit.lock);
}

static void dxgi_vk_swap_chain_cleanup_low_latency(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    if (chain->queue->device->vk_info.NV_low_latency2)
    {
        VK_CALL(vkDestroySemaphore(chain->queue->device->vk_device, chain->present.low_latency_sem, NULL));
        pthread_mutex_destroy(&chain->present.low_latency_swapchain_lock);
        pthread_mutex_destroy(&chain->present.low_latency_state_update_lock);
    }
}

static void dxgi_vk_swap_chain_cleanup_waiter_thread(struct dxgi_vk_swap_chain *chain)
{
    dxgi_vk_swap_chain_push_present_id(chain, 0, 0, 0, 0);
    pthread_join(chain->wait_thread.thread, NULL);
    pthread_mutex_destroy(&chain->wait_thread.lock);
    condvar_reltime_destroy(&chain->wait_thread.cond);
    vkd3d_free(chain->wait_thread.wait_queue);
}

static void dxgi_vk_swap_chain_cleanup_surface(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VK_CALL(vkDestroySurfaceKHR(chain->queue->device->vkd3d_instance->vk_instance,
            chain->vk_surface, NULL));
    vkd3d_free(chain->properties.formats);
    pthread_mutex_destroy(&chain->properties.lock);
}

static void dxgi_vk_swap_chain_cleanup_sync_objects(struct dxgi_vk_swap_chain *chain)
{
    vkd3d_native_sync_handle_destroy(chain->frame_latency_event);
    vkd3d_native_sync_handle_destroy(chain->frame_latency_event_internal);

    if (chain->outstanding_present_request)
    {
        while (!vkd3d_native_sync_handle_acquire_timeout(
                chain->present_request_done_event, 1000))
            dxgi_vk_swap_chain_check_latency_stall(chain);
        vkd3d_atomic_uint32_store_explicit(&chain->stall.present_done_pending,
                0, vkd3d_memory_order_relaxed);
        chain->outstanding_present_request = false;
    }
    vkd3d_native_sync_handle_destroy(chain->present_request_done_event);
}

static void dxgi_vk_swap_chain_cleanup_common(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    unsigned int i;

    dxgi_vk_swap_chain_fg_stop(chain);
    dxgi_vk_swap_chain_hud_cleanup(chain);
    VK_CALL(vkDestroySemaphore(chain->queue->device->vk_device, chain->present.vk_internal_blit_semaphore, NULL));
    VK_CALL(vkDestroySemaphore(chain->queue->device->vk_device, chain->present.vk_complete_semaphore, NULL));
    VK_CALL(vkDestroyCommandPool(chain->queue->device->vk_device, chain->present.vk_blit_command_pool, NULL));
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_release_semaphores); i++)
        VK_CALL(vkDestroySemaphore(chain->queue->device->vk_device, chain->present.vk_release_semaphores[i], NULL));
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_backbuffer_image_views); i++)
        VK_CALL(vkDestroyImageView(chain->queue->device->vk_device, chain->present.vk_backbuffer_image_views[i], NULL));
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_acquire_semaphore); i++)
        VK_CALL(vkDestroySemaphore(chain->queue->device->vk_device, chain->present.vk_acquire_semaphore[i], NULL));
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_swapchain_fences); i++)
        VK_CALL(vkDestroyFence(chain->queue->device->vk_device, chain->present.vk_swapchain_fences[i], NULL));

    VK_CALL(vkDestroySwapchainKHR(chain->queue->device->vk_device, chain->present.vk_swapchain, NULL));

    for (i = 0; i < ARRAY_SIZE(chain->user.backbuffers); i++)
    {
        if (chain->user.backbuffers[i])
            vkd3d_resource_decref((ID3D12Resource *)&chain->user.backbuffers[i]->ID3D12Resource_iface);
        VK_CALL(vkDestroyImageView(chain->queue->device->vk_device, chain->user.vk_image_views[i], NULL));
    }
}

static void dxgi_vk_swap_chain_cleanup(struct dxgi_vk_swap_chain *chain)
{
    /* Join with waiter thread first to avoid any sync issues. */
    dxgi_vk_swap_chain_cleanup_waiter_thread(chain);

    dxgi_vk_swap_chain_cleanup_frame_rate_limiter(chain);
    dxgi_vk_swap_chain_cleanup_low_latency(chain);
    dxgi_vk_swap_chain_cleanup_sync_objects(chain);
    dxgi_vk_swap_chain_cleanup_common(chain);

    /* Surface must be destroyed after swapchain. */
    dxgi_vk_swap_chain_cleanup_surface(chain);
}

static inline struct dxgi_vk_swap_chain *impl_from_IDXGIVkSwapChain(IDXGIVkSwapChain2 *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_vk_swap_chain, IDXGIVkSwapChain_iface);
}

static ULONG STDMETHODCALLTYPE dxgi_vk_swap_chain_AddRef(IDXGIVkSwapChain2 *iface)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    UINT refcount = InterlockedIncrement(&chain->refcount);

    TRACE("iface %p, refcount %u\n", iface, refcount);

    if (refcount == 1)
    {
        dxgi_vk_swap_chain_incref(chain);
        ID3D12CommandQueue_AddRef(&chain->queue->ID3D12CommandQueue_iface);
        d3d12_device_register_swapchain(chain->queue->device, chain);
    }

    return refcount;
}

static ULONG STDMETHODCALLTYPE dxgi_vk_swap_chain_Release(IDXGIVkSwapChain2 *iface)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    struct d3d12_device *device = chain->queue->device;
    struct d3d12_command_queue *queue = chain->queue;
    UINT refcount;

    refcount = InterlockedDecrement(&chain->refcount);
    TRACE("iface %p, refcount %u\n", iface, refcount);

    if (!refcount)
    {
        /* Calling this from the submission thread will result in a deadlock, so
         * drain the swapchain queue now. */
        dxgi_vk_swap_chain_drain_queue(chain);

        if (device->vk_info.NV_low_latency2)
            d3d12_device_remove_swapchain(device, chain);

        dxgi_vk_swap_chain_decref(chain);
        ID3D12CommandQueue_Release(&queue->ID3D12CommandQueue_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_QueryInterface(IDXGIVkSwapChain2 *iface, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    TRACE("iface %p\n", iface);
    if (IsEqualGUID(riid, &IID_IUnknown) ||
            IsEqualGUID(riid, &IID_IDXGIVkSwapChain) ||
            IsEqualGUID(riid, &IID_IDXGIVkSwapChain1) ||
            IsEqualGUID(riid, &IID_IDXGIVkSwapChain2))
    {
        dxgi_vk_swap_chain_AddRef(&chain->IDXGIVkSwapChain_iface);
        *object = iface;
        return S_OK;
    }

    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetDesc(IDXGIVkSwapChain2 *iface, DXGI_SWAP_CHAIN_DESC1 *pDesc)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    TRACE("iface %p, pDesc %p\n", iface, pDesc);
    *pDesc = chain->desc;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetAdapter(IDXGIVkSwapChain2 *iface, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    TRACE("iface %p\n", iface);
    return IUnknown_QueryInterface(chain->queue->device->parent, riid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetDevice(IDXGIVkSwapChain2 *iface, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    TRACE("iface %p\n", iface);
    return ID3D12Device12_QueryInterface(&chain->queue->device->ID3D12Device_iface, riid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetImage(IDXGIVkSwapChain2 *iface, UINT BufferId, REFIID riid, void **object)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    HRESULT hr;

    TRACE("iface %p, BufferId %u.\n", iface, BufferId);
    if (BufferId >= chain->desc.BufferCount)
    {
        vkd3d_ps5_d3d12_fail("SwapChain::GetBuffer", E_INVALIDARG,
                "buffer=%u buffer_count=%u format=%u size=%ux%u",
                BufferId, chain->desc.BufferCount, chain->desc.Format,
                chain->desc.Width, chain->desc.Height);
        return E_INVALIDARG;
    }

    hr = ID3D12Resource2_QueryInterface(
            &chain->user.backbuffers[BufferId]->ID3D12Resource_iface, riid, object);
    if (FAILED(hr))
    {
        vkd3d_ps5_d3d12_fail("SwapChain::GetBuffer", hr,
                "buffer=%u buffer_count=%u format=%u size=%ux%u",
                BufferId, chain->desc.BufferCount, chain->desc.Format,
                chain->desc.Width, chain->desc.Height);
    }
    return hr;
}

static UINT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetImageIndex(IDXGIVkSwapChain2 *iface)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    TRACE("iface %p.\n", iface);
    return chain->user.index;
}

static UINT STDMETHODCALLTYPE dxgi_vk_swap_chain_GetFrameLatency(IDXGIVkSwapChain2 *iface)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    TRACE("iface %p.\n", iface);
    return chain->frame_latency;
}

static HANDLE STDMETHODCALLTYPE dxgi_vk_swap_chain_GetFrameLatencyEvent(IDXGIVkSwapChain2 *iface)
{
    struct dxgi_vk_swap_chain *swapchain = impl_from_IDXGIVkSwapChain(iface);
    HANDLE duplicated_handle;
    VKD3D_UNUSED int fd;

    TRACE("iface %p.\n", iface);

    if (!vkd3d_native_sync_handle_is_valid(swapchain->frame_latency_event))
        return NULL;

#ifdef _WIN32
    /* Based on observation, this handle can be waited on, but ReleaseSemaphore() is not allowed.
     * Verified that NtQueryObject returns 0x100000 access mask (SYNCHRONIZE only). */
    if (!DuplicateHandle(GetCurrentProcess(), swapchain->frame_latency_event.handle,
            GetCurrentProcess(), &duplicated_handle,
            SYNCHRONIZE, FALSE, 0))
    {
        ERR("Failed to duplicate waitable handle.\n");
        return NULL;
    }
#else
    /* Ensure that we don't return fd 0 which would confuse the caller. */
    fd = dup(swapchain->frame_latency_event.fd);
    if (fd == 0)
    {
        fd = dup(fd);
        close(fd);
    }

    duplicated_handle = fd >= 0 ? (HANDLE)(intptr_t)fd : NULL;
#endif

    return duplicated_handle;
}

static HRESULT dxgi_vk_swap_chain_allocate_user_buffer(struct dxgi_vk_swap_chain *chain,
        const DXGI_SWAP_CHAIN_DESC1 *pDesc, struct d3d12_resource **ppResource)
{
    struct d3d12_device *device = chain->queue->device;
    D3D12_RESOURCE_DESC1 resource_desc;
    D3D12_HEAP_PROPERTIES heap_props;

    memset(&resource_desc, 0, sizeof(resource_desc));
    memset(&heap_props, 0, sizeof(heap_props));

    resource_desc.Width = pDesc->Width;
    resource_desc.Height = pDesc->Height;
    resource_desc.Format = pDesc->Format;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.MipLevels = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    return d3d12_resource_create_committed(device, &resource_desc, &heap_props, D3D12_HEAP_FLAG_NONE,
            D3D12_RESOURCE_STATE_PRESENT, NULL, 0, NULL, NULL, ppResource);
}

static HRESULT dxgi_vk_swap_chain_reallocate_user_buffers(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    struct d3d12_resource *old_resources[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    VkImageViewCreateInfo view_info;
    unsigned int i;
    VkResult vr;
    HRESULT hr;

    if (chain->desc.BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS)
        return E_INVALIDARG;

    for (i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; i++)
    {
        old_resources[i] = chain->user.backbuffers[i];
        chain->user.backbuffers[i] = NULL;
        VK_CALL(vkDestroyImageView(chain->queue->device->vk_device, chain->user.vk_image_views[i], NULL));
        chain->user.vk_image_views[i] = VK_NULL_HANDLE;
    }

    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    for (i = 0; i < chain->desc.BufferCount; i++)
    {
        if (FAILED(hr = dxgi_vk_swap_chain_allocate_user_buffer(chain, &chain->desc, &chain->user.backbuffers[i])))
            goto err;

        /* We need to hold a private reference to the resource, not a public one. */
        vkd3d_resource_incref((ID3D12Resource *)&chain->user.backbuffers[i]->ID3D12Resource_iface);
        ID3D12Resource2_Release(&chain->user.backbuffers[i]->ID3D12Resource_iface);
        chain->user.backbuffers[i]->hdr_swapchain_index = i;
        vkd3d_hdr_counted_event("backbuffer_tag index=%u resource=%u format=%u size=%"PRIu64"x%u",
                i, chain->user.backbuffers[i]->res.cookie.index,
                chain->user.backbuffers[i]->desc.Format, chain->user.backbuffers[i]->desc.Width,
                chain->user.backbuffers[i]->desc.Height);

        view_info.format = chain->user.backbuffers[i]->format->vk_format;
        view_info.image = chain->user.backbuffers[i]->res.vk_image;
        vr = VK_CALL(vkCreateImageView(chain->queue->device->vk_device, &view_info, NULL, &chain->user.vk_image_views[i]));
        if (vr < 0)
        {
            ERR("Failed to create image view for user image %u.\n", i);
            hr = E_OUTOFMEMORY;
            goto err;
        }
    }

    for (i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; i++)
        if (old_resources[i])
            vkd3d_resource_decref((ID3D12Resource *)&old_resources[i]->ID3D12Resource_iface);

    return S_OK;

err:
    for (i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; i++)
    {
        if (chain->user.backbuffers[i])
            vkd3d_resource_decref((ID3D12Resource *)&chain->user.backbuffers[i]->ID3D12Resource_iface);
        chain->user.backbuffers[i] = old_resources[i];
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_ChangeProperties(IDXGIVkSwapChain2 *iface, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
        const UINT *pNodeMasks, IUnknown *const *ppPresentQueues)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    DXGI_SWAP_CHAIN_DESC1 old_desc = chain->desc;
    HRESULT hr;
    UINT i;
    const char *method = pNodeMasks || ppPresentQueues
            ? "SwapChain::ResizeBuffers1" : "SwapChain::ResizeBuffers";

    TRACE("iface %p, pDesc %p, pNodeMasks %p, ppPresentQueues %p!\n", iface, pDesc, pNodeMasks, ppPresentQueues);
    vkd3d_ps5_d3d12_event("swapchain op=resize method=%s old=%ux%u/%u/%u "
            "new=%ux%u/%u/%u flags=0x%x",
            method, old_desc.Width, old_desc.Height, old_desc.BufferCount, old_desc.Format,
            pDesc->Width, pDesc->Height, pDesc->BufferCount, pDesc->Format, pDesc->Flags);

    /* TODO: Validate pNodeMasks and ppPresentQueues. */

    /* Public ref-counts must be 0 for this to be allowed. */
    for (i = 0; i < chain->desc.BufferCount; i++)
    {
        if (chain->user.backbuffers[i]->refcount != 0)
        {
            vkd3d_ps5_d3d12_fail(method, DXGI_ERROR_INVALID_CALL,
                    "buffer=%u refcount=%u old_count=%u new_count=%u",
                    i, chain->user.backbuffers[i]->refcount,
                    chain->desc.BufferCount, pDesc->BufferCount);
            return DXGI_ERROR_INVALID_CALL;
        }
    }

    chain->desc = *pDesc;

    /* Don't do anything in this case. */
    if (old_desc.Width == chain->desc.Width &&
            old_desc.Height == chain->desc.Height &&
            old_desc.BufferCount == chain->desc.BufferCount &&
            old_desc.Format == chain->desc.Format &&
            old_desc.Flags == chain->desc.Flags)
    {
        return S_OK;
    }

    /* Waits for any outstanding present event to complete, including the work it takes to blit to screen. */
    dxgi_vk_swap_chain_drain_user_images(chain);

    INFO("Reallocating swapchain (%u x %u), BufferCount = %u.\n",
            chain->desc.Width, chain->desc.Height, chain->desc.BufferCount);

    if (FAILED(hr = dxgi_vk_swap_chain_reallocate_user_buffers(chain)))
    {
        chain->desc = old_desc;
        vkd3d_ps5_d3d12_fail(method, hr,
                "size=%ux%u buffer_count=%u format=%u flags=0x%x",
                pDesc->Width, pDesc->Height, pDesc->BufferCount, pDesc->Format, pDesc->Flags);
        return hr;
    }

    if (chain->user.index >= chain->desc.BufferCount)
    {
        /* Need to reset the user index in case the buffer count is lowered.
         * It is unclear if we're allowed to always reset, but employ principle of least surprise. */
        chain->user.index = 0;
    }

#ifndef _WIN32
    /* Non-Win32 platforms (e.g. Wayland) may not be able to detect resizes on its own.
     * We have drained the present queue at this point, so it's safe to poke this bool.
     * If using native builds, it's up to the application to use ResizeBuffers properly. */
    chain->present.force_swapchain_recreation = true;
#endif

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetPresentRegion(IDXGIVkSwapChain2 *iface, const RECT *pRegion)
{
    FIXME("iface %p, pRegion %p stub!\n", iface, pRegion);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetGammaControl(IDXGIVkSwapChain2 *iface, UINT NumControlPoints, const DXGI_RGB *pControlPoints)
{
    FIXME("iface %p, NumControlPoints %u, pControlPoints %p stub!\n", iface, NumControlPoints, pControlPoints);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetFrameLatency(IDXGIVkSwapChain2 *iface, UINT MaxLatency)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    TRACE("iface %p, MaxLatency %u.\n", iface, MaxLatency);

    if (!MaxLatency || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
    {
        WARN("Invalid maximum frame latency %u.\n", MaxLatency);
        return DXGI_ERROR_INVALID_CALL;
    }

    /* Max frame latency without WAITABLE_OBJECT is always 3,
     * even if set on the device, according to docs. */
    if (!(chain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
    {
        WARN("DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT not set for swap chain %p.\n", iface);
        return DXGI_ERROR_INVALID_CALL;
    }

    /* Only increasing the latency is handled here; apparently it is
     * the application's responsibility to reduce the semaphore value
     * in case the latency gets reduced. */
    if (MaxLatency > chain->frame_latency)
    {
        UINT count = MaxLatency - chain->frame_latency;
        if (vkd3d_native_sync_handle_release(chain->frame_latency_event, count) >= 0)
            vkd3d_atomic_uint64_increment(
                    &chain->stall.release_semaphore_count,
                    vkd3d_memory_order_relaxed);
        else
            vkd3d_atomic_uint64_increment(
                    &chain->stall.release_semaphore_fail_count,
                    vkd3d_memory_order_relaxed);
    }

    chain->frame_latency = MaxLatency;
    return S_OK;
}

static VkXYColorEXT convert_xy_color(UINT16 *dxgi_color)
{
    return (VkXYColorEXT) { dxgi_color[0] / 50000.0f, dxgi_color[1] / 50000.0f };
}

static float convert_max_luminance(UINT dxgi_luminance)
{
    /* The documentation says this is in *whole* nits, but this
     * contradicts the HEVC standard it claims to mirror,
     * and the sample's behaviour.
     * We should come back and validate this once
     * https://github.com/microsoft/DirectX-Graphics-Samples/issues/796
     * has an answer. */
    return (float)dxgi_luminance;
}

static float convert_min_luminance(UINT dxgi_luminance)
{
    return dxgi_luminance * 0.0001f;
}

static float convert_level(UINT16 dxgi_level)
{
    return (float)dxgi_level;
}

static VkHdrMetadataEXT convert_hdr_metadata_hdr10(DXGI_HDR_METADATA_HDR10 *dxgi_metadata)
{
    VkHdrMetadataEXT vulkan_metadata = { VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
    vulkan_metadata.displayPrimaryRed = convert_xy_color(dxgi_metadata->RedPrimary);
    vulkan_metadata.displayPrimaryGreen = convert_xy_color(dxgi_metadata->GreenPrimary);
    vulkan_metadata.displayPrimaryBlue = convert_xy_color(dxgi_metadata->BluePrimary);
    vulkan_metadata.whitePoint = convert_xy_color(dxgi_metadata->WhitePoint);
    vulkan_metadata.maxLuminance = convert_max_luminance(dxgi_metadata->MaxMasteringLuminance);
    vulkan_metadata.minLuminance = convert_min_luminance(dxgi_metadata->MinMasteringLuminance);
    vulkan_metadata.maxContentLightLevel = convert_level(dxgi_metadata->MaxContentLightLevel);
    vulkan_metadata.maxFrameAverageLightLevel = convert_level(dxgi_metadata->MaxFrameAverageLightLevel);
    return vulkan_metadata;
}

static void dxgi_vk_swap_chain_set_hdr_metadata(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkHdrMetadataEXT hdr_metadata;

    if (!chain->queue->device->vk_info.EXT_hdr_metadata ||
            !chain->present.vk_swapchain ||
            chain->request.dxgi_hdr_metadata.Type != DXGI_HDR_METADATA_TYPE_HDR10)
    {
        return;
    }

    hdr_metadata = convert_hdr_metadata_hdr10(&chain->request.dxgi_hdr_metadata.HDR10);
    VK_CALL(vkSetHdrMetadataEXT(chain->queue->device->vk_device, 1, &chain->present.vk_swapchain, &hdr_metadata));
}

#ifdef _WIN32
static bool dxgi_vk_swap_chain_present_task_is_idle(struct dxgi_vk_swap_chain *chain)
{
    uint64_t presented_count = vkd3d_atomic_uint64_load_explicit(&chain->present.present_count, vkd3d_memory_order_acquire);
    return presented_count == chain->user.present_count;
}

static bool dxgi_vk_swap_chain_is_occluded(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDevice vk_physical_device = chain->queue->device->vk_physical_device;
    VkSurfaceCapabilitiesKHR surface_caps;

    VK_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_physical_device, chain->vk_surface, &surface_caps));
    /* Win32 jank, when these are 0 we cannot create a swapchain. */
    return surface_caps.maxImageExtent.width == 0 || surface_caps.maxImageExtent.height == 0;
}
#endif

static bool dxgi_vk_swap_chain_present_is_occluded(struct dxgi_vk_swap_chain *chain)
{
#ifdef _WIN32
    if (dxgi_vk_swap_chain_present_task_is_idle(chain))
    {
        /* Query the surface directly. */
        chain->present.is_occlusion_state = dxgi_vk_swap_chain_is_occluded(chain);
        return chain->present.is_occlusion_state != 0;
    }
    else
    {
        /* If presentation requests are pending it is not safe to access the surface directly
         * without adding tons of locks everywhere,
         * so rely on observed behavior from presentation thread. */
        return vkd3d_atomic_uint32_load_explicit(&chain->present.is_occlusion_state, vkd3d_memory_order_relaxed) != 0;
    }
#else
	/* Irrelevant on native build. */
	(void)chain;
	return false;
#endif
}

static void dxgi_vk_swap_chain_present_callback(void *chain);

static void dxgi_vk_swap_chain_wait_internal_handle(
        struct dxgi_vk_swap_chain *chain, bool low_latency_enable)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    bool non_blocking_internal_handle_wait = low_latency_enable;
    uint64_t completed_submissions = 0;
    uint64_t user_submissions = 0;

    vkd3d_atomic_uint32_increment(
            &chain->frame_latency_event_internal_wait_counts,
            vkd3d_memory_order_relaxed);

    if (non_blocking_internal_handle_wait)
    {
        /* If we're using low latency mode, we expect that applications sleep on their own in LatencySleep.
         * If we start sleeping ourselves here, we sometimes end up fighting with NV's LL2 implementation over
         * which sleep cycle gets to dominate. This can manifest as a random pumping pattern.
         *
         * If our sleep dominates, we end up in an unstable situation where LL2 may think we're
         * more CPU bound than we actually are.
         *
         * In a FIFO bound scenario however where GPU completes long before vblank hits,
         * we should rely on frame latency sleeps.
         *
         * Use a very simple heuristic. If the blit timeline semaphore lags behind by 2+ frames, assume we're
         * fully GPU bound and we should back off and let low latency deal with it more gracefully. */
        user_submissions = chain->user.blit_count;

        if (VK_CALL(vkGetSemaphoreCounterValue(chain->queue->device->vk_device,
                chain->present.vk_complete_semaphore,
                &completed_submissions)) == VK_SUCCESS)
        {
            /* We just submitted frame N. If N - 2 is already complete, it means there is <= 2 frames worth of GPU work
             * queued up. For a FIFO bound or CPU bound game, this is the case we expect, so we should use latency fences here.
             * If we're GPU bound with <= 2 frames queued up, we'll likely not block in our own latency handles anyway. */
            if (completed_submissions + 2 >= user_submissions)
            {
                non_blocking_internal_handle_wait = false;
            }
            else if (chain->debug_latency)
            {
                INFO("Completed count: %"PRIu64", submitted count: %"PRIu64". GPU queue is too deep, deferring to low latency sleep.\n",
                        completed_submissions, user_submissions);
            }
        }
        else
        {
            ERR("Failed to query semaphore complete value.\n");
            non_blocking_internal_handle_wait = false;
        }
    }

    if (non_blocking_internal_handle_wait)
    {
        /* Just make sure the counter doesn't get unbounded. */
        while (vkd3d_atomic_uint32_load_explicit(
                &chain->frame_latency_event_internal_wait_counts,
                vkd3d_memory_order_relaxed) &&
                vkd3d_native_sync_handle_acquire_timeout(
                chain->frame_latency_event_internal, 0))
        {
            vkd3d_atomic_uint32_decrement(
                    &chain->frame_latency_event_internal_wait_counts,
                    vkd3d_memory_order_relaxed);
        }
    }
    else
    {
        /* PS5: A bounded wait preserves normal back-pressure while ensuring
         * a lost internal semaphore count leaves an actionable stall record. */
        while (vkd3d_atomic_uint32_load_explicit(
                &chain->frame_latency_event_internal_wait_counts,
                vkd3d_memory_order_relaxed))
        {
            if (vkd3d_native_sync_handle_acquire_timeout(
                    chain->frame_latency_event_internal, 1000))
            {
                vkd3d_atomic_uint32_decrement(
                        &chain->frame_latency_event_internal_wait_counts,
                        vkd3d_memory_order_relaxed);
            }
            else
            {
                dxgi_vk_swap_chain_check_latency_stall(chain);
            }
        }
    }
}

static const char *vkd3d_hdr_writer_name(uint32_t writer)
{
    switch (writer)
    {
        case VKD3D_HDR_WRITER_DRAW: return "draw";
        case VKD3D_HDR_WRITER_DISPATCH: return "dispatch";
        case VKD3D_HDR_WRITER_COPY_RESOURCE: return "copy_resource";
        case VKD3D_HDR_WRITER_COPY_TEXTURE: return "copy_texture";
        case VKD3D_HDR_WRITER_RESOLVE: return "resolve";
        default: return "none";
    }
}

#define VKD3D_HDR_WRITER_CENSUS_WINDOW_NS (UINT64_C(10) * 1000 * 1000 * 1000)
#define VKD3D_HDR_WRITER_CENSUS_ENTRY_COUNT 16

struct vkd3d_hdr_writer_census_entry
{
    uint32_t writer_type;
    uint64_t shader_hash;
    uint64_t count;
    uint32_t view_format;
    uint32_t write_mask;
    uint32_t blend_enable;
    uint32_t blend_src;
    uint32_t blend_dst;
    uint32_t blend_op;
    uint32_t srv_format_count;
    uint32_t srv_formats[VKD3D_HDR_WRITER_SRV_FORMAT_COUNT];
};

static pthread_mutex_t vkd3d_hdr_writer_census_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vkd3d_hdr_writer_census_entry
        vkd3d_hdr_writer_census[VKD3D_HDR_WRITER_CENSUS_ENTRY_COUNT];
static uint64_t vkd3d_hdr_writer_census_start_ns;
static uint64_t vkd3d_hdr_writer_census_present_count;
static uint64_t vkd3d_hdr_writer_census_overflow;
static uint32_t vkd3d_hdr_writer_census_count;

static void vkd3d_hdr_writer_census_record(struct d3d12_resource *resource,
        uint32_t writer_type, uint64_t shader_hash)
{
    char counts[1024], srv_formats[128];
    struct vkd3d_hdr_writer_census_entry *entry;
    uint64_t now, duration_ms;
    size_t counts_offset, srv_offset;
    unsigned int i, top;

    if (!vkd3d_hdr_trace_enabled())
        return;

    now = vkd3d_get_current_time_ns();
    pthread_mutex_lock(&vkd3d_hdr_writer_census_lock);
    if (!vkd3d_hdr_writer_census_start_ns)
        vkd3d_hdr_writer_census_start_ns = now;

    for (i = 0; i < vkd3d_hdr_writer_census_count; i++)
    {
        if (vkd3d_hdr_writer_census[i].writer_type == writer_type &&
                vkd3d_hdr_writer_census[i].shader_hash == shader_hash)
            break;
    }
    if (i == vkd3d_hdr_writer_census_count)
    {
        if (i < ARRAY_SIZE(vkd3d_hdr_writer_census))
        {
            memset(&vkd3d_hdr_writer_census[i], 0,
                    sizeof(vkd3d_hdr_writer_census[i]));
            vkd3d_hdr_writer_census[i].writer_type = writer_type;
            vkd3d_hdr_writer_census[i].shader_hash = shader_hash;
            vkd3d_hdr_writer_census_count++;
        }
        else
        {
            vkd3d_hdr_writer_census_overflow++;
        }
    }
    if (i < ARRAY_SIZE(vkd3d_hdr_writer_census))
    {
        entry = &vkd3d_hdr_writer_census[i];
        entry->count++;
        if (writer_type != VKD3D_HDR_WRITER_NONE)
        {
            entry->view_format = vkd3d_atomic_uint32_load_explicit(
                    &resource->hdr_last_writer_view_format, vkd3d_memory_order_relaxed);
            entry->write_mask = vkd3d_atomic_uint32_load_explicit(
                    &resource->hdr_last_writer_write_mask, vkd3d_memory_order_relaxed);
            entry->blend_enable = vkd3d_atomic_uint32_load_explicit(
                    &resource->hdr_last_writer_blend_enable, vkd3d_memory_order_relaxed);
            entry->blend_src = vkd3d_atomic_uint32_load_explicit(
                    &resource->hdr_last_writer_blend_src, vkd3d_memory_order_relaxed);
            entry->blend_dst = vkd3d_atomic_uint32_load_explicit(
                    &resource->hdr_last_writer_blend_dst, vkd3d_memory_order_relaxed);
            entry->blend_op = vkd3d_atomic_uint32_load_explicit(
                    &resource->hdr_last_writer_blend_op, vkd3d_memory_order_relaxed);
            entry->srv_format_count = min(vkd3d_atomic_uint32_load_explicit(
                    &resource->hdr_last_writer_srv_format_count,
                    vkd3d_memory_order_relaxed),
                    (uint32_t)VKD3D_HDR_WRITER_SRV_FORMAT_COUNT);
            for (i = 0; i < entry->srv_format_count; i++)
            {
                entry->srv_formats[i] = vkd3d_atomic_uint32_load_explicit(
                        &resource->hdr_last_writer_srv_formats[i],
                        vkd3d_memory_order_relaxed);
            }
        }
    }
    vkd3d_hdr_writer_census_present_count++;

    if (now - vkd3d_hdr_writer_census_start_ns <
            VKD3D_HDR_WRITER_CENSUS_WINDOW_NS)
    {
        pthread_mutex_unlock(&vkd3d_hdr_writer_census_lock);
        return;
    }

    counts[0] = '\0';
    counts_offset = 0;
    top = 0;
    for (i = 0; i < vkd3d_hdr_writer_census_count; i++)
    {
        int length = snprintf(counts + counts_offset,
                sizeof(counts) - counts_offset, "%s%s:%016"PRIx64"=%"PRIu64,
                i ? "," : "", vkd3d_hdr_writer_name(
                        vkd3d_hdr_writer_census[i].writer_type),
                vkd3d_hdr_writer_census[i].shader_hash,
                vkd3d_hdr_writer_census[i].count);
        if (length > 0 && (size_t)length < sizeof(counts) - counts_offset)
            counts_offset += length;
        if (vkd3d_hdr_writer_census[i].count >
                vkd3d_hdr_writer_census[top].count)
            top = i;
    }

    entry = vkd3d_hdr_writer_census_count
            ? &vkd3d_hdr_writer_census[top] : NULL;
    strcpy(srv_formats, "-");
    srv_offset = 0;
    if (entry && entry->srv_format_count)
    {
        srv_formats[0] = '\0';
        for (i = 0; i < entry->srv_format_count; i++)
        {
            int length = snprintf(srv_formats + srv_offset,
                    sizeof(srv_formats) - srv_offset, "%s%u",
                    i ? "," : "", entry->srv_formats[i]);
            if (length > 0 && (size_t)length < sizeof(srv_formats) - srv_offset)
                srv_offset += length;
        }
    }

    duration_ms = (now - vkd3d_hdr_writer_census_start_ns) / 1000000;
    ERR("WINEPS5_HDR_TRACE VKD3D writer10s duration_ms=%"PRIu64
            " presents=%"PRIu64" keys=%u overflow=%"PRIu64" counts=%s"
            " top_type=%s top_shader=%016"PRIx64" top_count=%"PRIu64
            " target_format=%u write_mask=%#x blend=%u src=%u dst=%u op=%u"
            " srv_formats=%s\n",
            duration_ms, vkd3d_hdr_writer_census_present_count,
            vkd3d_hdr_writer_census_count, vkd3d_hdr_writer_census_overflow,
            counts_offset ? counts : "-",
            entry ? vkd3d_hdr_writer_name(entry->writer_type) : "none",
            entry ? entry->shader_hash : 0,
            entry ? entry->count : 0,
            entry ? entry->view_format : DXGI_FORMAT_UNKNOWN,
            entry ? entry->write_mask : 0,
            entry ? entry->blend_enable : 0,
            entry ? entry->blend_src : 0,
            entry ? entry->blend_dst : 0,
            entry ? entry->blend_op : 0, srv_formats);

    memset(vkd3d_hdr_writer_census, 0, sizeof(vkd3d_hdr_writer_census));
    vkd3d_hdr_writer_census_count = 0;
    vkd3d_hdr_writer_census_present_count = 0;
    vkd3d_hdr_writer_census_overflow = 0;
    vkd3d_hdr_writer_census_start_ns = now;
    pthread_mutex_unlock(&vkd3d_hdr_writer_census_lock);
}

static void dxgi_vk_swap_chain_hdr_trace_present(struct d3d12_resource *resource, unsigned int index)
{
    uint32_t srv_valid = vkd3d_atomic_uint32_load_explicit(
            &resource->hdr_present_srv_valid, vkd3d_memory_order_acquire);
    uint32_t writer = vkd3d_atomic_uint32_load_explicit(
            &resource->hdr_last_writer_type, vkd3d_memory_order_acquire);
    uint64_t shader_hash = vkd3d_atomic_uint64_load_explicit(
            &resource->hdr_last_writer_hash, vkd3d_memory_order_relaxed);
    uint32_t srv_descriptor = 0, srv_id = 0, srv_view_format = 0;
    uint32_t srv_resource_format = 0, srv_dimension = 0, srv_view_type = 0;
    uint32_t srv_height = 0, srv_depth = 0;
    uint64_t srv_width = 0;
    vkd3d_hdr_writer_census_record(resource, writer, shader_hash);

    if (srv_valid)
    {
        srv_descriptor = vkd3d_atomic_uint32_load_explicit(
                &resource->hdr_present_srv_descriptor, vkd3d_memory_order_relaxed);
        srv_id = vkd3d_atomic_uint32_load_explicit(
                &resource->hdr_present_srv_id, vkd3d_memory_order_relaxed);
        srv_view_format = vkd3d_atomic_uint32_load_explicit(
                &resource->hdr_present_srv_view_format, vkd3d_memory_order_relaxed);
        srv_resource_format = vkd3d_atomic_uint32_load_explicit(
                &resource->hdr_present_srv_resource_format, vkd3d_memory_order_relaxed);
        srv_dimension = vkd3d_atomic_uint32_load_explicit(
                &resource->hdr_present_srv_dimension, vkd3d_memory_order_relaxed);
        srv_view_type = vkd3d_atomic_uint32_load_explicit(
                &resource->hdr_present_srv_view_type, vkd3d_memory_order_relaxed);
        srv_width = vkd3d_atomic_uint64_load_explicit(
                &resource->hdr_present_srv_width, vkd3d_memory_order_relaxed);
        srv_height = vkd3d_atomic_uint32_load_explicit(
                &resource->hdr_present_srv_height, vkd3d_memory_order_relaxed);
        srv_depth = vkd3d_atomic_uint32_load_explicit(
                &resource->hdr_present_srv_depth, vkd3d_memory_order_relaxed);
    }

    vkd3d_hdr_counted_event("present index=%u resource=%u format=%u size=%"PRIu64"x%u"
            " last_writer=%s shader=%016"PRIx64
            " present_srv_valid=%u descriptor=%u srv_id=%u view_format=%u"
            " resource_format=%u dimension=%u view_type=%u srv_size=%"PRIu64"x%u depth=%u",
            index, resource->res.cookie.index, resource->desc.Format,
            resource->desc.Width, resource->desc.Height,
            vkd3d_hdr_writer_name(writer), shader_hash,
            srv_valid, srv_descriptor, srv_id, srv_view_format,
            srv_resource_format, srv_dimension, srv_view_type,
            srv_width, srv_height, srv_depth);

    vkd3d_atomic_uint32_store_explicit(
            &resource->hdr_present_srv_valid, 0, vkd3d_memory_order_release);
    vkd3d_atomic_uint64_store_explicit(
            &resource->hdr_last_writer_hash, 0, vkd3d_memory_order_relaxed);
    vkd3d_atomic_uint32_store_explicit(
            &resource->hdr_last_writer_type, VKD3D_HDR_WRITER_NONE, vkd3d_memory_order_release);
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_Present(IDXGIVkSwapChain2 *iface, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    struct dxgi_vk_swap_chain_present_request *request;
    struct d3d12_resource *backbuffer;
    struct vkd3d_queue_timeline_trace_cookie cookie;
    uint64_t auto_sdr_hash, auto_hdr_hash, auto_last_hash;
    uint64_t latency_wait_begin_ns, latency_wait_ns = 0;
    uint64_t fg_game_block_ns = 0, fg_wait_begin_ns;
    uint64_t present_time_ns, present_end_ns;
    struct vkd3d_ps5_hitch_scope ps5_hitch_scope;
    bool fg_metrics;
    uint32_t auto_last_writer_type;
    bool low_latency_enable;

    TRACE("iface %p, SyncInterval %u, PresentFlags #%x, pPresentParameters %p.\n",
            iface, SyncInterval, PresentFlags, pPresentParameters);
    (void)pPresentParameters;

    if (dxgi_vk_swap_chain_present_is_occluded(chain))
        return DXGI_STATUS_OCCLUDED;
    if (PresentFlags & DXGI_PRESENT_TEST)
        return S_OK;

    ps5_hitch_scope = vkd3d_ps5_hitch_begin(WINEPS5_HITCH_PRESENT);
    present_time_ns = vkd3d_get_current_time_ns();
    fg_metrics = vkd3d_atomic_uint32_load_explicit(&chain->present.fg.metrics_active, vkd3d_memory_order_acquire) != 0;
    dxgi_vk_swap_chain_note_present(chain);

    /* If we missed the event signal last frame, we have to wait for it now.
     * Otherwise, we end up in a floating state where our waits and thread signals might not stay in sync anymore. */
    if (chain->outstanding_present_request)
    {
        fg_wait_begin_ns = fg_metrics ? vkd3d_get_current_time_ns() : 0;
        while (!vkd3d_native_sync_handle_acquire_timeout(
                chain->present_request_done_event, 1000))
            dxgi_vk_swap_chain_check_latency_stall(chain);
        vkd3d_atomic_uint32_store_explicit(&chain->stall.present_done_pending,
                0, vkd3d_memory_order_relaxed);
        chain->outstanding_present_request = false;
        if (fg_metrics) fg_game_block_ns += vkd3d_get_current_time_ns() - fg_wait_begin_ns;
    }

    assert(chain->user.index < chain->desc.BufferCount);
    backbuffer = chain->user.backbuffers[chain->user.index];
    auto_sdr_hash = vkd3d_atomic_uint64_exchange_explicit(
            &backbuffer->hdr_auto_sdr_writer_hash, 0, vkd3d_memory_order_acq_rel);
    auto_hdr_hash = vkd3d_atomic_uint64_exchange_explicit(
            &backbuffer->hdr_auto_hdr_writer_hash, 0, vkd3d_memory_order_acq_rel);
    auto_last_writer_type = vkd3d_atomic_uint32_load_explicit(
            &backbuffer->hdr_last_writer_type, vkd3d_memory_order_acquire);
    auto_last_hash = auto_last_writer_type != VKD3D_HDR_WRITER_NONE
            ? vkd3d_atomic_uint64_load_explicit(
                    &backbuffer->hdr_last_writer_hash, vkd3d_memory_order_relaxed) : 0;
    dxgi_vk_swap_chain_hdr_trace_present(backbuffer, chain->user.index);

    /* The present iteration on present thread has a similar counter and it will pick up the request from the ring. */
    chain->user.present_count += 1;
    request = &chain->request_ring[chain->user.present_count % ARRAY_SIZE(chain->request_ring)];

    request->hdr_auto_sdr_writer_hash = auto_sdr_hash;
    request->hdr_auto_hdr_writer_hash = auto_hdr_hash;
    request->hdr_auto_last_writer_hash = auto_last_hash;

    request->swap_interval = SyncInterval;
    request->dxgi_format = chain->user.backbuffers[chain->user.index]->desc.Format;
    request->user_index = chain->user.index;
    /* If we don't have wait thread, BufferCount needs to be tweaked to control latency.
     * Some games that create BufferCount = 3 swapchains expect us to absorb a lot of latency or we start
     * starving. If we have waiter thread, we'll block elsewhere. */
    request->target_min_image_count = chain->wait_thread.supports_present_wait ? 0 : chain->desc.BufferCount + 1;
    request->dxgi_color_space_type = chain->user.dxgi_color_space_type;
    request->dxgi_hdr_metadata = chain->user.dxgi_hdr_metadata;
    request->modifies_hdr_metadata = chain->user.modifies_hdr_metadata;
    request->begin_frame_time_ns = chain->user.begin_frame_time_ns;
    request->present_time_ns = present_time_ns;
    request->low_latency_frame_id = vkd3d_atomic_uint64_load_explicit(
            &chain->queue->device->frame_markers.present, vkd3d_memory_order_acquire);

    if (chain->debug_latency && request->low_latency_frame_id)
        INFO("Presenting with low latency frame ID: %"PRIu64".\n", request->low_latency_frame_id);

    chain->user.modifies_hdr_metadata = false;

    if (chain->queue->device->vk_info.NV_low_latency2)
    {
        pthread_mutex_lock(&chain->present.low_latency_state_update_lock);
        request->requested_low_latency_state = chain->requested_low_latency_state;
        request->low_latency_update_requested = chain->low_latency_update_requested;
        chain->low_latency_update_requested = false;
        low_latency_enable = chain->requested_low_latency_state.mode;
        pthread_mutex_unlock(&chain->present.low_latency_state_update_lock);
    }
    else if (chain->queue->device->device_info.anti_lag_amd.antiLag)
    {
        spinlock_acquire(&chain->queue->device->low_latency_swapchain_spinlock);
        request->requested_anti_lag_state.mode = chain->queue->device->swapchain_info.mode;
        request->requested_anti_lag_state.max_fps = chain->queue->device->swapchain_info.max_fps;
        low_latency_enable = request->requested_anti_lag_state.mode;
        spinlock_release(&chain->queue->device->low_latency_swapchain_spinlock);
    }
    else
    {
        memset(&request->requested_low_latency_state, 0, sizeof(request->requested_low_latency_state));
        request->low_latency_update_requested = false;
        low_latency_enable = false;
    }

    /* Need to process this task in queue thread to deal with wait-before-signal.
     * All interesting works happens in the callback. */
    chain->user.blit_count += 1;
    if (vkd3d_native_sync_handle_is_valid(chain->present_request_done_event))
        vkd3d_atomic_uint32_store_explicit(&chain->stall.present_done_pending,
                1, vkd3d_memory_order_relaxed);
    d3d12_command_queue_enqueue_callback(chain->queue, dxgi_vk_swap_chain_present_callback, chain);

    chain->user.index = (chain->user.index + 1) % chain->desc.BufferCount;

    cookie = vkd3d_queue_timeline_trace_register_present_block(
            &chain->queue->device->queue_timeline_trace,
            chain->queue->device->frame_markers.present ?
                    chain->queue->device->frame_markers.present :
                    chain->user.blit_count);

    fg_wait_begin_ns = fg_metrics ? vkd3d_get_current_time_ns() : 0;
    /* Relevant if application does not use latency fence, or we force a lower latency through VKD3D_SWAPCHAIN_FRAME_LATENCY overrides. */
    if (vkd3d_native_sync_handle_is_valid(chain->frame_latency_event_internal))
    {
        latency_wait_begin_ns = vkd3d_get_current_time_ns();
        dxgi_vk_swap_chain_wait_internal_handle(chain, low_latency_enable);
        latency_wait_ns = vkd3d_get_current_time_ns() - latency_wait_begin_ns;
    }

    if (vkd3d_native_sync_handle_is_valid(chain->present_request_done_event))
    {
        /* For non-present wait path where we are trying to simulate KHR_present_wait in a poor man's way.
         * Block here until we have processed the present and acquired the next image. This is equivalent
         * to a frame latency of swapchain.imageCount - 1. (Usually 2).
         * To combat deadlocks, we can add a small timeout.
         * Failing the timeout is not severe. It will manifest as stutter, but it is better than spurious deadlock.
         * When KHR_present_wait is supported, this path is never taken.
         * If we're heavily GPU bound, we will generally end up blocking on GPU-completion fences in game code instead.
         * When we are present bound, we will generally always render at > 15 Hz. */
        if (!vkd3d_native_sync_handle_acquire_timeout(
                chain->present_request_done_event, 80))
        {
            WARN("Detected excessively slow Present() processing. Potential causes: resize, wait-before-signal.\n");
            /* Remember to wait for this next present. */
            chain->outstanding_present_request = true;
        }
        else
        {
            vkd3d_atomic_uint32_store_explicit(
                    &chain->stall.present_done_pending, 0,
                    vkd3d_memory_order_relaxed);
        }
    }
    if (fg_metrics)
    {
        fg_game_block_ns += vkd3d_get_current_time_ns() - fg_wait_begin_ns;
        dxgi_vk_swap_chain_fg_note_game_block(chain, fg_game_block_ns);
    }

    /* For latency debug purposes. Consider a frame to begin when we return from Present() with the next user index set.
     * This isn't necessarily correct if the application does WaitSingleObject() on the latency right after this call.
     * That call can take up a frame, so the real latency will be lower than the one reported.
     * Otherwise, the estimate should match up with the internal latency fence. */
    if (chain->debug_latency)
        chain->user.begin_frame_time_ns = vkd3d_get_current_time_ns();

    vkd3d_queue_timeline_trace_complete_present_block(
            &chain->queue->device->queue_timeline_trace, cookie);
    vkd3d_ps5_hitch_end(&ps5_hitch_scope);
    present_end_ns = vkd3d_get_current_time_ns();
    vkd3d_ps5_gpu_census_present(latency_wait_ns, present_end_ns);
    vkd3d_ps5_hitch_present(present_end_ns);

    return S_OK;
}

static VkColorSpaceKHR convert_color_space(DXGI_COLOR_SPACE_TYPE dxgi_color_space);
static bool dxgi_vk_swap_chain_update_formats_locked(struct dxgi_vk_swap_chain *chain, bool force_requery);

static bool dxgi_vk_swap_chain_supports_color_space(struct dxgi_vk_swap_chain *chain, DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    VkColorSpaceKHR vk_color_space;
    bool ret = false;
    uint32_t i;

    vk_color_space = convert_color_space(ColorSpace);

    pthread_mutex_lock(&chain->properties.lock);
    dxgi_vk_swap_chain_update_formats_locked(chain, false);
    for (i = 0; i < chain->properties.format_count && !ret; i++)
        if (chain->properties.formats[i].colorSpace == vk_color_space)
            ret = true;
    pthread_mutex_unlock(&chain->properties.lock);
    return ret;
}

static UINT STDMETHODCALLTYPE dxgi_vk_swap_chain_CheckColorSpaceSupport(IDXGIVkSwapChain2 *iface, DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    UINT support_flags = 0;
    if (dxgi_vk_swap_chain_supports_color_space(chain, ColorSpace))
        support_flags |= DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;
    vkd3d_ps5_d3d12_event("swapchain op=colorspace_check colorspace=%u support=0x%x",
            ColorSpace, support_flags);
    TRACE("iface %p, ColorSpace %u, SupportFlags #%x.\n", iface, ColorSpace, support_flags);
    return support_flags;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetColorSpace(IDXGIVkSwapChain2 *iface, DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    bool supported;

    TRACE("iface %p, ColorSpace %u.\n", iface, ColorSpace);
    supported = dxgi_vk_swap_chain_supports_color_space(chain, ColorSpace);
    vkd3d_ps5_d3d12_event("swapchain op=colorspace old=%u new=%u supported=%u",
            chain->user.dxgi_color_space_type, ColorSpace, supported);
    if (!supported)
    {
        vkd3d_ps5_d3d12_fail("SwapChain::SetColorSpace1", E_INVALIDARG,
                "old=%u new=%u", chain->user.dxgi_color_space_type, ColorSpace);
        return E_INVALIDARG;
    }

    if (chain->present.fg.running)
        dxgi_vk_swap_chain_drain_queue(chain);
    chain->user.dxgi_color_space_type = ColorSpace;
    chain->user.modifies_hdr_metadata = true;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_SetHDRMetaData(IDXGIVkSwapChain2 *iface, const DXGI_VK_HDR_METADATA *pMetaData)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);
    TRACE("iface %p, pMetadata %p.\n", iface, pMetaData);
    vkd3d_ps5_d3d12_event("swapchain op=hdrmeta type=%u max_mastering=%u min_mastering=%u "
            "max_cll=%u max_fall=%u",
            pMetaData->Type, pMetaData->Type == DXGI_HDR_METADATA_TYPE_HDR10
                    ? pMetaData->HDR10.MaxMasteringLuminance : 0,
            pMetaData->Type == DXGI_HDR_METADATA_TYPE_HDR10
                    ? pMetaData->HDR10.MinMasteringLuminance : 0,
            pMetaData->Type == DXGI_HDR_METADATA_TYPE_HDR10
                    ? pMetaData->HDR10.MaxContentLightLevel : 0,
            pMetaData->Type == DXGI_HDR_METADATA_TYPE_HDR10
                    ? pMetaData->HDR10.MaxFrameAverageLightLevel : 0);
    chain->user.dxgi_hdr_metadata = *pMetaData;
    chain->user.modifies_hdr_metadata = true;
    return S_OK;
}

static void STDMETHODCALLTYPE dxgi_vk_swap_chain_GetLastPresentCount(IDXGIVkSwapChain2 *iface, UINT64 *present_count)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);

    TRACE("iface %p, present_count %p.\n", iface, present_count);

    /* Represents the number of times Present has been called successfully */
    *present_count = chain->user.present_count;
}

static void STDMETHODCALLTYPE dxgi_vk_swap_chain_GetFrameStatistics(IDXGIVkSwapChain2 *iface,
        DXGI_VK_FRAME_STATISTICS *frame_statistics)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);

    TRACE("iface %p, frame_statistics %p!\n", iface, frame_statistics);

    /* Returns the total number of frames presented, and the
     * timestamp when the last present has completed. */
    spinlock_acquire(&chain->frame_statistics.lock);
    frame_statistics->PresentCount = chain->frame_statistics.count;
    frame_statistics->PresentQPCTime = chain->frame_statistics.time;
    spinlock_release(&chain->frame_statistics.lock);
}

static void STDMETHODCALLTYPE dxgi_vk_swap_chain_SetTargetFrameRate(IDXGIVkSwapChain2 *iface, double frame_rate)
{
    struct dxgi_vk_swap_chain *chain = impl_from_IDXGIVkSwapChain(iface);

    TRACE("iface %p, frame_rate %lf.\n", iface, frame_rate);

    /* Env var takes priority over the display mode and config option */
    if (chain->frame_rate_limit.has_user_override)
        return;

    pthread_mutex_lock(&chain->frame_rate_limit.lock);

    if (frame_rate != 0.0)
    {
        /* Target frame rate may be negative, in which case we should only engage the limiter
         * if the measured frame rate is higher than expected, e.g. due to the actual display
         * refresh rate not matching the display mode used for the swap chain. */
        INFO("Set target frame rate to %.1lf FPS.\n", fabs(frame_rate));

        chain->frame_rate_limit.enable = frame_rate > 0.0;
        chain->frame_rate_limit.target_interval_ns = (uint64_t)(1.0e9 / fabs(frame_rate));
        chain->frame_rate_limit.next_deadline_ns = 0;

        chain->frame_rate_limit.heuristic_frame_time_ns = 0;
        chain->frame_rate_limit.heuristic_frame_count = 0;
    }
    else
    {
        if (chain->frame_rate_limit.enable)
            INFO("Disabling frame rate limit.\n");

        chain->frame_rate_limit.enable = false;
        chain->frame_rate_limit.target_interval_ns = 0;
    }

    pthread_mutex_unlock(&chain->frame_rate_limit.lock);
}

static CONST_VTBL struct IDXGIVkSwapChain2Vtbl dxgi_vk_swap_chain_vtbl =
{
    /* IUnknown methods */
    dxgi_vk_swap_chain_QueryInterface,
    dxgi_vk_swap_chain_AddRef,
    dxgi_vk_swap_chain_Release,

    /* IDXGIVkSwapChain methods */
    dxgi_vk_swap_chain_GetDesc,
    dxgi_vk_swap_chain_GetAdapter,
    dxgi_vk_swap_chain_GetDevice,
    dxgi_vk_swap_chain_GetImage,
    dxgi_vk_swap_chain_GetImageIndex,
    dxgi_vk_swap_chain_GetFrameLatency,
    dxgi_vk_swap_chain_GetFrameLatencyEvent,
    dxgi_vk_swap_chain_ChangeProperties,
    dxgi_vk_swap_chain_SetPresentRegion,
    dxgi_vk_swap_chain_SetGammaControl,
    dxgi_vk_swap_chain_SetFrameLatency,
    dxgi_vk_swap_chain_Present,
    dxgi_vk_swap_chain_CheckColorSpaceSupport,
    dxgi_vk_swap_chain_SetColorSpace,
    dxgi_vk_swap_chain_SetHDRMetaData,
    /* IDXGIVkSwapChain1 methods */
    dxgi_vk_swap_chain_GetLastPresentCount,
    dxgi_vk_swap_chain_GetFrameStatistics,
    /* IDXGIVkSwapChain2 methods */
    dxgi_vk_swap_chain_SetTargetFrameRate,
};

static bool dxgi_vk_swap_chain_update_formats_locked(struct dxgi_vk_swap_chain *chain, bool force_requery)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDevice vk_physical_device = chain->queue->device->vk_physical_device;
    VkResult vr;

    if (chain->properties.format_count != 0 && !force_requery)
        return true;

    if ((vr = VK_CALL(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, chain->vk_surface,
            &chain->properties.format_count, NULL))) < 0)
    {
        ERR("Failed to query surface formats.\n");
        chain->properties.format_count = 0;
        return false;
    }

    if (!vkd3d_array_reserve((void**)&chain->properties.formats, &chain->properties.formats_size,
            chain->properties.format_count, sizeof(*chain->properties.formats)))
    {
        ERR("Failed to allocate memory.\n");
        chain->properties.format_count = 0;
        return false;
    }

    if ((vr = VK_CALL(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, chain->vk_surface,
            &chain->properties.format_count, chain->properties.formats))) < 0)
    {
        ERR("Failed to query surface formats.\n");
        chain->properties.format_count = 0;
        return false;
    }

    return true;
}

static HRESULT dxgi_vk_swap_chain_create_surface(struct dxgi_vk_swap_chain *chain, IDXGIVkSurfaceFactory *pFactory)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDevice vk_physical_device;
    VkInstance vk_instance;
    VkBool32 supported;
    VkResult vr;

    vk_instance = chain->queue->device->vkd3d_instance->vk_instance;
    vk_physical_device = chain->queue->device->vk_physical_device;
    vr = IDXGIVkSurfaceFactory_CreateSurface(pFactory, vk_instance, vk_physical_device, &chain->vk_surface);

    if (vr < 0)
    {
        ERR("Failed to create surface, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    vr = VK_CALL(vkGetPhysicalDeviceSurfaceSupportKHR(vk_physical_device, chain->queue->vkd3d_queue->vk_family_index, chain->vk_surface, &supported));
    if (vr < 0)
    {
        ERR("Failed to query for surface support, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    if (!supported)
    {
        ERR("Surface is not supported for presentation.\n");
        return E_INVALIDARG;
    }

    pthread_mutex_init(&chain->properties.lock, NULL);

    return S_OK;
}

static HRESULT dxgi_vk_swap_chain_init_sync_objects(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkSemaphoreTypeCreateInfoKHR type_info;
    VkSemaphoreCreateInfo create_info;
    unsigned long latency_override;
    VkResult vr;
    char env[8];
    HRESULT hr;

    if (chain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
    {
        INFO("Enabling frame latency handles.\n");
        chain->frame_latency = 1;

        if (FAILED(hr = vkd3d_native_sync_handle_create(chain->frame_latency,
                VKD3D_NATIVE_SYNC_HANDLE_TYPE_SEMAPHORE, &chain->frame_latency_event)))
        {
            WARN("Failed to create frame latency semaphore, hr %#x.\n", hr);
            return hr;
        }
    }
    else
    {
#define DEFAULT_FRAME_LATENCY 3
        /* 3 frames is the default in the API, so that's what we should report to application if asks. */
        chain->frame_latency = DEFAULT_FRAME_LATENCY;
    }

    if (chain->queue->device->device_info.present_wait_features.presentWait)
    {
        /* Applications can easily forget to increment their latency handles.
         * This means we don't keep latency objects in sync anymore.
         * Maintain our own latency fence to keep things under control.
         * If we don't have present wait, we will sync with present_request_done_event (below) instead.
         * Adding more sync against internal blit fences is meaningless
         * and can cause weird pumping issues in some cases since we're simultaneously syncing
         * against two different timelines.
         * While we used to deduce latency based on BufferCount by default,
         * it was causing various issues, especially on Deck.
         * With 2 frame latency where CPU and GPU are decently subscribed,
         * the present wait event will come in with VBlank-alignment, and that extra delay can be enough to nudge CPU timings to the point
         * where GPU goes falsely idle. This ends up dropping GPU clocks, and we have bad feedback loop effects.
         * This effect has been observed in enough games now that it's too risky to enable that behavior by default. */
        chain->frame_latency_internal = DEFAULT_FRAME_LATENCY;

        if (vkd3d_get_env_var("VKD3D_SWAPCHAIN_LATENCY_FRAMES", env, sizeof(env)))
        {
            latency_override = strtoul(env, NULL, 0);
            if (latency_override >= 1 && latency_override <= DXGI_MAX_SWAP_CHAIN_BUFFERS)
                chain->frame_latency_internal = latency_override;
        }

        INFO("Ensure maximum latency of %u frames with KHR_present_wait.\n",
                chain->frame_latency_internal);

        /* On the first frame, we are supposed to acquire,
         * but we only acquire after a Present, so do the implied one here.
         * We consume a count after Present(), i.e. start of next frame,
         * starting with one less takes care of this. */
        if (FAILED(hr = vkd3d_native_sync_handle_create(chain->frame_latency_internal - 1,
                VKD3D_NATIVE_SYNC_HANDLE_TYPE_SEMAPHORE, &chain->frame_latency_event_internal)))
        {
            WARN("Failed to create internal frame latency semaphore, hr %#x.\n", hr);
            return hr;
        }
    }

    if (!chain->queue->device->device_info.present_wait_features.presentWait &&
            FAILED(hr = vkd3d_native_sync_handle_create(0, VKD3D_NATIVE_SYNC_HANDLE_TYPE_EVENT,
                    &chain->present_request_done_event)))
    {
        WARN("Failed to create internal present done event, hr %#x.\n", hr);
        return hr;
    }

    create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create_info.pNext = &type_info;
    create_info.flags = 0;
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR;
    type_info.pNext = NULL;
    type_info.initialValue = 0;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;

    vr = VK_CALL(vkCreateSemaphore(chain->queue->device->vk_device, &create_info,
            NULL, &chain->present.vk_complete_semaphore));
    if (vr < 0)
    {
        ERR("Failed to create timeline semaphore, vr %d.\n", vr);
        return hresult_from_vkd3d_result(vr);
    }

    vr = VK_CALL(vkCreateSemaphore(chain->queue->device->vk_device, &create_info,
            NULL, &chain->present.vk_internal_blit_semaphore));
    if (vr < 0)
    {
        ERR("Failed to create timeline semaphore, vr %d.\n", vr);
        return hresult_from_vkd3d_result(vr);
    }

    return S_OK;
}

static void dxgi_vk_swap_chain_drain_waiter(struct dxgi_vk_swap_chain *chain)
{
    /* Make sure wait thread is not waiting on anything before we destroy swapchain. */
    pthread_mutex_lock(&chain->wait_thread.lock);

    /* Skip ahead if there are multiple frames queued. */
    chain->wait_thread.skip_waits = true;
    while (chain->wait_thread.wait_queue_count || chain->wait_thread.display_latency.count)
        condvar_reltime_wait(&chain->wait_thread.cond,
                &chain->wait_thread.lock);
    chain->wait_thread.skip_waits = false;
    pthread_mutex_unlock(&chain->wait_thread.lock);
}

static void dxgi_vk_swap_chain_destroy_swapchain_in_present_task(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkQueue vk_queue;
    unsigned int i;

    dxgi_vk_swap_chain_fg_stop(chain);
    if (!chain->present.vk_swapchain)
        return;

    /* If we are going to destroy the swapchain and the device supports VK_NV_low_latency2
     * take the low latency lock. This ensures none of the other NV low latency functions
     * will attempt to use the stale swapchain handle. */
    if (chain->queue->device->vk_info.NV_low_latency2)
        pthread_mutex_lock(&chain->present.low_latency_swapchain_lock);

    if (chain->swapchain_maintenance1)
    {
        dxgi_vk_swap_chain_drain_swapchain_fences(chain);
    }
    else
    {
        /* Best effort workaround. */
        vk_queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
        VK_CALL(vkQueueWaitIdle(vk_queue));
        vkd3d_queue_release(chain->queue->vkd3d_queue);
    }

    dxgi_vk_swap_chain_drain_waiter(chain);

    for (i = 0; i < ARRAY_SIZE(chain->present.vk_backbuffer_image_views); i++)
        VK_CALL(vkDestroyImageView(chain->queue->device->vk_device, chain->present.vk_backbuffer_image_views[i], NULL));
    for (i = 0; i < ARRAY_SIZE(chain->present.vk_release_semaphores); i++)
        VK_CALL(vkDestroySemaphore(chain->queue->device->vk_device, chain->present.vk_release_semaphores[i], NULL));
    memset(chain->present.vk_backbuffer_images, 0, sizeof(chain->present.vk_backbuffer_images));
    memset(chain->present.vk_backbuffer_image_views, 0, sizeof(chain->present.vk_backbuffer_image_views));
    memset(chain->present.vk_release_semaphores, 0, sizeof(chain->present.vk_release_semaphores));

    VK_CALL(vkDestroySwapchainKHR(chain->queue->device->vk_device, chain->present.vk_swapchain, NULL));
    chain->present.vk_swapchain = VK_NULL_HANDLE;
    chain->present.backbuffer_width = 0;
    chain->present.backbuffer_height = 0;
    chain->present.backbuffer_format = VK_FORMAT_UNDEFINED;
    chain->present.backbuffer_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    memset(&chain->present.hdr.pipeline, 0, sizeof(chain->present.hdr.pipeline));
    chain->present.hdr.pipeline_mode = VKD3D_SWAPCHAIN_HDR_MODE_OFF;
    chain->present.hdr.pipeline_attempted = false;
    chain->present.backbuffer_count = 0;
    chain->present.force_swapchain_recreation = false;
    chain->present.present_id_valid = false;
    chain->present.present_id = 0;
    chain->present.current_backbuffer_index = UINT32_MAX;

    if (chain->queue->device->vk_info.NV_low_latency2)
    {
        spinlock_acquire(&chain->queue->device->low_latency_swapchain_spinlock);
        chain->queue->device->swapchain_info.vk_swapchain_count--;
        spinlock_release(&chain->queue->device->low_latency_swapchain_spinlock);

        pthread_mutex_unlock(&chain->present.low_latency_swapchain_lock);
    }
}

static VkColorSpaceKHR convert_color_space(DXGI_COLOR_SPACE_TYPE dxgi_color_space)
{
    switch (dxgi_color_space)
    {
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
            return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
            return VK_COLOR_SPACE_HDR10_ST2084_EXT;
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
        default:
            WARN("Unhandled color space %#x. Falling back to sRGB.\n", dxgi_color_space);
            return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
}

static bool dxgi_vk_swap_chain_accept_format(const VkSurfaceFormatKHR *format, VkFormat vk_format)
{
    if (vk_format == VK_FORMAT_UNDEFINED)
    {
        switch (format->format)
        {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
                return true;

            default:
                return false;
        }
    }
    else
    {
        return format->format == vk_format;
    }
}

static bool dxgi_vk_swap_chain_find_surface_format(struct dxgi_vk_swap_chain *chain, VkFormat vk_format,
        VkColorSpaceKHR color_space, VkSurfaceFormatKHR *format)
{
    bool ret = false;
    uint32_t i;

    pthread_mutex_lock(&chain->properties.lock);
    dxgi_vk_swap_chain_update_formats_locked(chain, false);
    for (i = 0; i < chain->properties.format_count && !ret; i++)
    {
        if (dxgi_vk_swap_chain_accept_format(&chain->properties.formats[i], vk_format) &&
                chain->properties.formats[i].colorSpace == color_space)
        {
            *format = chain->properties.formats[i];
            ret = true;
        }
    }
    pthread_mutex_unlock(&chain->properties.lock);
    return ret;
}

static bool dxgi_vk_swap_chain_select_format(struct dxgi_vk_swap_chain *chain, VkSurfaceFormatKHR *format)
{
    VkFormat vk_format = vkd3d_get_format(chain->queue->device, chain->request.dxgi_format, false)->vk_format;
    VkColorSpaceKHR vk_color_space = convert_color_space(chain->request.dxgi_color_space_type);

    if (dxgi_vk_swap_chain_find_surface_format(chain, vk_format, vk_color_space, format))
        return true;

    /* If we're using sRGB swapchains, we can fallback.
     * Usually happens for RGBA8 or 10-bit UNORM and display does not support it as a present format.
     * This can be trivially worked around by selecting e.g. BGRA8. */
    if (vk_color_space == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
        return dxgi_vk_swap_chain_find_surface_format(chain, VK_FORMAT_UNDEFINED, vk_color_space, format);
    }
    else
    {
        /* Refuse to present unsupported HDR since it will look completely bogus. */
        return false;
    }
}

static bool dxgi_vk_swap_chain_check_present_mode_support(struct dxgi_vk_swap_chain *chain, VkPresentModeKHR present_mode)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDevice vk_physical_device = chain->queue->device->vk_physical_device;
    VkPresentModeKHR supported_modes[16];
    uint32_t mode_count;
    uint32_t i;

    mode_count = ARRAY_SIZE(supported_modes);
    VK_CALL(vkGetPhysicalDeviceSurfacePresentModesKHR(vk_physical_device, chain->vk_surface, &mode_count, supported_modes));
    for (i = 0; i < mode_count; i++)
        if (supported_modes[i] == present_mode)
            return true;
    return false;
}

static void dxgi_vk_swap_chain_init_blit_pipeline(struct dxgi_vk_swap_chain *chain)
{
    struct d3d12_device *device = chain->queue->device;
    struct vkd3d_swapchain_pipeline_key key;
    HRESULT hr;

    key.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    key.filter = chain->desc.Scaling == DXGI_SCALING_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    key.format = chain->present.backbuffer_format;
    key.hdr_mode = VKD3D_SWAPCHAIN_HDR_MODE_OFF;

    hr = vkd3d_meta_get_swapchain_pipeline(&device->meta_ops, &key, &chain->present.pipeline);
    if (FAILED(hr))
        ERR("Failed to initialize swapchain pipeline.\n");
    vkd3d_hdr_trace("call=SwapchainBlitFormat dxgi_format=%#x vk_format=%d filter=%d result=%#x",
            chain->request.dxgi_format, key.format, key.filter, hr);
}

static bool dxgi_vk_swap_chain_init_hdr_pipeline(struct dxgi_vk_swap_chain *chain,
        enum vkd3d_swapchain_hdr_mode mode)
{
    struct d3d12_device *device = chain->queue->device;
    struct vkd3d_swapchain_pipeline_key key;
    HRESULT hr;

    if (!chain->present.hdr.pipeline_attempted ||
            chain->present.hdr.pipeline_mode != mode)
    {
        memset(&chain->present.hdr.pipeline, 0, sizeof(chain->present.hdr.pipeline));
        chain->present.hdr.pipeline_attempted = true;
        chain->present.hdr.pipeline_mode = mode;
        key.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
        key.filter = chain->desc.Scaling == DXGI_SCALING_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        key.format = chain->present.backbuffer_format;
        key.hdr_mode = mode;

        hr = vkd3d_meta_get_swapchain_pipeline(&device->meta_ops, &key,
                &chain->present.hdr.pipeline);
        if (FAILED(hr))
            ERR("PS5: Failed to initialize HDR %s swapchain pipeline, hr %#x.\n",
                    dxgi_vk_swap_chain_hdr_mode_name(mode), hr);
    }

    return chain->present.hdr.pipeline.vk_pipeline != VK_NULL_HANDLE;
}

static bool dxgi_vk_swap_chain_find_compatible_unlocked_present_mode(
        struct dxgi_vk_swap_chain *chain,
        VkPresentModeKHR *vk_present_mode,
        uint32_t *vk_min_image_count)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDeviceSurfaceInfo2KHR surface_info2;
    VkSurfacePresentModeCompatibilityEXT compat;
    VkSurfacePresentModeEXT present_mode;
    VkPresentModeKHR present_modes[32];
    VkSurfaceCapabilities2KHR caps2;
    bool has_compatible = false;
    uint32_t i;

    /* swapchain maintenance implies surface maintenance */
    if (!chain->swapchain_maintenance1)
        return false;

    surface_info2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surface_info2.pNext = NULL;
    surface_info2.surface = chain->vk_surface;
    present_mode.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT;
    present_mode.pNext = NULL;
    present_mode.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    vk_prepend_struct(&surface_info2, &present_mode);

    caps2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    caps2.pNext = NULL;
    compat.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT;
    compat.pNext = NULL;
    compat.presentModeCount = ARRAY_SIZE(present_modes);
    compat.pPresentModes = present_modes;
    vk_prepend_struct(&caps2, &compat);

    VK_CALL(vkGetPhysicalDeviceSurfaceCapabilities2KHR(chain->queue->device->vk_physical_device,
            &surface_info2, &caps2));

    for (i = 0; !has_compatible && i < compat.presentModeCount; i++)
    {
        if (compat.pPresentModes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
        {
            *vk_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            has_compatible = true;
        }
    }

    for (i = 0; !has_compatible && i < compat.presentModeCount; i++)
    {
        if (compat.pPresentModes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            *vk_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            has_compatible = true;
        }
    }

    if (!has_compatible)
        return false;

    /* This is the count for FIFO specifically. */
    *vk_min_image_count = caps2.surfaceCapabilities.minImageCount;

    caps2.pNext = NULL;
    present_mode.presentMode = *vk_present_mode;
    VK_CALL(vkGetPhysicalDeviceSurfaceCapabilities2KHR(chain->queue->device->vk_physical_device,
            &surface_info2, &caps2));

    /* Query for IMMEDIATE/MAILBOX specifically. */
    *vk_min_image_count = max(*vk_min_image_count, caps2.surfaceCapabilities.minImageCount);
    return true;
}

static void dxgi_vk_swap_chain_set_low_latency_state(struct dxgi_vk_swap_chain *chain, struct low_latency_state *low_latency_state)
{
    /* It is possible that the Vulkan swapchain does not exist when the application sets
     * the low latency state. If that is the case, just update the present latency state
     * and it will be set during dxgi_vk_swap_chain_recreate_swapchain_in_present_task. */
    if (chain->present.vk_swapchain)
    {
        const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
        VkLatencySleepModeInfoNV latency_sleep_mode_info;

        memset(&latency_sleep_mode_info, 0, sizeof(latency_sleep_mode_info));
        latency_sleep_mode_info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV;
        latency_sleep_mode_info.pNext = NULL;

        latency_sleep_mode_info.lowLatencyMode = low_latency_state->mode;
        latency_sleep_mode_info.lowLatencyBoost = low_latency_state->boost;
        latency_sleep_mode_info.minimumIntervalUs = low_latency_state->minimum_interval_us;

        VK_CALL(vkSetLatencySleepModeNV(chain->queue->device->vk_device, chain->present.vk_swapchain, &latency_sleep_mode_info));
    }

    chain->present.low_latency_state = *low_latency_state;
}

static void dxgi_vk_swap_chain_low_latency_state_update(struct dxgi_vk_swap_chain *chain)
{
    if (chain->request.low_latency_update_requested)
    {
        if (chain->present.low_latency_state.mode != chain->request.requested_low_latency_state.mode ||
                chain->present.low_latency_state.boost != chain->request.requested_low_latency_state.boost ||
                chain->present.low_latency_state.minimum_interval_us != chain->request.requested_low_latency_state.minimum_interval_us)
        {
            dxgi_vk_swap_chain_set_low_latency_state(chain, &chain->request.requested_low_latency_state);
        }
    }

    if (chain->debug_latency)
    {
        INFO("chain: %p, low latency mode: %s%s (%u us).\n",
                (void *)chain,
                chain->present.low_latency_state.mode ? "ON" : "OFF",
                chain->present.low_latency_state.boost ? " (+ boost)" : "",
                chain->present.low_latency_state.minimum_interval_us);
    }
}

static void dxgi_vk_swap_chain_anti_lag_state_update(struct dxgi_vk_swap_chain *chain)
{
    struct d3d12_device *device = chain->queue->device;

    if (chain->request.low_latency_frame_id)
    {
        const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
        VkAntiLagPresentationInfoAMD present_info;
        VkAntiLagDataAMD anti_lag;
        bool should_signal;

        memset(&anti_lag, 0, sizeof(anti_lag));
        memset(&present_info, 0, sizeof(present_info));
        anti_lag.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
        anti_lag.mode = chain->request.requested_anti_lag_state.mode ?
                VK_ANTI_LAG_MODE_ON_AMD : VK_ANTI_LAG_MODE_OFF_AMD;
        anti_lag.maxFPS = chain->request.requested_anti_lag_state.max_fps;
        anti_lag.pPresentationInfo = &present_info;

        present_info.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD;
        present_info.frameIndex = chain->request.low_latency_frame_id;
        present_info.stage = VK_ANTI_LAG_STAGE_PRESENT_AMD;

        /* Don't submit the same frame marker over and over. This will probably matter for frame-gen. */
        spinlock_acquire(&chain->queue->device->low_latency_swapchain_spinlock);
        should_signal = present_info.frameIndex > device->frame_markers.consumed_present_id;
        if (should_signal)
            device->frame_markers.consumed_present_id = present_info.frameIndex;
        spinlock_release(&chain->queue->device->low_latency_swapchain_spinlock);

        /* There is no strong requirement that frame index is submitted monotonically,
         * so it should be fine to drop the lock while calling UpdateAMD.
         * We don't want to hold a lock while calling AntiLagUpdateAMD.
         * This is only a theoretical problem if there are two physical queues that concurrently call
         * QueuePresentKHR. */
        if (should_signal)
        {
            TRACE("AntiLag present timeline, frame %"PRIu64".\n", present_info.frameIndex);
            VK_CALL(vkAntiLagUpdateAMD(device->vk_device, &anti_lag));
        }
    }
}

static void dxgi_vk_swap_chain_recreate_swapchain_in_present_task(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDevice vk_physical_device = chain->queue->device->vk_physical_device;
    VkSwapchainLatencyCreateInfoNV swapchain_latency_create_info;
    VkSwapchainPresentModesCreateInfoEXT present_modes_info;
    VkDevice vk_device = chain->queue->device->vk_device;
    VkCommandPoolCreateInfo command_pool_create_info;
    VkSwapchainCreateInfoKHR swapchain_create_info;
    VkPresentModeKHR present_mode_group[2];
    char present_mode_env[VKD3D_PATH_MAX];
    VkSurfaceCapabilitiesKHR surface_caps;
    VkSurfaceFormatKHR surface_format;
    VkImageViewCreateInfo view_info;
    uint32_t override_image_count;
    bool new_occlusion_state;
    char count_env[16];
    VkResult vr;
    uint32_t i;

    dxgi_vk_swap_chain_destroy_swapchain_in_present_task(chain);

    /* Don't bother if we've observed ERROR_SURFACE_LOST. */
    if (chain->present.is_surface_lost)
        return;

    /* If we fail to query formats we are hosed, treat it as a SURFACE_LOST scenario. */
    pthread_mutex_lock(&chain->properties.lock);
    /* This is only called on an event where we have to recreate the swapchain,
     * re-query format support at this time. It should not be called in the steady state. */
    if (!dxgi_vk_swap_chain_update_formats_locked(chain, true))
    {
        chain->present.is_surface_lost = true;
        pthread_mutex_unlock(&chain->properties.lock);
        return;
    }
    pthread_mutex_unlock(&chain->properties.lock);

    VK_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_physical_device, chain->vk_surface, &surface_caps));

    /* Win32 quirk. Minimized windows have maximum extents of zero. */
    new_occlusion_state = surface_caps.maxImageExtent.width == 0 || surface_caps.maxImageExtent.height == 0;
    vkd3d_atomic_uint32_store_explicit(&chain->present.is_occlusion_state, (uint32_t)new_occlusion_state, vkd3d_memory_order_relaxed);

    /* There is nothing to do. We'll do a dummy present. */
    if (new_occlusion_state)
        return;

    /* Sanity check, this cannot happen on Win32 surfaces, but could happen on Wayland. */
    if (surface_caps.currentExtent.width == UINT32_MAX || surface_caps.currentExtent.height == UINT32_MAX)
    {
        /* TODO: Can add extended interface to query surface size. */
        surface_caps.currentExtent.width = chain->desc.Width;
        surface_caps.currentExtent.height = chain->desc.Height;
    }

    /* No format to present to yet. Can happen in transition states for HDR.
     * Where we have modified color space, but not yet changed user backbuffer format. */
    if (!dxgi_vk_swap_chain_select_format(chain, &surface_format))
    {
        vkd3d_hdr_trace("call=SwapchainSurfaceFormat dxgi_format=%#x dxgi_color_space=%#x "
                "vk_format=unavailable vk_color_space=unavailable",
                chain->request.dxgi_format, chain->request.dxgi_color_space_type);
        vkd3d_ps5_d3d12_fail("SwapChain::Present", E_FAIL,
                "phase=surface_format dxgi_format=%u colorspace=%u size=%ux%u",
                chain->request.dxgi_format, chain->request.dxgi_color_space_type,
                chain->desc.Width, chain->desc.Height);
        return;
    }

    vkd3d_hdr_trace("call=SwapchainSurfaceFormat dxgi_format=%#x dxgi_color_space=%#x "
            "vk_format=%d vk_color_space=%d", chain->request.dxgi_format,
            chain->request.dxgi_color_space_type, surface_format.format, surface_format.colorSpace);

    memset(&swapchain_create_info, 0, sizeof(swapchain_create_info));
    swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;

    chain->present.override_present_mode = false;

    if (vkd3d_get_env_var("VKD3D_SWAPCHAIN_PRESENT_MODE", present_mode_env, sizeof(present_mode_env)))
    {
        VkPresentModeKHR candidate_present_mode;
        if (vkd3d_swapchain_present_mode_parse(present_mode_env, &candidate_present_mode))
        {
            if (dxgi_vk_swap_chain_check_present_mode_support(chain, candidate_present_mode))
            {
                chain->present.selected_present_mode = candidate_present_mode;
                chain->present.compatible_unlocked_present_mode = false;
                chain->present.override_present_mode = true;
                INFO("Overriding swapchain present mode to %s.\n", present_mode_env);
            }
            else
                WARN("Ignoring unsupported mode for VKD3D_SWAPCHAIN_PRESENT_MODE=%s.\n", present_mode_env);
        }
        else
            WARN("Ignoring unrecognized value for VKD3D_SWAPCHAIN_PRESENT_MODE=%s.\n", present_mode_env);
    }

    if (!chain->present.override_present_mode)
    {
        chain->present.compatible_unlocked_present_mode =
                dxgi_vk_swap_chain_find_compatible_unlocked_present_mode(chain,
                        &chain->present.unlocked_present_mode,
                        &surface_caps.minImageCount);

        if (chain->present.compatible_unlocked_present_mode)
        {
            /* Just start out in FIFO, we will change it at-will later. */
            chain->present.selected_present_mode = VK_PRESENT_MODE_FIFO_KHR;

            present_mode_group[0] = chain->present.selected_present_mode;
            present_mode_group[1] = chain->present.unlocked_present_mode;
            present_modes_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT;
            present_modes_info.pNext = NULL;
            present_modes_info.pPresentModes = present_mode_group;
            present_modes_info.presentModeCount = ARRAY_SIZE(present_mode_group);
            vk_prepend_struct(&swapchain_create_info, &present_modes_info);
        }
        else
        {
            chain->present.selected_present_mode = chain->request.swap_interval > 0 ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;

            /* Prefer IMMEDIATE over MAILBOX. FIFO is guaranteed to be supported. */
            if (chain->present.selected_present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR &&
                    !dxgi_vk_swap_chain_check_present_mode_support(chain, chain->present.selected_present_mode))
            {
                if (dxgi_vk_swap_chain_check_present_mode_support(chain, VK_PRESENT_MODE_MAILBOX_KHR))
                    chain->present.selected_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
                else
                    chain->present.selected_present_mode = VK_PRESENT_MODE_FIFO_KHR;
            }
        }
    }

    swapchain_create_info.surface = chain->vk_surface;
    swapchain_create_info.imageArrayLayers = 1;
    swapchain_create_info.imageColorSpace = surface_format.colorSpace;
    swapchain_create_info.imageFormat = surface_format.format;
    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchain_create_info.presentMode = chain->present.selected_present_mode;
    swapchain_create_info.clipped = VK_TRUE;

    /* We don't block directly on Present(), so there's no reason to use more than 3 images if even application requests more.
     * We could get away with 2 if we used WSI acquire semaphore and async acquire was supported, but e.g. Mesa does not support that.
     * However, without present-wait, we'll have to inherit quirky behavior from legacy swapchain
     * and use minImageCount = BufferCount + 1. */
    swapchain_create_info.minImageCount = max(max(chain->request.target_min_image_count, 3u),
            surface_caps.minImageCount);

    vkd3d_get_env_var("VKD3D_SWAPCHAIN_IMAGES", count_env, sizeof(count_env));
    if (strlen(count_env) > 0)
    {
        override_image_count = strtoul(count_env, NULL, 0);
        swapchain_create_info.minImageCount = max(surface_caps.minImageCount, override_image_count);
        INFO("Overriding swapchain images to %u.\n", swapchain_create_info.minImageCount);
    }

    if (surface_caps.maxImageCount && swapchain_create_info.minImageCount > surface_caps.maxImageCount)
        swapchain_create_info.minImageCount = surface_caps.maxImageCount;

    swapchain_create_info.imageExtent = surface_caps.currentExtent;
    swapchain_create_info.imageExtent.width = max(swapchain_create_info.imageExtent.width, surface_caps.minImageExtent.width);
    swapchain_create_info.imageExtent.width = min(swapchain_create_info.imageExtent.width, surface_caps.maxImageExtent.width);
    swapchain_create_info.imageExtent.height = max(swapchain_create_info.imageExtent.height, surface_caps.minImageExtent.height);
    swapchain_create_info.imageExtent.height = min(swapchain_create_info.imageExtent.height, surface_caps.maxImageExtent.height);

    if (chain->queue->device->vk_info.NV_low_latency2)
    {
        memset(&swapchain_latency_create_info, 0, sizeof(swapchain_latency_create_info));
        swapchain_latency_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV;
        swapchain_latency_create_info.pNext = NULL;
        swapchain_latency_create_info.latencyModeEnable = true;
        vk_prepend_struct(&swapchain_create_info, &swapchain_latency_create_info);
    }

    if (chain->queue->device->vk_info.NV_low_latency2)
        pthread_mutex_lock(&chain->present.low_latency_swapchain_lock);

    vr = VK_CALL(vkCreateSwapchainKHR(vk_device, &swapchain_create_info, NULL, &chain->present.vk_swapchain));
    if (vr < 0)
    {
        ERR("Failed to create swapchain, vr %d.\n", vr);
        chain->present.vk_swapchain = VK_NULL_HANDLE;
        if (chain->queue->device->vk_info.NV_low_latency2)
            pthread_mutex_unlock(&chain->present.low_latency_swapchain_lock);
        return;
    }

    /* If low latency is supported restore the current low latency state now */
    if (chain->queue->device->vk_info.NV_low_latency2)
    {
        struct d3d12_device *device = chain->queue->device;

        spinlock_acquire(&device->low_latency_swapchain_spinlock);
        device->swapchain_info.vk_swapchain_count++;

        if (device->swapchain_info.vk_swapchain_count > 1 && device->swapchain_info.low_latency_swapchain)
        {
            dxgi_vk_swap_chain_decref(device->swapchain_info.low_latency_swapchain);
            device->swapchain_info.low_latency_swapchain = NULL;
        }
        spinlock_release(&device->low_latency_swapchain_spinlock);

        dxgi_vk_swap_chain_set_low_latency_state(chain, &chain->present.low_latency_state);
        pthread_mutex_unlock(&chain->present.low_latency_swapchain_lock);
    }

    chain->present.backbuffer_count = ARRAY_SIZE(chain->present.vk_backbuffer_images);
    VK_CALL(vkGetSwapchainImagesKHR(vk_device, chain->present.vk_swapchain, &chain->present.backbuffer_count, chain->present.vk_backbuffer_images));

    INFO("Got %u swapchain images.\n", chain->present.backbuffer_count);

    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.format = swapchain_create_info.imageFormat;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.layerCount = 1;
    view_info.subresourceRange.levelCount = 1;

    for (i = 0; i < chain->present.backbuffer_count; i++)
    {
        view_info.image = chain->present.vk_backbuffer_images[i];
        VK_CALL(vkCreateImageView(vk_device, &view_info, NULL, &chain->present.vk_backbuffer_image_views[i]));
    }

    chain->present.backbuffer_width = swapchain_create_info.imageExtent.width;
    chain->present.backbuffer_height = swapchain_create_info.imageExtent.height;
    chain->present.backbuffer_format = swapchain_create_info.imageFormat;
    chain->present.backbuffer_color_space = swapchain_create_info.imageColorSpace;
    memset(&chain->present.hdr.pipeline, 0, sizeof(chain->present.hdr.pipeline));
    chain->present.hdr.pipeline_mode = VKD3D_SWAPCHAIN_HDR_MODE_OFF;
    chain->present.hdr.pipeline_attempted = false;
    chain->present.current_backbuffer_index = UINT32_MAX;

    if (!chain->present.vk_blit_command_pool)
    {
        command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_pool_create_info.pNext = NULL;
        command_pool_create_info.queueFamilyIndex = chain->queue->vkd3d_queue->vk_family_index;
        command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CALL(vkCreateCommandPool(vk_device, &command_pool_create_info, NULL, &chain->present.vk_blit_command_pool));
    }

    dxgi_vk_swap_chain_init_blit_pipeline(chain);
    dxgi_vk_swap_chain_set_hdr_metadata(chain);
}

static bool dxgi_vk_swap_chain_request_needs_swapchain_recreation(
        const struct dxgi_vk_swap_chain *chain,
        const struct dxgi_vk_swap_chain_present_request *request,
        const struct dxgi_vk_swap_chain_present_request *last_request)
{
    return request->dxgi_color_space_type != last_request->dxgi_color_space_type ||
            request->dxgi_format != last_request->dxgi_format ||
            request->target_min_image_count != last_request->target_min_image_count ||
            ((!!request->swap_interval) != (!!last_request->swap_interval) &&
                    !chain->present.compatible_unlocked_present_mode &&
                    !chain->present.override_present_mode);
}

static void dxgi_vk_swap_chain_present_complete(struct dxgi_vk_swap_chain *chain, uint64_t present_count, bool already_signalled)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    struct vkd3d_queue_timeline_trace_cookie cookie;
    VkSemaphoreSubmitInfo signal_semaphore_info;
    struct vkd3d_fence_wait_info fence_info;
    VkSubmitInfo2 submit_info;
    VkQueue vk_queue;
    VkResult vr = VK_SUCCESS;

    /* Guarantee a 1:1 ratio of Present() calls to increments here. This timeline is
     * used for user thread waiting as well as to delay  */
    if (!already_signalled)
    {
        memset(&signal_semaphore_info, 0, sizeof(signal_semaphore_info));
        signal_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_semaphore_info.semaphore = chain->present.vk_complete_semaphore;
        signal_semaphore_info.value = present_count;
        signal_semaphore_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        memset(&submit_info, 0, sizeof(submit_info));
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.signalSemaphoreInfoCount = 1;
        submit_info.pSignalSemaphoreInfos = &signal_semaphore_info;

        vk_queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
        vr = VK_CALL(vkQueueSubmit2(vk_queue, 1, &submit_info, VK_NULL_HANDLE));
        vkd3d_queue_release(chain->queue->vkd3d_queue);
    }

    /* Mark frame boundary. */
    cookie = vkd3d_queue_timeline_trace_register_swapchain_blit(
            &chain->queue->device->queue_timeline_trace,
            chain->present.present_id_valid ?
                    chain->present.present_id : present_count);

    if (vkd3d_queue_timeline_trace_cookie_is_valid(cookie))
    {
        memset(&fence_info, 0, sizeof(fence_info));
        fence_info.vk_semaphore = chain->present.vk_complete_semaphore;
        fence_info.vk_semaphore_value = chain->present.present_count;

        vkd3d_enqueue_timeline_semaphore(&chain->queue->fence_worker, &fence_info, &cookie);
    }

    if (vr)
    {
        ERR("Failed to submit present discard, vr = %d.\n", vr);
        VKD3D_DEVICE_REPORT_FAULT_AND_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);
    }
}

static void dxgi_vk_swap_chain_record_render_pass(struct dxgi_vk_swap_chain *chain, VkCommandBuffer vk_cmd, uint32_t swapchain_index)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkRenderingAttachmentInfo attachment_info;
    enum vkd3d_swapchain_hdr_mode hdr_mode;
    enum vkd3d_hdr_auto_match auto_observed, auto_content;
    const struct vkd3d_swapchain_info *pipeline;
    const struct dxgi_vk_hdr_push_constants *hdr_push;
    struct d3d12_resource *user_backbuffer;
    VkImageMemoryBarrier2 image_barrier[2];
    VkDescriptorImageInfo image_info;
    VkWriteDescriptorSet write_info;
    struct d3d12_resource *resource;
    VkRenderingInfo rendering_info;
    VkDependencyInfo dep_info;
    VkViewport viewport;
    uint64_t auto_writer;
    bool hdr_transform;
    bool blank_present;
    bool blit_command;
    VkImage source_image;
    VkImageView source_view;
    VkImageLayout source_layout;

    /* If application intends to present before we have rendered to it,
     * it is valid, but we need to ignore the blit, just clear backbuffer. */
    resource = chain->present.fg.source ? NULL : chain->user.backbuffers[chain->request.user_index];
    blank_present = resource && vkd3d_atomic_uint32_load_explicit(&resource->initial_layout_transition, vkd3d_memory_order_relaxed) != 0;

    if (blank_present)
        WARN("Application is presenting user index %u, but it has never been rendered to.\n", chain->request.user_index);

    /* PS5: Poll only real HDR10 swapchains. Off and non-HDR paths retain the
     * fixed-function blit; an active transform substitutes the shader blit. */
    hdr_mode = VKD3D_SWAPCHAIN_HDR_MODE_OFF;
    hdr_push = &chain->present.hdr.push;
    hdr_transform = false;
    dxgi_vk_swap_chain_hud_update(chain);
    if (chain->present.backbuffer_color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT)
    {
        dxgi_vk_swap_chain_update_hdr_settings(chain);
        hdr_mode = chain->present.hdr.effective.mode;
        if (hdr_mode == VKD3D_SWAPCHAIN_HDR_MODE_AUTO)
        {
            auto_observed = dxgi_vk_swap_chain_hdr_auto_observe(chain, &auto_writer);
            auto_content = dxgi_vk_swap_chain_hdr_auto_update(
                    chain, auto_observed, auto_writer);
            if (auto_content == VKD3D_HDR_AUTO_SDR)
            {
                hdr_mode = VKD3D_SWAPCHAIN_HDR_MODE_SDR;
            }
            else if (auto_content == VKD3D_HDR_AUTO_HDR &&
                    chain->present.hdr.effective.hdr_cap_nits >= 80.0f &&
                    chain->present.hdr.effective.hdr_cap_nits < 10000.0f)
            {
                hdr_mode = VKD3D_SWAPCHAIN_HDR_MODE_CAP;
                hdr_push = &chain->present.hdr.auto_cap_push;
            }
            else
            {
                hdr_mode = VKD3D_SWAPCHAIN_HDR_MODE_OFF;
            }
        }

        if (!blank_present &&
                (hdr_mode == VKD3D_SWAPCHAIN_HDR_MODE_SDR ||
                (hdr_mode == VKD3D_SWAPCHAIN_HDR_MODE_CAP &&
                hdr_push->peak_nits < 10000.0f)))
            hdr_transform = dxgi_vk_swap_chain_init_hdr_pipeline(chain, hdr_mode);
    }
    pipeline = hdr_transform ? &chain->present.hdr.pipeline : &chain->present.pipeline;

    memset(&attachment_info, 0, sizeof(attachment_info));
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    attachment_info.imageView = chain->present.vk_backbuffer_image_views[swapchain_index];
    attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment_info.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    if (chain->desc.Scaling == DXGI_SCALING_NONE || blank_present)
        attachment_info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;

    memset(&rendering_info, 0, sizeof(rendering_info));
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    rendering_info.renderArea.extent.width = chain->present.backbuffer_width;
    rendering_info.renderArea.extent.height = chain->present.backbuffer_height;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &attachment_info;

    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    if (chain->desc.Scaling == DXGI_SCALING_NONE)
    {
        viewport.width = (float)chain->desc.Width;
        viewport.height = (float)chain->desc.Height;
    }
    else
    {
        viewport.width = chain->present.backbuffer_width;
        viewport.height = chain->present.backbuffer_height;
    }

    /* Avoids having to go back to legacy heap. A PS5 HDR transform needs
     * shader math, so it replaces this fixed-function copy without a new pass. */
    blit_command = !blank_present && !hdr_transform &&
            viewport.width == (float)chain->present.backbuffer_width &&
            viewport.height == (float)chain->present.backbuffer_height;

    memset(&dep_info, 0, sizeof(dep_info));
    dep_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep_info.imageMemoryBarrierCount = blit_command ? 2 : 1;
    dep_info.pImageMemoryBarriers = image_barrier;

    /* srcStage = COLOR_ATTACHMENT to link up to acquire semaphore. */
    memset(image_barrier, 0, sizeof(image_barrier));
    image_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    image_barrier[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;

    user_backbuffer = chain->present.fg.source ? NULL : chain->user.backbuffers[chain->request.user_index];
    source_image = user_backbuffer ? user_backbuffer->res.vk_image : chain->present.fg.source->image;
    source_view = user_backbuffer ? chain->user.vk_image_views[chain->request.user_index] : chain->present.fg.source->view;
    source_layout = user_backbuffer ? user_backbuffer->common_layout : VK_IMAGE_LAYOUT_GENERAL;

    if (blit_command)
    {
        image_barrier[0].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        image_barrier[0].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        image_barrier[0].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        image_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        image_barrier[1].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        image_barrier[1].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        image_barrier[1].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        image_barrier[1].oldLayout = source_layout;
        image_barrier[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image_barrier[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier[1].image = source_image;
        image_barrier[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image_barrier[1].subresourceRange.levelCount = 1;
        image_barrier[1].subresourceRange.layerCount = 1;
    }
    else
    {
        image_barrier[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        image_barrier[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        image_barrier[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    image_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier[0].image = chain->present.vk_backbuffer_images[swapchain_index];
    image_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_barrier[0].subresourceRange.levelCount = 1;
    image_barrier[0].subresourceRange.layerCount = 1;
    if ((vkd3d_config_flags & VKD3D_CONFIG_FLAG_DEBUG_UTILS) &&
            chain->queue->device->vk_info.EXT_debug_utils)
    {
        VkDebugUtilsLabelEXT label;
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pNext = NULL;
        label.pLabelName = "BlitSwapChain";
        label.color[0] = 1.0f;
        label.color[1] = 1.0f;
        label.color[2] = 1.0f;
        label.color[3] = 1.0f;
        VK_CALL(vkCmdBeginDebugUtilsLabelEXT(vk_cmd, &label));
    }

    VK_CALL(vkCmdPipelineBarrier2(vk_cmd, &dep_info));

    if (blit_command)
    {
        VkImageBlit blit;
        memset(&blit, 0, sizeof(blit));
        blit.dstSubresource.layerCount = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource = blit.dstSubresource;

        blit.srcOffsets[1].x = (int)chain->desc.Width;
        blit.srcOffsets[1].y = (int)chain->desc.Height;
        blit.srcOffsets[1].z = 1;

        blit.dstOffsets[1].x = (int)chain->present.backbuffer_width;
        blit.dstOffsets[1].y = (int)chain->present.backbuffer_height;
        blit.dstOffsets[1].z = 1;

        VK_CALL(vkCmdBlitImage(vk_cmd, source_image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                chain->present.vk_backbuffer_images[swapchain_index],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit, VK_FILTER_LINEAR));
    }
    else
    {
        VK_CALL(vkCmdBeginRendering(vk_cmd, &rendering_info));

        if (!blank_present)
        {
            VK_CALL(vkCmdSetViewport(vk_cmd, 0, 1, &viewport));
            VK_CALL(vkCmdSetScissor(vk_cmd, 0, 1, &rendering_info.renderArea));
            VK_CALL(vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline->vk_pipeline));

            write_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_info.pNext = NULL;
            write_info.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write_info.pBufferInfo = NULL;
            write_info.dstSet = VK_NULL_HANDLE;
            write_info.pTexelBufferView = NULL;
            write_info.pImageInfo = &image_info;
            write_info.dstBinding = 0;
            write_info.dstArrayElement = 0;
            write_info.descriptorCount = 1;
            image_info.imageView = source_view;
            image_info.imageLayout = source_layout;
            image_info.sampler = VK_NULL_HANDLE;

            /* We will never have a heap here, so it's meaningless to try to use heap.
             * It's expected that driver will inherit the heap if it cares.
             * We could try using vkCmdBlitImage instead when possible. */
            VK_CALL(vkCmdPushDescriptorSetKHR(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline->vk_pipeline_layout, 0, 1, &write_info));

            if (hdr_transform)
            {
                VK_CALL(vkCmdPushConstants(vk_cmd, pipeline->vk_pipeline_layout,
                        VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                        sizeof(*hdr_push), hdr_push));
            }

            VK_CALL(vkCmdDraw(vk_cmd, 3, 1, 0, 0));
        }

        VK_CALL(vkCmdEndRendering(vk_cmd));
    }

    if (blit_command)
    {
		image_barrier[0].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        image_barrier[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;

        image_barrier[1].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        image_barrier[1].srcAccessMask = VK_ACCESS_2_NONE;
        image_barrier[1].dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        image_barrier[1].dstAccessMask = VK_ACCESS_2_NONE;
        image_barrier[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image_barrier[1].newLayout = source_layout;
    }
    else
    {
        image_barrier[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        image_barrier[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (chain->present.hud.settings.enabled)
    {
        image_barrier[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        image_barrier[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier[0].oldLayout = image_barrier[0].newLayout;
        image_barrier[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        /* Also restores the source image when the base pass used a blit. */
        VK_CALL(vkCmdPipelineBarrier2(vk_cmd, &dep_info));
        dxgi_vk_swap_chain_hud_draw(chain, vk_cmd, swapchain_index, &rendering_info);
        dep_info.imageMemoryBarrierCount = 1;
        image_barrier[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        image_barrier[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }

    image_barrier[0].dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    image_barrier[0].dstAccessMask = VK_ACCESS_2_NONE;
    image_barrier[0].oldLayout = image_barrier[0].newLayout;
    image_barrier[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VK_CALL(vkCmdPipelineBarrier2(vk_cmd, &dep_info));

    if ((vkd3d_config_flags & VKD3D_CONFIG_FLAG_DEBUG_UTILS) &&
            chain->queue->device->vk_info.EXT_debug_utils)
    {
        VK_CALL(vkCmdEndDebugUtilsLabelEXT(vk_cmd));
    }
}

static bool dxgi_vk_swap_chain_submit_blit(struct dxgi_vk_swap_chain *chain, uint32_t swapchain_index)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkDevice vk_device = chain->queue->device->vk_device;
    VkSemaphoreSubmitInfo signal_semaphore_info[3];
    VkSemaphoreCreateInfo semaphore_create_info;
    VkSemaphoreSubmitInfo wait_semaphore_info;
    VkCommandBufferAllocateInfo allocate_info;
    VkCommandBufferSubmitInfo cmd_buffer_info;
    VkCommandBufferBeginInfo cmd_begin_info;
    VkSubmitInfo2 submit_infos[2];
    VkCommandBuffer vk_cmd;
    VkQueue vk_queue;
    VkResult vr;

    /* Create objects on-demand. */
    if (!chain->present.vk_release_semaphores[swapchain_index])
    {
        semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore_create_info.pNext = NULL;
        semaphore_create_info.flags = 0;
        vr = VK_CALL(vkCreateSemaphore(vk_device, &semaphore_create_info,
                    NULL, &chain->present.vk_release_semaphores[swapchain_index]));
        if (vr < 0)
        {
            ERR("Failed to create semaphore, vr %d.\n", vr);
            return false;
        }
    }

    if (!chain->present.vk_blit_command_buffers[swapchain_index])
    {
        memset(&allocate_info, 0, sizeof(allocate_info));
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandBufferCount = 1;
        allocate_info.commandPool = chain->present.vk_blit_command_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        vr = VK_CALL(vkAllocateCommandBuffers(vk_device, &allocate_info,
                    &chain->present.vk_blit_command_buffers[swapchain_index]));
        if (vr < 0)
        {
            ERR("Failed to allocate command buffers, vr %d\n", vr);
            return false;
        }
    }
    vk_cmd = chain->present.vk_blit_command_buffers[swapchain_index];

    /* FG's worker must not stall after every output just to retire it. Wait
     * only when this WSI command-buffer/HUD-buffer slice is actually reused.
     * Private FG image read/write hazards remain ordered by queue barriers. */
    if (chain->present.fg.running && chain->present.backbuffer_blit_timelines[swapchain_index])
        dxgi_vk_swap_chain_drain_internal_blit_semaphore(chain,
                chain->present.backbuffer_blit_timelines[swapchain_index]);
    VK_CALL(vkResetCommandBuffer(vk_cmd, 0));
    memset(&cmd_begin_info, 0, sizeof(cmd_begin_info));
    cmd_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cmd_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CALL(vkBeginCommandBuffer(vk_cmd, &cmd_begin_info));
    /* FG work is completed ahead of its absolute output deadline by the
     * paced worker, not started after waking at the interpolation midpoint. */
    dxgi_vk_swap_chain_record_render_pass(chain, vk_cmd, swapchain_index);
    VK_CALL(vkEndCommandBuffer(vk_cmd));

    assert(chain->present.acquire_semaphore_signalled[chain->present.acquire_semaphore_index]);

    vk_queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);

    memset(&wait_semaphore_info, 0, sizeof(wait_semaphore_info));
    wait_semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_semaphore_info.semaphore = chain->present.vk_acquire_semaphore[chain->present.acquire_semaphore_index];
    wait_semaphore_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;

    memset(signal_semaphore_info, 0, sizeof(signal_semaphore_info));
    signal_semaphore_info[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_semaphore_info[0].semaphore = chain->present.vk_release_semaphores[swapchain_index];
    signal_semaphore_info[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    chain->present.internal_blit_count += 1;
    signal_semaphore_info[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_semaphore_info[1].semaphore = chain->present.vk_internal_blit_semaphore;
    signal_semaphore_info[1].value = chain->present.internal_blit_count;
    signal_semaphore_info[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    /* External submission */
    signal_semaphore_info[2].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_semaphore_info[2].semaphore = chain->queue->vkd3d_queue->submission_timeline;
    signal_semaphore_info[2].value = ++chain->queue->vkd3d_queue->submission_timeline_count;
    signal_semaphore_info[2].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    memset(&cmd_buffer_info, 0, sizeof(cmd_buffer_info));
    cmd_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_buffer_info.commandBuffer = vk_cmd;

    memset(submit_infos, 0, sizeof(submit_infos));
    submit_infos[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit_infos[0].commandBufferInfoCount = 1;
    submit_infos[0].pCommandBufferInfos = &cmd_buffer_info;
    submit_infos[0].pWaitSemaphoreInfos = &wait_semaphore_info;
    submit_infos[0].waitSemaphoreInfoCount = 1;
    submit_infos[0].signalSemaphoreInfoCount = 1;
    submit_infos[0].pSignalSemaphoreInfos = &signal_semaphore_info[0];

    /* Internal blit semaphore must be signaled after we signal vk_release_semaphores.
     * To guarantee this, the signals must happen in different batches. */
    submit_infos[1].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit_infos[1].signalSemaphoreInfoCount = 2;
    submit_infos[1].pSignalSemaphoreInfos = &signal_semaphore_info[1];

    vr = VK_CALL(vkQueueSubmit2(vk_queue, ARRAY_SIZE(submit_infos), submit_infos, VK_NULL_HANDLE));
    vkd3d_queue_release(chain->queue->vkd3d_queue);
    VKD3D_DEVICE_REPORT_FAULT_AND_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);

    if (vr < 0)
    {
        ERR("Failed to submit swapchain blit, vr %d.\n", vr);
        if (chain->present.fg.running)
        {
            /* No signal was queued for this value. The paced worker must not
             * wait forever on a timeline increment belonging to a failed submit. */
            --chain->present.internal_blit_count;
            chain->present.fg.output_success = false;
            pthread_mutex_lock(&chain->present.fg.lock);
            chain->present.fg.failed = true;
            pthread_mutex_unlock(&chain->present.fg.lock);
            ERR("WINEPS5 vkd3d fg status=SubmissionFailed method=%s vr=%d\n",
                    wineps5_fg_method_name(chain->present.fg.settings.method),vr);
        }
    }
    else
    {
        chain->present.backbuffer_blit_timelines[swapchain_index] =
                chain->present.internal_blit_count;
        chain->present.acquire_semaphore_consumed_at_blit[chain->present.acquire_semaphore_index] =
                chain->present.internal_blit_count;
        chain->present.acquire_semaphore_signalled[chain->present.acquire_semaphore_index] = false;
    }

    return vr == VK_SUCCESS;
}

static void dxgi_vk_swap_chain_present_recreate_swapchain_if_required(struct dxgi_vk_swap_chain *chain)
{
    if (!chain->present.vk_swapchain || chain->present.force_swapchain_recreation)
        dxgi_vk_swap_chain_recreate_swapchain_in_present_task(chain);
}

static VkResult dxgi_vk_swap_chain_ensure_unsignaled_acquire_semaphore(struct dxgi_vk_swap_chain *chain,
        uint32_t index, bool blocking)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkSemaphoreCreateInfo sem_info;
    VkResult vr = VK_SUCCESS;
    uint64_t drain_count;

    if (blocking)
    {
        /* Any pending wait must have been satisfied before we queue up a new signal. */
        drain_count = chain->present.acquire_semaphore_consumed_at_blit[index];
        if (drain_count)
            dxgi_vk_swap_chain_drain_internal_blit_semaphore(chain, drain_count);
    }

    if (chain->present.acquire_semaphore_signalled[index])
    {
        /* There is no pending wait, so we insert it now. */
        dxgi_vk_swap_chain_wait_acquire_semaphore(chain, chain->present.vk_acquire_semaphore[index], blocking);
        chain->present.acquire_semaphore_consumed_at_blit[index] = chain->present.internal_blit_count;
        chain->present.acquire_semaphore_signalled[index] = false;
    }

    if (!chain->present.vk_acquire_semaphore[index])
    {
        memset(&sem_info, 0, sizeof(sem_info));
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        if ((vr = VK_CALL(vkCreateSemaphore(chain->queue->device->vk_device, &sem_info,
                NULL, &chain->present.vk_acquire_semaphore[index]))) != VK_SUCCESS)
        {
            ERR("Failed to create semaphore, vr %d\n", vr);
            chain->present.vk_acquire_semaphore[index] = VK_NULL_HANDLE;
        }
    }

    return vr;
}

static VkResult dxgi_vk_swap_chain_try_acquire_next_image(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkDevice vk_device = chain->queue->device->vk_device;
    uint32_t semaphore_index;
    VkResult vr;

    if (!chain->present.vk_swapchain)
        return VK_ERROR_OUT_OF_DATE_KHR;
    if (chain->present.current_backbuffer_index != UINT32_MAX)
        return VK_SUCCESS;

    /* Ensure that we wait for semaphores before leaving it behind to avoid
     * having to do a blocking wait later. This can happen if we acquired successfully, but got
     * SUBOPTIMAL. That would leave a semaphore floating if we don't wait for it. */
    semaphore_index = chain->present.acquire_semaphore_index;
    vr = dxgi_vk_swap_chain_ensure_unsignaled_acquire_semaphore(chain, semaphore_index, false);
    if (vr != VK_SUCCESS)
        return vr;

    chain->present.acquire_semaphore_index = (chain->present.acquire_semaphore_index + 1) %
            ARRAY_SIZE(chain->present.vk_acquire_semaphore);
    semaphore_index = chain->present.acquire_semaphore_index;

    /* This should never actually block unless something unexpected happens. */
    vr = dxgi_vk_swap_chain_ensure_unsignaled_acquire_semaphore(chain, semaphore_index, true);
    if (vr != VK_SUCCESS)
        return vr;

    vr = VK_CALL(vkAcquireNextImageKHR(vk_device, chain->present.vk_swapchain, UINT64_MAX,
            chain->present.vk_acquire_semaphore[semaphore_index], VK_NULL_HANDLE,
            &chain->present.current_backbuffer_index));

    if (vr < 0)
        chain->present.current_backbuffer_index = UINT32_MAX;
    else
        chain->present.acquire_semaphore_signalled[semaphore_index] = true;

    return vr;
}

static void dxgi_vk_swap_chain_present_iteration(struct dxgi_vk_swap_chain *chain, uint64_t present_count, unsigned int retry_counter, bool generated)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkSwapchainPresentFenceInfoEXT present_fence_info;
    VkSwapchainPresentModeInfoEXT present_mode_info;
    VkPresentInfoKHR present_info;
    uint64_t minimum_present_id;
    uint64_t acquire_start_ns, acquire_ns;
    uint64_t queue_present_time_ns;
    VkPresentIdKHR present_id;
    uint32_t swapchain_index;
    bool use_present_id;
    VkResult vk_result;
    VkQueue vk_queue;
    VkResult vr;

    VKD3D_REGION_DECL(queue_present);
    if (chain->present.fg.running && (!chain->present.vk_swapchain || chain->present.force_swapchain_recreation))
    {
        pthread_mutex_lock(&chain->present.fg.lock);
        chain->present.fg.needs_rebuild = true;
        pthread_mutex_unlock(&chain->present.fg.lock);
        return;
    }

    dxgi_vk_swap_chain_present_recreate_swapchain_if_required(chain);
    if (!chain->present.vk_swapchain)
    {
        vkd3d_ps5_d3d12_fail("SwapChain::Present", E_FAIL,
                "phase=recreate retry=%u format=%u size=%ux%u",
                retry_counter, chain->desc.Format, chain->desc.Width, chain->desc.Height);
        return;
    }

    acquire_start_ns = chain->present.fg.running ? vkd3d_get_current_time_ns() : 0;
    vr = dxgi_vk_swap_chain_try_acquire_next_image(chain);
    if (chain->present.fg.running)
    {
        acquire_ns = vkd3d_get_current_time_ns() - acquire_start_ns;
        chain->present.fg.acquire_sum_ns += acquire_ns;
        chain->present.fg.acquire_max_ns = max(chain->present.fg.acquire_max_ns, acquire_ns);
        ++chain->present.fg.acquire_samples;
    }
    VKD3D_DEVICE_REPORT_FAULT_AND_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);
    if (vr < 0)
    {
        vkd3d_ps5_d3d12_fail("SwapChain::Present", hresult_from_vk_result(vr),
                "phase=acquire vr=%d retry=%u format=%u size=%ux%u",
                vr, retry_counter, chain->desc.Format, chain->desc.Width, chain->desc.Height);
    }

    /* Handle any errors and retry as needed. If we cannot make meaningful forward progress, just give up and retry later. */
    if (vr == VK_SUBOPTIMAL_KHR || vr < 0)
        chain->present.force_swapchain_recreation = true;
    if (chain->present.fg.running && chain->present.force_swapchain_recreation)
    {
        if (vr == VK_ERROR_SURFACE_LOST_KHR) chain->present.is_surface_lost = true;
        pthread_mutex_lock(&chain->present.fg.lock);
        chain->present.fg.needs_rebuild = true;
        pthread_mutex_unlock(&chain->present.fg.lock);
        ERR("WINEPS5 vkd3d fg status=SwapchainChanged method=%s vr=%d\n",
                wineps5_fg_method_name(chain->present.fg.settings.method),vr);
        return;
    }
    if (vr < 0)
        dxgi_vk_swap_chain_destroy_swapchain_in_present_task(chain);

    if (vr == VK_ERROR_OUT_OF_DATE_KHR)
    {
        if (retry_counter < 3)
            dxgi_vk_swap_chain_present_iteration(chain, present_count, retry_counter + 1, generated);
    }
    else if (vr == VK_ERROR_SURFACE_LOST_KHR)
    {
        /* If the surface is lost, we cannot expect to get forward progress. Just keep rendering to nothing. */
        chain->present.is_surface_lost = true;
    }

    if (vr < 0)
        return;

    swapchain_index = chain->present.current_backbuffer_index;
    assert(swapchain_index != UINT32_MAX);

    if (!dxgi_vk_swap_chain_submit_blit(chain, swapchain_index))
    {
        vkd3d_ps5_d3d12_fail("SwapChain::Present", E_FAIL,
                "phase=submit_blit image=%u format=%u size=%ux%u",
                swapchain_index, chain->desc.Format, chain->desc.Width, chain->desc.Height);
        return;
    }

    /* Only mark anti-lag update after we have submitted blit
     * so we don't get wrong associations. */
    if (!chain->present.fg.running && chain->queue->device->device_info.anti_lag_amd.antiLag)
        dxgi_vk_swap_chain_anti_lag_state_update(chain);

    memset(&present_info, 0, sizeof(present_info));
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.pSwapchains = &chain->present.vk_swapchain;
    present_info.swapchainCount = 1;
    present_info.pImageIndices = &swapchain_index;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &chain->present.vk_release_semaphores[swapchain_index];
    present_info.pResults = &vk_result;

    if (chain->swapchain_maintenance1)
    {
        chain->present.swapchain_fence_index = (chain->present.swapchain_fence_index + 1) %
                ARRAY_SIZE(chain->present.vk_swapchain_fences);

        present_fence_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
        present_fence_info.swapchainCount = 1;
        present_fence_info.pFences = &chain->present.vk_swapchain_fences[chain->present.swapchain_fence_index];
        present_fence_info.pNext = NULL;
        vk_prepend_struct(&present_info, &present_fence_info);

        dxgi_vk_swap_chain_ensure_unsignaled_swapchain_fence(chain, chain->present.swapchain_fence_index);
        chain->present.vk_swapchain_fences_signalled[chain->present.swapchain_fence_index] = true;

        if (chain->present.compatible_unlocked_present_mode)
        {
            present_mode_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT;
            present_mode_info.pNext = NULL;
            present_mode_info.swapchainCount = 1;
            present_mode_info.pPresentModes = &chain->present.selected_present_mode;
            chain->present.selected_present_mode = chain->request.swap_interval > 0 ?
                    VK_PRESENT_MODE_FIFO_KHR : chain->present.unlocked_present_mode;
            vk_prepend_struct(&present_info, &present_mode_info);
        }
    }


    /* Tag actual WSI outputs in both paths for nonblocking display telemetry.
     * The game latency handle still uses present-wait only for capped modes;
     * uncapped/FG game completion keeps its existing semaphore path. */
    if (chain->wait_thread.supports_present_wait &&
            (!chain->present.present_id_valid || chain->present.fg.running))
    {
        minimum_present_id = chain->present.present_id + 1;
        if (chain->present.low_latency_state.mode)
            chain->present.present_id = chain->request.low_latency_frame_id;
        else
            chain->present.present_id = present_count;

        /* Ensure present ID is increasing monotonically.
         * If application is exceptionally weird, i.e. does not set markers at all,
         * low latency will not work as intended. */
        chain->present.present_id = max(chain->present.present_id, minimum_present_id);

        /* We've now reached the point where any further submissions to any queue cannot affect this frame.
         * wait-before-signal is already resolved. Use a globally monotonic counter for low-latency swapchains. */
        if (chain->present.low_latency_state.mode)
        {
            struct vkd3d_device_frame_markers *markers = &chain->queue->device->frame_markers;
            spinlock_acquire(&chain->queue->device->low_latency_swapchain_spinlock);
            chain->present.present_id = max(chain->present.present_id, markers->consumed_present_id + 1);
            markers->consumed_present_id = chain->present.present_id;
            spinlock_release(&chain->queue->device->low_latency_swapchain_spinlock);
        }

        if (chain->debug_latency)
            INFO("Presenting with frame ID: %"PRIu64".\n", chain->present.present_id);

        present_id.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
        present_id.pNext = NULL;
        present_id.swapchainCount = 1;
        present_id.pPresentIds = &chain->present.present_id;
        use_present_id = true;
        vk_prepend_struct(&present_info, &present_id);
    }
    else
        use_present_id = false;

    /* Submit the real HDR/HUD/blit before the hold, not after it. Otherwise
     * the next game render can overtake that blit on our single queue and add
     * a base-frame of latency/backlog. Only WSI presentation is paced; acquire
     * order remains generated first, then real. No queue lock is held here. */
    if (chain->present.fg.running && !generated && chain->present.fg.present_deadline_ns)
        dxgi_vk_swap_chain_fg_wait_output(chain,chain->present.fg.present_deadline_ns,false);
    vk_queue = vkd3d_queue_acquire(chain->queue->vkd3d_queue);
    VKD3D_REGION_BEGIN(queue_present);
    if (chain->present.fg.running)
    {
        chain->present.fg.last_output_ns = vkd3d_get_current_time_ns();
        chain->present.fg.output_success = false;
    }
    vr = VK_CALL(vkQueuePresentKHR(vk_queue, &present_info));
    queue_present_time_ns = !generated ? vkd3d_get_current_time_ns() : 0;
    if (chain->present.fg.running)
        chain->present.fg.output_success = vr >= 0 && vk_result >= 0;
    if (chain->present.fg.running && chain->present.fg.output_success)
        wineps5_hud_note(&chain->present.hud.output_timing, chain->present.fg.last_output_ns);
    VKD3D_REGION_END(queue_present);
    vkd3d_queue_release(chain->queue->vkd3d_queue);
    if (!generated && vr >= 0 && vk_result >= 0)
        vkd3d_ps5_gpu_census_present_latency(chain->request.present_time_ns, queue_present_time_ns);
    if (!generated && use_present_id && vr >= 0 && vk_result >= 0)
        dxgi_vk_swap_chain_push_display_latency(chain,chain->present.present_id);
    if (chain->present.fg.running && vr >= 0 && vk_result >= 0)
    {
        if (!generated)
            vkd3d_ps5_gpu_census_fg_stage(WINEPS5_FG_STAGE_REAL_PRESENT,chain->request.present_time_ns,
                    chain->present.fg.last_output_ns);
        else if (!chain->present.fg.generation)
            vkd3d_ps5_gpu_census_fg_stage(WINEPS5_FG_STAGE_FIRST_GENERATED,chain->request.present_time_ns,
                    chain->present.fg.last_output_ns);
    }
    VKD3D_DEVICE_REPORT_FAULT_AND_BREADCRUMB_IF(chain->queue->device, vr == VK_ERROR_DEVICE_LOST);

    if (vr == VK_SUCCESS && vk_result != VK_SUCCESS)
        vr = vk_result;
    if (vr < 0)
    {
        vkd3d_ps5_d3d12_fail("SwapChain::Present", hresult_from_vk_result(vr),
                "phase=queue_present vr=%d retry=%u format=%u size=%ux%u",
                vr, retry_counter, chain->desc.Format, chain->desc.Width, chain->desc.Height);
    }

    if (vr >= 0)
        chain->present.current_backbuffer_index = UINT32_MAX;

    if (use_present_id && vr >= 0)
        chain->present.present_id_valid = true;

    vkd3d_queue_timeline_trace_register_instantaneous(&chain->queue->device->queue_timeline_trace,
            VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_QUEUE_PRESENT,
            chain->present.present_id_valid ? chain->present.present_id : 0);

    /* Handle any errors and retry as needed. If we cannot make meaningful forward progress, just give up and retry later. */
    if (vr == VK_SUBOPTIMAL_KHR || vr < 0)
        chain->present.force_swapchain_recreation = true;
    if (chain->present.fg.running && chain->present.force_swapchain_recreation)
    {
        if (vr == VK_ERROR_SURFACE_LOST_KHR) chain->present.is_surface_lost = true;
        pthread_mutex_lock(&chain->present.fg.lock);
        chain->present.fg.needs_rebuild = true;
        pthread_mutex_unlock(&chain->present.fg.lock);
        ERR("WINEPS5 vkd3d fg status=SwapchainChanged method=%s vr=%d\n",
                wineps5_fg_method_name(chain->present.fg.settings.method),vr);
        return;
    }
    if (vr < 0)
        dxgi_vk_swap_chain_destroy_swapchain_in_present_task(chain);

    if (vr == VK_ERROR_OUT_OF_DATE_KHR)
    {
        if (retry_counter < 3)
            dxgi_vk_swap_chain_present_iteration(chain, present_count, retry_counter + 1, generated);
    }
    else if (vr == VK_ERROR_SURFACE_LOST_KHR)
    {
        /* If the surface is lost, we cannot expect to get forward progress. Just keep rendering to nothing. */
        chain->present.is_surface_lost = true;
    }
}

static void dxgi_vk_swap_chain_signal_waitable_handle(struct dxgi_vk_swap_chain *chain, uint64_t present_count)
{
    bool pacing_should_wait = present_mode_pacing_should_wait(chain->present.selected_present_mode) ||
            chain->present.low_latency_state.mode;
    uint64_t present_id = pacing_should_wait && chain->present.present_id_valid ? chain->present.present_id : 0;

    dxgi_vk_swap_chain_push_present_id(chain, present_count, present_id, chain->request.begin_frame_time_ns, 0);
}

static void dxgi_vk_swap_chain_delay_next_frame(struct dxgi_vk_swap_chain *chain, uint64_t current_time_ns);
#include "wineps5_fg_present.h"

static void dxgi_vk_swap_chain_present_callback(void *chain_)
{
    const struct dxgi_vk_swap_chain_present_request *next_request;
    struct dxgi_vk_swap_chain *chain = chain_;
    uint64_t next_present_count;
    uint64_t callback_begin_ns;
    unsigned int captured;
    bool fg_owned;
    HRESULT hr;

    next_present_count = chain->present.present_count + 1;
    next_request = &chain->request_ring[next_present_count % ARRAY_SIZE(chain->request_ring)];
    callback_begin_ns = vkd3d_atomic_uint32_load_explicit(&chain->present.fg.metrics_active,
            vkd3d_memory_order_acquire) ? vkd3d_get_current_time_ns() : 0;
    fg_owned = dxgi_vk_swap_chain_fg_capture(chain, next_request, next_present_count, &captured);
    if (fg_owned)
    {
        /* Exactly one game completion/latency signal, after the source copy,
         * never after interpolation, pacing, generated frames or scanout. */
        dxgi_vk_swap_chain_present_complete(chain, next_present_count, captured != UINT32_MAX);
        dxgi_vk_swap_chain_push_present_id(chain, next_present_count, 0, next_request->begin_frame_time_ns,
                captured != UINT32_MAX ? next_request->present_time_ns : 0);
        vkd3d_atomic_uint64_store_explicit(&chain->present.present_count,
                next_present_count, vkd3d_memory_order_release);
        dxgi_vk_swap_chain_fg_note_callback_block(chain, callback_begin_ns);
        goto completed;
    }
    if (dxgi_vk_swap_chain_request_needs_swapchain_recreation(chain, next_request, &chain->request))
        chain->present.force_swapchain_recreation = true;

    chain->request = *next_request;
    if (chain->request.modifies_hdr_metadata)
        dxgi_vk_swap_chain_set_hdr_metadata(chain);

    if (chain->queue->device->vk_info.NV_low_latency2)
        dxgi_vk_swap_chain_low_latency_state_update(chain);

    /* If no QueuePresentKHRs successfully commits a present ID, we'll fallback to a normal queue signal. */
    chain->present.present_id_valid = false;

    /* A present iteration may or may not render to backbuffer. We'll apply best effort here.
     * Forward progress must be ensured, so if we cannot get anything on-screen in a reasonable amount of retries, ignore it. */
    dxgi_vk_swap_chain_present_iteration(chain, next_present_count, 0, false);

    /* When this is signalled, lets main thread know that it's safe to free user buffers.
     * Signal this just once on the outside since we might have retries, which complicates command buffer recycling. */
    dxgi_vk_swap_chain_present_complete(chain, next_present_count, false);

    /* Signal latency fence. */
    dxgi_vk_swap_chain_signal_waitable_handle(chain, next_present_count);

    /* Signal main thread that we are done with all CPU work.
     * No need to signal a condition variable, main thread can poll to deduce. */
    vkd3d_atomic_uint64_store_explicit(&chain->present.present_count, next_present_count, vkd3d_memory_order_release);
completed:

    /* Considerations for implementations without KHR_present_wait which we inherit from the legacy swapchain.
     * To have any hope of achieving reasonable and bounded latency,
     * we need to use the method where we eagerly acquire next image right after present.
     * This avoids any late blocking when presenting, which is a disaster for latency.
     * The present caller must then block on the queue reaching here.
     * To avoid hard deadlocks with present wait-before-signal,
     * a reasonably small timeout is possible.
     * A deadlock in this scenario has only been observed on NVIDIA, which supports present_wait anyways.
     *
     * On implementations with present wait, blocking late is a good idea since it avoids potential
     * GPU bubbles. While blocking here, we cannot submit more work to the GPU on this queue,
     * since we're in a queue callback. */
    if (!chain->wait_thread.supports_present_wait)
    {
        if (!fg_owned)
        {
            dxgi_vk_swap_chain_try_acquire_next_image(chain);
            dxgi_vk_swap_chain_delay_next_frame(chain, vkd3d_get_current_time_ns());
        }

        /* The old implementation used acquire semaphores, and did not block, so to maintain same behavior,
         * defer blocking on the VkFence. */
        if (vkd3d_native_sync_handle_is_valid(chain->present_request_done_event))
        {
            hr = vkd3d_native_sync_handle_signal(
                    chain->present_request_done_event);
            if (SUCCEEDED(hr))
                vkd3d_atomic_uint64_increment(&chain->stall.set_event_count,
                        vkd3d_memory_order_relaxed);
            else
                vkd3d_atomic_uint64_increment(
                        &chain->stall.set_event_fail_count,
                        vkd3d_memory_order_relaxed);
        }
    }

#ifdef VKD3D_ENABLE_BREADCRUMBS
    vkd3d_breadcrumb_tracer_update_barrier_hashes(&chain->queue->device->breadcrumb_tracer);
#endif

#ifdef VKD3D_ENABLE_PROFILING
    vkd3d_timestamp_profiler_mark_frame_boundary(chain->queue->device->timestamp_profiler);
#endif
}

static void dxgi_vk_swap_chain_update_frame_statistics(struct dxgi_vk_swap_chain *chain, uint64_t present_count)
{
    uint64_t time;

#ifdef _WIN32
    /* DXGI returns the raw QPC value */
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    time = li.QuadPart;
#else
    time = vkd3d_get_current_time_ns();
#endif

    spinlock_acquire(&chain->frame_statistics.lock);
    chain->frame_statistics.count = present_count;
    chain->frame_statistics.time = time;
    spinlock_release(&chain->frame_statistics.lock);
}

static void dxgi_vk_swap_chain_platform_sleep_for_ns(struct platform_sleep_state *sleep_state, uint64_t duration_ns)
{
#if defined(_WIN32)
    LARGE_INTEGER ticks;

    if (sleep_state->NtDelayExecution)
    {
        /* Ticks are in units of 100ns, with negative values indicating a
         * duration and positive values an absolute time in tick counts. */
        ticks.QuadPart = (int64_t)duration_ns / -100;

        /* Set alertable = false to avoid wineserver round-trips */
        sleep_state->NtDelayExecution(FALSE, &ticks);
    }
    else
    {
        Sleep(duration_ns / 1000000);
    }
#else
    struct timespec duration;

    (void)sleep_state;

    duration.tv_sec = duration_ns / 1000000000;
    duration.tv_nsec = duration_ns % 1000000000;

    /* ignore interrupts */
    nanosleep(&duration, NULL);
#endif
}

static void dxgi_vk_swap_chain_delay_next_frame(struct dxgi_vk_swap_chain *chain, uint64_t current_time_ns)
{
    struct platform_sleep_state *sleep_state = &chain->frame_rate_limit.sleep_state;
    uint64_t window_start_time_ns, window_total_time_ns, window_expected_time_ns;
    uint32_t frame_count_min, frame_count_max, frame_count;
    uint64_t sleep_threshold_ns, sleep_duration_ns;
    uint32_t frame_latency = DEFAULT_FRAME_LATENCY;
    static const uint32_t max_window_size = 128u;
    static const uint32_t min_window_size = 8u;
    uint64_t next_deadline_ns = 0;

    pthread_mutex_lock(&chain->frame_rate_limit.lock);

    if (chain->frame_rate_limit.target_interval_ns)
    {
        if (!chain->frame_rate_limit.enable)
        {
            /* Ignore app-provided frame latency since it may not be reliable */
            if (chain->queue->device->device_info.present_wait_features.presentWait)
                frame_latency = chain->frame_latency_internal;

            frame_count = chain->frame_rate_limit.heuristic_frame_count;

            if (frame_count >= min_window_size)
            {
                window_start_time_ns = chain->frame_rate_limit.heuristic_frame_time_ns;
                window_total_time_ns = current_time_ns - window_start_time_ns;

                window_expected_time_ns = frame_count * chain->frame_rate_limit.target_interval_ns;

                frame_count_min = frame_count - 1;
                frame_count_max = frame_count + frame_latency;

                if ((frame_count_max * window_total_time_ns) < (frame_count * window_expected_time_ns))
                {
                    /* Frame delivery has been faster than the refresh rate even
                     * accounting for swap chain buffering, enable limiter. */
                    chain->frame_rate_limit.enable = true;

                    INFO("Measured frame rate of %.1lf FPS exceeds desired refresh rate of %.1lf Hz, enabling limiter.\n",
                            1.0e9 / (double)(window_total_time_ns) * (double)(frame_count),
                            1.0e9 / (double)(chain->frame_rate_limit.target_interval_ns));
                }
                else if ((frame_count_min * window_total_time_ns) > (frame_count * window_expected_time_ns) ||
                        (frame_count >= max_window_size))
                {
                    /* Frame rate has been lower than the refresh rate, reset frame window. */
                    chain->frame_rate_limit.heuristic_frame_count = 0;
                    chain->frame_rate_limit.heuristic_frame_time_ns = 0;
                }
            }
        }

        if (chain->frame_rate_limit.enable)
        {
            next_deadline_ns = chain->frame_rate_limit.next_deadline_ns;

            /* Each frame is assigned a time window [t0,t1) during which presentation is expected to
             * complete, with its duration being equal to one frame interval. If a frame completes
             * before t1, just advance the next frame's time window by one frame interval and sleep
             * as necessary, otherwise reset the sequence using the current time as a starting point,
             * which should roughly line up with a display refresh. */
            if (current_time_ns < next_deadline_ns + chain->frame_rate_limit.target_interval_ns)
                chain->frame_rate_limit.next_deadline_ns += chain->frame_rate_limit.target_interval_ns;
            else
                chain->frame_rate_limit.next_deadline_ns = current_time_ns + chain->frame_rate_limit.target_interval_ns;
        }
        else
        {
            /* Advance or initialize the frame window used to measure the current frame rate. */
            if (!chain->frame_rate_limit.heuristic_frame_time_ns)
                chain->frame_rate_limit.heuristic_frame_time_ns = current_time_ns;

            chain->frame_rate_limit.heuristic_frame_count++;
        }
    }

    pthread_mutex_unlock(&chain->frame_rate_limit.lock);

    if (current_time_ns >= next_deadline_ns)
        return;

    /* Busy-wait for the last couple of milliseconds for accuracy, only use the
     * platform's sleep function for durations longer than the threshold, which
     * is an estimated error based on the timer interval and sleep duration. */
    sleep_duration_ns = next_deadline_ns - current_time_ns;
    sleep_threshold_ns = sleep_state->sleep_threshold_ns + sleep_duration_ns / 6;

    while (sleep_duration_ns > sleep_threshold_ns)
    {
        dxgi_vk_swap_chain_platform_sleep_for_ns(sleep_state, sleep_duration_ns - sleep_threshold_ns);

        current_time_ns = vkd3d_get_current_time_ns();
        sleep_duration_ns = next_deadline_ns > current_time_ns ? next_deadline_ns - current_time_ns : 0;
    }

    while (current_time_ns < next_deadline_ns)
    {
        vkd3d_pause();
        current_time_ns = vkd3d_get_current_time_ns();
    }
}

/* Observation of KHR present completion, never a wait in the game path.
 * The retained ring entry pins the old swapchain until drain_waiter has
 * requested skipping and this polling call has returned. MAILBOX can replace
 * an ID: completion then means that ID or a later one reached presentation. */
static void dxgi_vk_swap_chain_poll_display_latency(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    struct wineps5_display_latency_entry entry;
    uint64_t completion_ns;
    VkResult vr;
    bool skipped;
    for (;;)
    {
        pthread_mutex_lock(&chain->wait_thread.lock);
        if (chain->wait_thread.skip_waits)
        {
            chain->wait_thread.display_latency.count = 0;
            condvar_reltime_signal(&chain->wait_thread.cond);
        }
        /* Game latency-release work always takes priority over observation. */
        if (!chain->wait_thread.display_latency.count || chain->wait_thread.wait_queue_count)
        {
            pthread_mutex_unlock(&chain->wait_thread.lock);
            return;
        }
        entry = chain->wait_thread.display_latency.entries[chain->wait_thread.display_latency.head];
        pthread_mutex_unlock(&chain->wait_thread.lock);
        vr = VK_CALL(vkWaitForPresentKHR(chain->queue->device->vk_device,
                chain->present.vk_swapchain,entry.id,0));
        completion_ns = vkd3d_get_current_time_ns();
        pthread_mutex_lock(&chain->wait_thread.lock);
        skipped = chain->wait_thread.skip_waits;
        if (skipped || vr != VK_TIMEOUT)
            wineps5_display_latency_pop(&chain->wait_thread.display_latency);
        if (!chain->wait_thread.display_latency.count)
            condvar_reltime_signal(&chain->wait_thread.cond);
        pthread_mutex_unlock(&chain->wait_thread.lock);
        if (!skipped && vr == VK_SUCCESS)
        {
            vkd3d_ps5_gpu_census_display_latency(entry.present_ns,completion_ns);
            if (entry.fg)
                vkd3d_ps5_gpu_census_fg_stage(WINEPS5_FG_STAGE_DISPLAY_COMPLETE,entry.present_ns,completion_ns);
        }
        if (!skipped && vr == VK_TIMEOUT)
            return;
    }
}

static void *dxgi_vk_swap_chain_wait_worker(void *chain_)
{
    struct dxgi_vk_swap_chain *chain = chain_;
    struct vkd3d_queue_timeline_trace *timeline_trace =
            &chain->queue->device->queue_timeline_trace;
    const struct vkd3d_vk_device_procs *vk_procs =
            &chain->queue->device->vk_procs;
    struct vkd3d_queue_timeline_trace_cookie cookie;
    VkSemaphoreWaitInfo wait_info;
    struct present_wait_entry entry;
    uint64_t end_frame_time_ns = 0;
    int previous_semaphore;
    bool skip_waits;
    VkResult vr;
    int rc;

    vkd3d_set_thread_name("vkd3d-swapchain-sync");

    for (;;)
    {
        dxgi_vk_swap_chain_poll_display_latency(chain);
        pthread_mutex_lock(&chain->wait_thread.lock);
        while (!chain->wait_thread.wait_queue_count)
        {
            if (chain->wait_thread.display_latency.count)
                rc = condvar_reltime_wait_timeout_ms(&chain->wait_thread.cond, &chain->wait_thread.lock, 1);
            else
                rc = condvar_reltime_wait(&chain->wait_thread.cond, &chain->wait_thread.lock) ? -1 : 0;
            if (rc < 0)
            {
                ERR("Failed to wait on swapchain condition variable.\n");
                pthread_mutex_unlock(&chain->wait_thread.lock);
                return NULL;
            }
            pthread_mutex_unlock(&chain->wait_thread.lock);
            dxgi_vk_swap_chain_poll_display_latency(chain);
            pthread_mutex_lock(&chain->wait_thread.lock);
        }
        entry = chain->wait_thread.wait_queue[0];
        skip_waits = chain->wait_thread.skip_waits;
        pthread_mutex_unlock(&chain->wait_thread.lock);

        /* Sentinel for swapchain teardown. */
        if (!entry.present_count)
            break;

        if (entry.id)
        {
            cookie = vkd3d_queue_timeline_trace_register_present_wait(
                    timeline_trace, entry.id);
            /* PS5: Bounded waits let a concurrent recreation's skip request
             * retire an old present instead of waiting on it forever. */
            while (!skip_waits)
            {
                vr = VK_CALL(vkWaitForPresentKHR(
                        chain->queue->device->vk_device,
                        chain->present.vk_swapchain, entry.id,
                        1000000000ull));
                if (vr != VK_TIMEOUT)
                    break;

                dxgi_vk_swap_chain_check_latency_stall(chain);
                pthread_mutex_lock(&chain->wait_thread.lock);
                skip_waits = chain->wait_thread.skip_waits;
                pthread_mutex_unlock(&chain->wait_thread.lock);
            }
            vkd3d_queue_timeline_trace_complete_present_wait(
                    timeline_trace, cookie);
        }
        else
        {
            memset(&wait_info, 0, sizeof(wait_info));
            wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait_info.pSemaphores = &chain->present.vk_complete_semaphore;
            wait_info.pValues = &entry.present_count;
            wait_info.semaphoreCount = 1;
            do
            {
                vr = VK_CALL(vkWaitSemaphores(
                        chain->queue->device->vk_device, &wait_info,
                        1000000000ull));
                if (vr == VK_TIMEOUT)
                    dxgi_vk_swap_chain_check_latency_stall(chain);
            }
            while (vr == VK_TIMEOUT);

            if (vr != VK_SUCCESS)
                ERR("Failed to wait for present semaphore, vr %d.\n", vr);
        }

        end_frame_time_ns = vkd3d_get_current_time_ns();

        if (chain->wait_thread.supports_present_wait)
            dxgi_vk_swap_chain_delay_next_frame(chain, end_frame_time_ns);

        dxgi_vk_swap_chain_update_frame_statistics(chain, entry.present_count);

        if (vkd3d_native_sync_handle_is_valid(chain->frame_latency_event))
        {
            previous_semaphore = vkd3d_native_sync_handle_release(
                    chain->frame_latency_event, 1);
            if (previous_semaphore >= 0)
            {
                vkd3d_atomic_uint64_increment(
                        &chain->stall.release_semaphore_count,
                        vkd3d_memory_order_relaxed);

                /* Some applications can keep using MaxLatency of 1,
                 * but just avoid acquiring the semaphore a few times to achieve higher latency.
                 * Only warn if the counter becomes unreasonably large, e.g. 4+ frames.
                 * That's a good sign the application is actually not behaving correctly. */
                if (previous_semaphore >= (int)chain->frame_latency + 4)
                {
                    TRACE("Incrementing frame latency semaphore beyond max latency. "
                            "Did application forget to acquire? (new count = %d, max latency = %u)\n",
                            previous_semaphore + 1, chain->frame_latency);
                }
            }
            else
            {
                vkd3d_atomic_uint64_increment(
                        &chain->stall.release_semaphore_fail_count,
                        vkd3d_memory_order_relaxed);
                TRACE("Failed to increment swapchain semaphore. Did application forget to acquire?\n");
            }

            vkd3d_atomic_uint32_decrement(
                    &chain->stall.pending_latency_count,
                    vkd3d_memory_order_relaxed);
        }

        if (vkd3d_native_sync_handle_is_valid(
                chain->frame_latency_event_internal))
        {
            if (vkd3d_native_sync_handle_release(
                    chain->frame_latency_event_internal, 1) >= 0)
            {
                vkd3d_atomic_uint64_increment(
                        &chain->stall.internal_release_semaphore_count,
                        vkd3d_memory_order_relaxed);
            }
            else
            {
                vkd3d_atomic_uint64_increment(
                        &chain->stall.internal_release_semaphore_fail_count,
                        vkd3d_memory_order_relaxed);
            }
        }
        if (!entry.id && entry.fg_present_ns && vr == VK_SUCCESS)
            vkd3d_ps5_gpu_census_fg_stage(WINEPS5_FG_STAGE_COPY_COMPLETE,entry.fg_present_ns,end_frame_time_ns);

        if (entry.id && entry.begin_frame_time_ns)
            INFO("vkWaitForPresentKHR frame latency: %.3f ms.\n",
                    1e-6 * (end_frame_time_ns - entry.begin_frame_time_ns));

        /* Need to let present tasks know when it's safe to destroy a swapchain.
         * We must have completed all outstanding waits touching VkSwapchainKHR. */
        pthread_mutex_lock(&chain->wait_thread.lock);
        chain->wait_thread.wait_queue_count -= 1;
        memmove(chain->wait_thread.wait_queue,
                chain->wait_thread.wait_queue + 1,
                chain->wait_thread.wait_queue_count *
                sizeof(*chain->wait_thread.wait_queue));
        if (chain->wait_thread.wait_queue_count == 0)
            condvar_reltime_signal(&chain->wait_thread.cond);
        pthread_mutex_unlock(&chain->wait_thread.lock);
    }

    return NULL;
}

static HRESULT dxgi_vk_swap_chain_init_waiter_thread(struct dxgi_vk_swap_chain *chain)
{
    char env[8];

    spinlock_init(&chain->frame_statistics.lock);

    vkd3d_array_reserve((void **)&chain->wait_thread.wait_queue, &chain->wait_thread.wait_queue_size,
            DXGI_MAX_SWAP_CHAIN_BUFFERS, sizeof(*chain->wait_thread.wait_queue));
    pthread_mutex_init(&chain->wait_thread.lock, NULL);
    condvar_reltime_init(&chain->wait_thread.cond);

    /* Have to throw a thread under the bus unfortunately.
     * That thread will only wait on present IDs and release HANDLEs as necessary. */
    if (pthread_create(&chain->wait_thread.thread, NULL, dxgi_vk_swap_chain_wait_worker, chain))
    {
        pthread_mutex_destroy(&chain->wait_thread.lock);
        condvar_reltime_destroy(&chain->wait_thread.cond);
        return E_OUTOFMEMORY;
    }

    ERR("WINEPS5 vkd3d display_latency source=%s observation=poll_1ms_pending_only mailbox=completion_upper_bound\n",
            chain->queue->device->device_info.present_wait_features.presentWait ?
            "khr_present_wait_completion" : "unavailable");
    if ((chain->wait_thread.supports_present_wait = chain->queue->device->device_info.present_wait_features.presentWait))
    {
        if (vkd3d_get_env_var("VKD3D_SWAPCHAIN_DEBUG_LATENCY", env, sizeof(env)) && strcmp(env, "1") == 0)
            chain->debug_latency = true;

        INFO("Enabling present wait path for frame latency.\n");
    }

    return S_OK;
}

static HRESULT dxgi_vk_swap_chain_init_low_latency(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkPhysicalDevice vk_physical_device = chain->queue->device->vk_physical_device;

    VkLatencySurfaceCapabilitiesNV latency_surface_caps;
    VkSemaphoreTypeCreateInfoKHR semaphore_type_info;
    VkPhysicalDeviceSurfaceInfo2KHR surface_info;
    VkSurfaceCapabilities2KHR surface_caps;
    VkSemaphoreCreateInfo semaphore_info;
    VkResult vr;

    chain->present.low_latency_present_mode_count = 0;

    chain->present.low_latency_sem = VK_NULL_HANDLE;
    chain->present.low_latency_sem_value = 0;

    chain->present.low_latency_state.mode = false;
    chain->present.low_latency_state.boost = false;
    chain->present.low_latency_state.minimum_interval_us = 0;

    if (chain->queue->device->vk_info.NV_low_latency2)
    {
        memset(&surface_info, 0, sizeof(surface_info));
        surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
        surface_info.pNext = NULL;
        surface_info.surface = chain->vk_surface;

        memset(&latency_surface_caps, 0, sizeof(latency_surface_caps));
        latency_surface_caps.sType = VK_STRUCTURE_TYPE_LATENCY_SURFACE_CAPABILITIES_NV;
        latency_surface_caps.presentModeCount = ARRAY_SIZE(chain->present.low_latency_present_modes);
        latency_surface_caps.pPresentModes = chain->present.low_latency_present_modes;

        memset(&surface_caps, 0, sizeof(surface_caps));
        surface_caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
        surface_caps.pNext = &latency_surface_caps;

        if ((vr = VK_CALL(vkGetPhysicalDeviceSurfaceCapabilities2KHR(vk_physical_device, &surface_info,
                &surface_caps))) < 0)
        {
            ERR("Failed to query latency surface capabilities count, vr %d.\n", vr);
            return hresult_from_vk_result(vr);
        }

        chain->present.low_latency_present_mode_count = latency_surface_caps.presentModeCount;

        memset(&semaphore_type_info, 0, sizeof(semaphore_type_info));
        semaphore_type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR;
        semaphore_type_info.pNext = NULL;
        semaphore_type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
        semaphore_type_info.initialValue = 0;

        memset(&semaphore_info, 0, sizeof(semaphore_info));
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore_info.pNext = &semaphore_type_info;
        semaphore_info.flags = 0;

        if ((vr = VK_CALL(vkCreateSemaphore(chain->queue->device->vk_device, &semaphore_info,
                NULL, &chain->present.low_latency_sem))) < 0)
        {
            ERR("Failed to create semaphore, vr %d.\n", vr);
            return hresult_from_vk_result(vr);
        }

        pthread_mutex_init(&chain->present.low_latency_swapchain_lock, NULL);
        pthread_mutex_init(&chain->present.low_latency_state_update_lock, NULL);
    }

    return S_OK;
}

static HRESULT dxgi_vk_swap_chain_init_sleep_state(struct platform_sleep_state *sleep_state)
{
#if defined(_WIN32)
    uint64_t sleep_granularity_ns;
    ULONG min, max, cur;
    HMODULE ntdll;
    bool is_wine;

    is_wine = !!GetModuleHandleW(L"winevulkan.dll");

    if ((ntdll = GetModuleHandleW(L"ntdll.dll")))
    {
        sleep_state->NtQueryTimerResolution = (void*)GetProcAddress(ntdll, "NtQueryTimerResolution");
        sleep_state->NtSetTimerResolution = (void*)GetProcAddress(ntdll, "NtSetTimerResolution");
        sleep_state->NtDelayExecution = (void*)GetProcAddress(ntdll, "NtDelayExecution");
    }

    /* Older versions of Wine do not implement these functions, be robust here. */
    if (sleep_state->NtQueryTimerResolution && !sleep_state->NtQueryTimerResolution(&min, &max, &cur))
    {
        sleep_granularity_ns = 100 * cur;

        if (sleep_state->NtSetTimerResolution && !sleep_state->NtSetTimerResolution(max, TRUE, &cur))
            sleep_granularity_ns = 100 * max;

        INFO("Timer interval is %.1lf ms.\n", (double)sleep_granularity_ns / 1.0e6);
    }
    else
    {
        sleep_granularity_ns = 1000000;  /* 1ms */
    }

    /* This should always be available, however we can fall back to plain old Sleep() if not. */
    if (!sleep_state->NtDelayExecution)
        FIXME("NtDelayExecution not found in ntdll.\n");

    sleep_state->sleep_threshold_ns = (is_wine ? 1 : 4) * sleep_granularity_ns;
    return S_OK;
#else
    /* On native builds, we use nanosleep. Assume reasonable accuracy down to 1ms. */
    sleep_state->sleep_threshold_ns = 1000000;
    return S_OK;
#endif
}

static HRESULT dxgi_vk_swap_chain_init_frame_rate_limiter(struct dxgi_vk_swap_chain *chain)
{
    double target_frame_rate;
    char env[16];
    HRESULT hr;

    pthread_mutex_init(&chain->frame_rate_limit.lock, NULL);

    if (vkd3d_get_env_var("VKD3D_FRAME_RATE", env, sizeof(env)))
    {
        target_frame_rate = strtod(env, NULL);

        if (target_frame_rate > 0.0)
        {
            INFO("Set frame rate limit to %.1lf FPS via environment.\n", target_frame_rate);

            chain->frame_rate_limit.enable = true;
            chain->frame_rate_limit.has_user_override = true;
            chain->frame_rate_limit.target_interval_ns = (uint64_t)(1.0e9 / target_frame_rate);
        }
    }

    hr = dxgi_vk_swap_chain_init_sleep_state(&chain->frame_rate_limit.sleep_state);
    if (FAILED(hr))
        pthread_mutex_destroy(&chain->frame_rate_limit.lock);
    return hr;
}

static HRESULT dxgi_vk_swap_chain_init(struct dxgi_vk_swap_chain *chain, IDXGIVkSurfaceFactory *pFactory,
        const DXGI_SWAP_CHAIN_DESC1 *pDesc, struct d3d12_command_queue *queue)
{
    HRESULT hr;

    chain->IDXGIVkSwapChain_iface.lpVtbl = &dxgi_vk_swap_chain_vtbl;
    chain->refcount = 1;
    chain->internal_refcount = 1;
    chain->queue = queue;
    chain->desc = *pDesc;
    spinlock_init(&chain->present.fg.block_stats_lock);

    chain->swapchain_maintenance1 =
            queue->device->device_info.swapchain_maintenance1_features.swapchainMaintenance1 == VK_TRUE;

    INFO("Creating swapchain (%u x %u), BufferCount = %u.\n",
            pDesc->Width, pDesc->Height, pDesc->BufferCount);

    if (FAILED(hr = dxgi_vk_swap_chain_reallocate_user_buffers(chain)))
        goto cleanup_common;

    if (FAILED(hr = dxgi_vk_swap_chain_init_sync_objects(chain)))
        goto cleanup_common;

    if (FAILED(hr = dxgi_vk_swap_chain_create_surface(chain, pFactory)))
        goto cleanup_sync_objects;

    if (FAILED(hr = dxgi_vk_swap_chain_init_waiter_thread(chain)))
        goto cleanup_surface;

    if (FAILED(hr = dxgi_vk_swap_chain_init_low_latency(chain)))
        goto cleanup_waiter_thread;

    if (FAILED(hr = dxgi_vk_swap_chain_init_frame_rate_limiter(chain)))
        goto cleanup_low_latency;

    ID3D12CommandQueue_AddRef(&queue->ID3D12CommandQueue_iface);
    return S_OK;

cleanup_low_latency:
    dxgi_vk_swap_chain_cleanup_low_latency(chain);
cleanup_waiter_thread:
    dxgi_vk_swap_chain_cleanup_waiter_thread(chain);
cleanup_surface:
    dxgi_vk_swap_chain_cleanup_surface(chain);
cleanup_sync_objects:
    dxgi_vk_swap_chain_cleanup_sync_objects(chain);
cleanup_common:
    dxgi_vk_swap_chain_cleanup_common(chain);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_vk_swap_chain_factory_CreateSwapChain(IDXGIVkSwapChainFactory *iface,
        IDXGIVkSurfaceFactory *pFactory, const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIVkSwapChain **ppSwapchain)
{
    struct dxgi_vk_swap_chain_factory *factory = impl_from_IDXGIVkSwapChainFactory(iface);
    struct dxgi_vk_swap_chain *chain;
    HRESULT hr;

    TRACE("iface %p, pFactory %p, pDesc %p.\n", iface, pFactory, pDesc);
    vkd3d_ps5_d3d12_event("swapchain op=create size=%ux%u buffer_count=%u format=%u "
            "usage=0x%x swap_effect=%u flags=0x%x",
            pDesc->Width, pDesc->Height, pDesc->BufferCount, pDesc->Format,
            pDesc->BufferUsage, pDesc->SwapEffect, pDesc->Flags);

    chain = vkd3d_calloc(1, sizeof(*chain));
    if (!chain)
    {
        vkd3d_ps5_d3d12_fail("CreateSwapChain", E_OUTOFMEMORY,
                "size=%ux%u buffer_count=%u format=%u flags=0x%x",
                pDesc->Width, pDesc->Height, pDesc->BufferCount, pDesc->Format, pDesc->Flags);
        return E_OUTOFMEMORY;
    }

    if (FAILED(hr = dxgi_vk_swap_chain_init(chain, pFactory, pDesc, factory->queue)))
    {
        vkd3d_ps5_d3d12_fail("CreateSwapChain", hr,
                "size=%ux%u buffer_count=%u format=%u flags=0x%x",
                pDesc->Width, pDesc->Height, pDesc->BufferCount, pDesc->Format, pDesc->Flags);
        vkd3d_free(chain);
        return hr;
    }

    if (chain->queue->device->vk_info.NV_low_latency2)
        d3d12_device_register_swapchain(chain->queue->device, chain);

    *ppSwapchain = (IDXGIVkSwapChain*)&chain->IDXGIVkSwapChain_iface;
    return S_OK;
}

static CONST_VTBL struct IDXGIVkSwapChainFactoryVtbl dxgi_vk_swap_chain_factory_vtbl =
{
    /* IUnknown methods */
    dxgi_vk_swap_chain_factory_QueryInterface,
    dxgi_vk_swap_chain_factory_AddRef,
    dxgi_vk_swap_chain_factory_Release,

    /* IDXGIVkSwapChain methods */
    dxgi_vk_swap_chain_factory_CreateSwapChain,
};

bool dxgi_vk_swap_chain_low_latency_enabled(struct dxgi_vk_swap_chain *chain)
{
    return chain->present.low_latency_state.mode;
}

void dxgi_vk_swap_chain_latency_sleep(struct dxgi_vk_swap_chain *chain)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    struct vkd3d_queue_timeline_trace_cookie cookie;
    VkLatencySleepInfoNV latency_sleep_info;
    VkSemaphoreWaitInfo sem_wait_info;
    bool should_sleep = false;

    /* Increment the low latency sem value before the wait */
    chain->present.low_latency_sem_value++;

    memset(&latency_sleep_info, 0, sizeof(latency_sleep_info));
    latency_sleep_info.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV;
    latency_sleep_info.pNext = NULL;
    latency_sleep_info.signalSemaphore = chain->present.low_latency_sem;
    latency_sleep_info.value = chain->present.low_latency_sem_value;

    pthread_mutex_lock(&chain->present.low_latency_swapchain_lock);

    if (chain->present.vk_swapchain)
    {
        should_sleep = true;
        VK_CALL(vkLatencySleepNV(chain->queue->device->vk_device, chain->present.vk_swapchain, &latency_sleep_info));
    }

    pthread_mutex_unlock(&chain->present.low_latency_swapchain_lock);

    if (should_sleep)
    {
        memset(&sem_wait_info, 0, sizeof(sem_wait_info));
        sem_wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        sem_wait_info.pNext = NULL;
        sem_wait_info.flags = 0;
        sem_wait_info.semaphoreCount = 1;
        sem_wait_info.pSemaphores = &chain->present.low_latency_sem;
        sem_wait_info.pValues = &chain->present.low_latency_sem_value;

        cookie = vkd3d_queue_timeline_trace_register_low_latency_sleep(
                &chain->queue->device->queue_timeline_trace, chain->present.low_latency_sem_value);
        VK_CALL(vkWaitSemaphores(chain->queue->device->vk_device, &sem_wait_info, UINT64_MAX));
        vkd3d_queue_timeline_trace_complete_low_latency_sleep(
                &chain->queue->device->queue_timeline_trace, cookie);
    }
}

void dxgi_vk_swap_chain_set_latency_sleep_mode(struct dxgi_vk_swap_chain *chain, bool low_latency_mode,
	bool low_latency_boost, uint32_t minimum_interval_us)
{
    pthread_mutex_lock(&chain->present.low_latency_state_update_lock);

    chain->requested_low_latency_state.mode = low_latency_mode;
    chain->requested_low_latency_state.boost = low_latency_boost;
    chain->requested_low_latency_state.minimum_interval_us = minimum_interval_us;

    /* The actual call to vkSetLatencySleepModeNV will happen
     * when the application calls Present and the requested low
     * latency state is passed to the present task. */
    chain->low_latency_update_requested = true;

    pthread_mutex_unlock(&chain->present.low_latency_state_update_lock);
}

void dxgi_vk_swap_chain_set_latency_marker(struct dxgi_vk_swap_chain *chain, uint64_t frameID, VkLatencyMarkerNV marker)
{
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    VkSetLatencyMarkerInfoNV latency_marker_info;

    memset(&latency_marker_info, 0, sizeof(latency_marker_info));
    latency_marker_info.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV;
    latency_marker_info.pNext = NULL;
    latency_marker_info.presentID = frameID;
    latency_marker_info.marker = marker;

    if (chain->debug_latency && marker == VK_LATENCY_MARKER_PRESENT_START_NV)
        INFO("Setting present frame marker %"PRIu64".\n", frameID);

    pthread_mutex_lock(&chain->present.low_latency_swapchain_lock);

    if (chain->present.vk_swapchain)
        VK_CALL(vkSetLatencyMarkerNV(chain->queue->device->vk_device, chain->present.vk_swapchain, &latency_marker_info));

    pthread_mutex_unlock(&chain->present.low_latency_swapchain_lock);
}

void dxgi_vk_swap_chain_get_latency_info(struct dxgi_vk_swap_chain *chain, D3D12_LATENCY_RESULTS *latency_results)
{
    VkLatencyTimingsFrameReportNV frame_reports[ARRAY_SIZE(latency_results->frame_reports)];
    const struct vkd3d_vk_device_procs *vk_procs = &chain->queue->device->vk_procs;
    const VkLatencyTimingsFrameReportNV *last_effective_report = NULL;
    VkGetLatencyMarkerInfoNV marker_info;
    uint32_t i;

    /* There is no natural count, return blank output for missing output. */
    memset(latency_results->frame_reports, 0, sizeof(latency_results->frame_reports));

    pthread_mutex_lock(&chain->present.low_latency_swapchain_lock);

    if (chain->present.vk_swapchain)
    {
        memset(&marker_info, 0, sizeof(marker_info));
        marker_info.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV;

        VK_CALL(vkGetLatencyTimingsNV(chain->queue->device->vk_device, chain->present.vk_swapchain, &marker_info));

        /* Apparently we have to report all 64 entries, or nothing. */
        if (marker_info.timingCount >= ARRAY_SIZE(frame_reports))
        {
            memset(frame_reports, 0, sizeof(frame_reports));
            marker_info.timingCount = min(marker_info.timingCount, ARRAY_SIZE(frame_reports));
            for (i = 0; i < marker_info.timingCount; i++)
                frame_reports[i].sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
            marker_info.pTimings = frame_reports;

            VK_CALL(vkGetLatencyTimingsNV(chain->queue->device->vk_device, chain->present.vk_swapchain, &marker_info));

            for (i = 0; i < marker_info.timingCount; i++)
            {
                D3D12_FRAME_REPORT *report;

                /* If the frame ID isn't a natural aligned value,
                 * we assume it's a frame that the application never submitted a marker for.
                 * Non-aligned IDs appear when the monotonicity guard in the present path
                 * bumps a stale or duplicate low_latency_frame_id (e.g. after a Wayland
                 * compositor workspace switch stalls presents, or when DLSS Frame Generation
                 * presents interpolated frames without setting new latency markers).
                 * Skip these entries rather than discarding all reports, since Streamline's
                 * sl.dlss_g module checks latency reports to verify Reflex is active and
                 * will disable Frame Generation if all reports are zeroed. */
                if (frame_reports[i].presentID % VKD3D_LOW_LATENCY_FRAME_ID_STRIDE != 0 || frame_reports[i].presentID == 0)
                    continue;

                report = &latency_results->frame_reports[i];

                report->frameID = frame_reports[i].presentID / VKD3D_LOW_LATENCY_FRAME_ID_STRIDE;
                report->inputSampleTime = frame_reports[i].inputSampleTimeUs;
                report->simStartTime = frame_reports[i].simStartTimeUs;
                report->simEndTime = frame_reports[i].simEndTimeUs;
                report->renderSubmitStartTime = frame_reports[i].renderSubmitStartTimeUs;
                report->renderSubmitEndTime = frame_reports[i].renderSubmitEndTimeUs;
                report->presentStartTime = frame_reports[i].presentStartTimeUs;
                report->presentEndTime = frame_reports[i].presentEndTimeUs;
                report->driverStartTime = frame_reports[i].driverStartTimeUs;
                report->driverEndTime = frame_reports[i].driverEndTimeUs;
                report->osRenderQueueStartTime = frame_reports[i].osRenderQueueStartTimeUs;
                report->osRenderQueueEndTime = frame_reports[i].osRenderQueueEndTimeUs;
                report->gpuRenderStartTime = frame_reports[i].gpuRenderStartTimeUs;
                report->gpuRenderEndTime = frame_reports[i].gpuRenderEndTimeUs;
                report->gpuActiveRenderTimeUs = frame_reports[i].gpuRenderEndTimeUs - frame_reports[i].gpuRenderStartTimeUs;

                if (last_effective_report)
                    report->gpuFrameTimeUs = frame_reports[i].gpuRenderEndTimeUs - last_effective_report->gpuRenderEndTimeUs;
                else
                    report->gpuFrameTimeUs = 0;

                last_effective_report = &frame_reports[i];
            }
        }
    }

    pthread_mutex_unlock(&chain->present.low_latency_swapchain_lock);
}

ULONG dxgi_vk_swap_chain_incref(struct dxgi_vk_swap_chain *chain)
{
    ULONG refcount = InterlockedIncrement(&chain->internal_refcount);

    TRACE("%p increasing refcount to %u.\n", chain, refcount);

    return refcount;
}

ULONG dxgi_vk_swap_chain_decref(struct dxgi_vk_swap_chain *chain)
{
    ULONG refcount = InterlockedDecrement(&chain->internal_refcount);

    TRACE("%p decreasing refcount to %u.\n", chain, refcount);

    if (!refcount)
    {
        dxgi_vk_swap_chain_cleanup(chain);
        vkd3d_free(chain);
    }

    return refcount;
}

HRESULT dxgi_vk_swap_chain_factory_init(struct d3d12_command_queue *queue, struct dxgi_vk_swap_chain_factory *chain)
{
    chain->IDXGIVkSwapChainFactory_iface.lpVtbl = &dxgi_vk_swap_chain_factory_vtbl;
    chain->queue = queue;
    return S_OK;
}
