/*
 * Copyright 2016 Józef Kucia for CodeWeavers
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

#include "vkd3d_private.h"
#include "wineps5_fg.h"

#include <errno.h>
#include <stdarg.h>


#define VKD3D_MAX_DXGI_FORMAT DXGI_FORMAT_A4B4G4R4_UNORM

#define VKD3D_HDR_TRACE_MAX_LINES 256
#define VKD3D_HDR_TRACE_MAX_LINE_LENGTH 512

static pthread_once_t vkd3d_hdr_trace_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t vkd3d_hdr_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static bool vkd3d_hdr_trace_is_enabled;
static bool vkd3d_hdr_trace_limit_reported;
static size_t vkd3d_hdr_trace_line_count;
static char vkd3d_hdr_trace_lines[VKD3D_HDR_TRACE_MAX_LINES][VKD3D_HDR_TRACE_MAX_LINE_LENGTH];
#define VKD3D_HDR_PSO_TRACE_MAX_LINES 32
static size_t vkd3d_hdr_pso_trace_line_count;
static bool vkd3d_hdr_pso_trace_limit_reported;

#define VKD3D_HDR_MANIFEST_CAP (2u * 1024u * 1024u)
#define VKD3D_HDR_MANIFEST_LINE_LENGTH 4096

static pthread_mutex_t vkd3d_hdr_manifest_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t vkd3d_hdr_manifest_bytes;
static bool vkd3d_hdr_manifest_cap_reported;
static bool vkd3d_hdr_manifest_open_reported;

/* PS5: Keep query diagnostics cheap and finite even when a game resolves every frame. */
#define VKD3D_QUERY_TRACE_MAX_LINES 512
#define VKD3D_QUERY_TRACE_MAX_LINE_LENGTH 512

static pthread_once_t vkd3d_query_trace_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t vkd3d_query_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static bool vkd3d_query_trace_is_enabled;
static bool vkd3d_query_trace_limit_reported;
static size_t vkd3d_query_trace_line_count;

/* PS5: These diagnostics are always armed, but only failures, format queries,
 * and rare swapchain state changes enter the lock or format a line. */
#define VKD3D_PS5_D3D12_LOG_MAX_LINES 512
#define VKD3D_PS5_D3D12_FAIL_ENTRY_COUNT 256
#define VKD3D_PS5_D3D12_FORMAT_ENTRY_COUNT 512
#define VKD3D_PS5_D3D12_FAIL_FLUSH_NS (UINT64_C(10) * 1000 * 1000 * 1000)

struct vkd3d_ps5_d3d12_fail_entry
{
    const char *method;
    HRESULT hr;
    uint64_t count;
    uint64_t last_log_ns;
};

struct vkd3d_ps5_d3d12_format_entry
{
    const char *feature;
    DXGI_FORMAT format;
    uint32_t sample_count;
    uint32_t support1;
    uint32_t support2;
    HRESULT hr;
};

static pthread_mutex_t vkd3d_ps5_d3d12_log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vkd3d_ps5_d3d12_fail_entry
        vkd3d_ps5_d3d12_fail_entries[VKD3D_PS5_D3D12_FAIL_ENTRY_COUNT];
static struct vkd3d_ps5_d3d12_format_entry
        vkd3d_ps5_d3d12_format_entries[VKD3D_PS5_D3D12_FORMAT_ENTRY_COUNT];
static size_t vkd3d_ps5_d3d12_fail_entry_count;
static size_t vkd3d_ps5_d3d12_format_entry_count;
static size_t vkd3d_ps5_d3d12_log_line_count;
static bool vkd3d_ps5_d3d12_log_cap_reported;

#define VKD3D_PS5_GPU_CENSUS_WINDOW_NS (UINT64_C(10) * 1000 * 1000 * 1000)
#define VKD3D_PS5_GPU_CENSUS_QUEUE_COUNT 3

struct vkd3d_ps5_gpu_census_queue
{
    uint64_t submissions;
    uint64_t busy_ns;
    uint64_t latency_sum_ns;
    uint64_t latency_max_ns;
    uint64_t wait_ns;
    uint64_t last_complete_ns;
};

static pthread_mutex_t vkd3d_ps5_gpu_census_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vkd3d_ps5_gpu_census_queue
        vkd3d_ps5_gpu_census_queues[VKD3D_PS5_GPU_CENSUS_QUEUE_COUNT];
static uint64_t vkd3d_ps5_gpu_census_window_start_ns;
static uint64_t vkd3d_ps5_gpu_census_last_completion_ns;
static uint64_t vkd3d_ps5_gpu_census_frames;
static uint64_t vkd3d_ps5_gpu_census_latency_wait_ns;
static struct wineps5_present_latency vkd3d_ps5_present_latency;
static struct wineps5_present_latency vkd3d_ps5_display_latency;
static struct wineps5_fg_stage_stats vkd3d_ps5_fg_stages;

static struct wineps5_pso_stats vkd3d_ps5_pso;
static struct wineps5_pso_profile vkd3d_ps5_pso_profile;
static VKD3D_THREAD_LOCAL uint64_t vkd3d_ps5_pso_thread_compile_ns;
static uint64_t vkd3d_ps5_pso_draw_compile_ns, vkd3d_ps5_pso_last_present_ns;

/* PS5 per-hitch attribution (WINEPS5_PROFILE=1). The GPU accumulator is shared
 * under vkd3d_ps5_gpu_census_lock. Every thread that enters an instrumented
 * vkd3d call owns a registry slot whose running totals it publishes under a
 * seqlock; the thread whose Present ends a global interval snapshots all slots
 * and the process initial (main) thread under vkd3d_ps5_hitch_lock. A hitch is
 * printed one Present late, so CPU samples taken just after its end can
 * tighten the CPU-time bounds. */
#define VKD3D_PS5_HITCH_SLOTS 32
bool vkd3d_ps5_hitch_active;
static pthread_once_t vkd3d_ps5_hitch_once = PTHREAD_ONCE_INIT;
static struct wineps5_hitch_gpu vkd3d_ps5_hitch_gpu;

struct vkd3d_ps5_hitch_slot
{
    uint32_t tid;      /* 0: free; claimed and freed under vkd3d_ps5_hitch_lock */
    uint32_t sequence; /* odd while the owner updates totals/scope */
    struct wineps5_hitch_wine *wine;
    struct wineps5_hitch_thread totals;
    struct wineps5_hitch_scope_state scope;
    struct wineps5_hitch_track track; /* reader side, under the lock */
};

static pthread_mutex_t vkd3d_ps5_hitch_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vkd3d_ps5_hitch_slot vkd3d_ps5_hitch_slots[VKD3D_PS5_HITCH_SLOTS];
static struct
{
    struct wineps5_hitch_wine *wine;
    uint32_t tid;
    struct wineps5_hitch_track track; /* used while main owns no slot */
} vkd3d_ps5_hitch_main;
static struct wineps5_hitch_clock vkd3d_ps5_hitch_clock;
static struct wineps5_hitch_limiter vkd3d_ps5_hitch_limiter;
static uint64_t vkd3d_ps5_hitch_unslotted, vkd3d_ps5_hitch_torn;
/* Present-time scratch, under the lock (no allocation per frame). */
static struct wineps5_hitch_snapshot vkd3d_ps5_hitch_deltas[VKD3D_PS5_HITCH_SLOTS];
static struct wineps5_hitch_cpu_bound vkd3d_ps5_hitch_prev_bounds[VKD3D_PS5_HITCH_SLOTS];
static uint64_t vkd3d_ps5_hitch_spans[VKD3D_PS5_HITCH_SLOTS];
static uint64_t vkd3d_ps5_hitch_busy[VKD3D_PS5_HITCH_SLOTS];
static bool vkd3d_ps5_hitch_eligible[VKD3D_PS5_HITCH_SLOTS];
static bool vkd3d_ps5_hitch_stepped[VKD3D_PS5_HITCH_SLOTS];
static uint64_t vkd3d_ps5_hitch_cores_seen;

struct vkd3d_ps5_hitch_role
{
    const char *name;
    uint32_t tid;
    int slot; /* registry index, or -1 for main without a slot */
    uint64_t span_ns;
    struct wineps5_hitch_snapshot delta;
    struct wineps5_hitch_cpu_bound b0, b1; /* on cpu() at the interval's start and end */
};

struct vkd3d_ps5_hitch_event
{
    bool active;
    uint64_t seq, frame_ns;
    uint32_t presenter, main, busiest, live;
    struct wineps5_hitch_gpu gpu;
    unsigned int role_count;
    struct vkd3d_ps5_hitch_role roles[3];
};
static struct vkd3d_ps5_hitch_event vkd3d_ps5_hitch_pending;

struct vkd3d_ps5_hitch_tls
{
    struct vkd3d_ps5_hitch_slot *slot;
    unsigned int depth;
    bool unslotted;
};
static VKD3D_THREAD_LOCAL struct vkd3d_ps5_hitch_tls vkd3d_ps5_hitch_tls;

/* Read the current census window without resetting or perturbing its logger. */
void vkd3d_ps5_gpu_census_snapshot(struct vkd3d_ps5_gpu_snapshot *out)
{
    uint64_t now = vkd3d_get_current_time_ns(), elapsed;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    if (vkd3d_ps5_gpu_census_window_start_ns && now > vkd3d_ps5_gpu_census_window_start_ns)
    {
        elapsed = now - vkd3d_ps5_gpu_census_window_start_ns;
        out->window_s = elapsed * 1.0e-9;
        out->direct_busy_pct = min(100.0, 100.0 * vkd3d_ps5_gpu_census_queues[0].busy_ns / elapsed);
        out->latency_wait_ms = vkd3d_ps5_gpu_census_latency_wait_ns * 1.0e-6;
        out->present_latency_ms_mean = wineps5_present_latency_mean_ms(&vkd3d_ps5_present_latency);
        out->display_latency_ms_mean = wineps5_present_latency_mean_ms(&vkd3d_ps5_display_latency);
        out->display_latency_samples = vkd3d_ps5_display_latency.count;
    }
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
}

static void vkd3d_hdr_trace_init_once(void)
{
    char value[16];

    vkd3d_hdr_trace_is_enabled = vkd3d_get_env_var("WINEPS5_HDR_TRACE", value, sizeof(value))
            && !strcmp(value, "1");
    if (vkd3d_hdr_trace_is_enabled)
        ERR("WINEPS5_HDR_TRACE VKD3D enabled=1\n");
}

void vkd3d_hdr_trace_initialize(void)
{
    pthread_once(&vkd3d_hdr_trace_once, vkd3d_hdr_trace_init_once);
}

bool vkd3d_hdr_trace_enabled(void)
{
    vkd3d_hdr_trace_initialize();
    return vkd3d_hdr_trace_is_enabled;
}

void vkd3d_hdr_trace(const char *format, ...)
{
    char line[VKD3D_HDR_TRACE_MAX_LINE_LENGTH];
    va_list args;
    int length;
    size_t i;

    vkd3d_hdr_trace_initialize();
    if (!vkd3d_hdr_trace_is_enabled)
        return;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0)
        return;
    line[sizeof(line) - 1] = '\0';

    pthread_mutex_lock(&vkd3d_hdr_trace_lock);
    for (i = 0; i < vkd3d_hdr_trace_line_count; i++)
    {
        if (!strcmp(vkd3d_hdr_trace_lines[i], line))
        {
            pthread_mutex_unlock(&vkd3d_hdr_trace_lock);
            return;
        }
    }

    if (vkd3d_hdr_trace_line_count == VKD3D_HDR_TRACE_MAX_LINES)
    {
        if (!vkd3d_hdr_trace_limit_reported)
        {
            ERR("WINEPS5_HDR_TRACE VKD3D limit=256\n");
            vkd3d_hdr_trace_limit_reported = true;
        }
        pthread_mutex_unlock(&vkd3d_hdr_trace_lock);
        return;
    }

    strcpy(vkd3d_hdr_trace_lines[vkd3d_hdr_trace_line_count++], line);
    ERR("WINEPS5_HDR_TRACE VKD3D %s\n", line);
    pthread_mutex_unlock(&vkd3d_hdr_trace_lock);
}

static void vkd3d_query_trace_init_once(void)
{
    char value[16];

    vkd3d_query_trace_is_enabled = vkd3d_get_env_var("WINEPS5_QUERY_TRACE", value, sizeof(value))
            && !strcmp(value, "1");
    if (vkd3d_query_trace_is_enabled)
        ERR("WINEPS5_QUERY_TRACE VKD3D enabled=1 max_lines=%u max_values_per_resolve=%u\n",
                VKD3D_QUERY_TRACE_MAX_LINES, VKD3D_QUERY_TRACE_MAX_VALUES);
}

void vkd3d_query_trace_initialize(void)
{
    pthread_once(&vkd3d_query_trace_once, vkd3d_query_trace_init_once);
}

bool vkd3d_query_trace_enabled(void)
{
    vkd3d_query_trace_initialize();
    return vkd3d_query_trace_is_enabled;
}

void vkd3d_query_trace(const char *format, ...)
{
    char line[VKD3D_QUERY_TRACE_MAX_LINE_LENGTH];
    va_list args;
    int length;

    vkd3d_query_trace_initialize();
    if (!vkd3d_query_trace_is_enabled)
        return;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0)
        return;
    line[sizeof(line) - 1] = '\0';

    pthread_mutex_lock(&vkd3d_query_trace_lock);
    if (vkd3d_query_trace_line_count == VKD3D_QUERY_TRACE_MAX_LINES)
    {
        if (!vkd3d_query_trace_limit_reported)
        {
            ERR("WINEPS5_QUERY_TRACE VKD3D limit=%u\n", VKD3D_QUERY_TRACE_MAX_LINES);
            vkd3d_query_trace_limit_reported = true;
        }
        pthread_mutex_unlock(&vkd3d_query_trace_lock);
        return;
    }

    vkd3d_query_trace_line_count++;
    ERR("WINEPS5_QUERY_TRACE VKD3D %s\n", line);
    pthread_mutex_unlock(&vkd3d_query_trace_lock);
}

static bool vkd3d_ps5_d3d12_log_claim_locked(void)
{
    if (vkd3d_ps5_d3d12_log_line_count < VKD3D_PS5_D3D12_LOG_MAX_LINES)
    {
        vkd3d_ps5_d3d12_log_line_count++;
        return true;
    }

    if (!vkd3d_ps5_d3d12_log_cap_reported)
    {
        ERR("WINEPS5 d3d12 log cap reached\n");
        vkd3d_ps5_d3d12_log_cap_reported = true;
    }
    return false;
}

void vkd3d_ps5_d3d12_fail(const char *method, HRESULT hr, const char *format, ...)
{
    char detail[256];
    struct vkd3d_ps5_d3d12_fail_entry *entry = NULL;
    uint64_t now;
    va_list args;
    size_t i;

    if (format)
    {
        va_start(args, format);
        if (vsnprintf(detail, sizeof(detail), format, args) < 0)
            detail[0] = '\0';
        va_end(args);
        detail[sizeof(detail) - 1] = '\0';
    }
    else
    {
        strcpy(detail, "none");
    }

    now = vkd3d_get_current_time_ns();
    pthread_mutex_lock(&vkd3d_ps5_d3d12_log_lock);

    for (i = 0; i < vkd3d_ps5_d3d12_fail_entry_count; i++)
    {
        if (vkd3d_ps5_d3d12_fail_entries[i].hr == hr &&
                !strcmp(vkd3d_ps5_d3d12_fail_entries[i].method, method))
        {
            entry = &vkd3d_ps5_d3d12_fail_entries[i];
            break;
        }
    }

    if (!entry && vkd3d_ps5_d3d12_fail_entry_count < ARRAY_SIZE(vkd3d_ps5_d3d12_fail_entries))
    {
        entry = &vkd3d_ps5_d3d12_fail_entries[vkd3d_ps5_d3d12_fail_entry_count++];
        entry->method = method;
        entry->hr = hr;
        entry->count = 0;
        entry->last_log_ns = 0;
    }

    if (entry)
    {
        entry->count++;
        if (entry->count == 1 || now - entry->last_log_ns >= VKD3D_PS5_D3D12_FAIL_FLUSH_NS)
        {
            entry->last_log_ns = now;
            if (vkd3d_ps5_d3d12_log_claim_locked())
            {
                ERR("WINEPS5 d3d12 fail method=%s hr=0x%08x count=%"PRIu64" detail=%s\n",
                        method, (unsigned int)hr, entry->count, detail[0] ? detail : "none");
            }
        }
    }
    else if (vkd3d_ps5_d3d12_log_claim_locked())
    {
        ERR("WINEPS5 d3d12 fail method=%s hr=0x%08x count=1 detail=%s\n",
                method, (unsigned int)hr, detail[0] ? detail : "none");
    }

    pthread_mutex_unlock(&vkd3d_ps5_d3d12_log_lock);
}

void vkd3d_ps5_d3d12_format(const char *feature, DXGI_FORMAT format,
        uint32_t sample_count, uint32_t support1, uint32_t support2, HRESULT hr)
{
    struct vkd3d_ps5_d3d12_format_entry *entry;
    size_t i;

    pthread_mutex_lock(&vkd3d_ps5_d3d12_log_lock);
    for (i = 0; i < vkd3d_ps5_d3d12_format_entry_count; i++)
    {
        entry = &vkd3d_ps5_d3d12_format_entries[i];
        if (entry->format == format && entry->sample_count == sample_count &&
                entry->support1 == support1 && entry->support2 == support2 &&
                entry->hr == hr && !strcmp(entry->feature, feature))
        {
            pthread_mutex_unlock(&vkd3d_ps5_d3d12_log_lock);
            return;
        }
    }

    if (vkd3d_ps5_d3d12_format_entry_count < ARRAY_SIZE(vkd3d_ps5_d3d12_format_entries))
    {
        entry = &vkd3d_ps5_d3d12_format_entries[vkd3d_ps5_d3d12_format_entry_count++];
        entry->feature = feature;
        entry->format = format;
        entry->sample_count = sample_count;
        entry->support1 = support1;
        entry->support2 = support2;
        entry->hr = hr;

        if (vkd3d_ps5_d3d12_log_claim_locked())
        {
            ERR("WINEPS5 d3d12 format feature=%s format=%u sample_count=%u "
                    "support1=0x%08x support2=0x%08x hr=0x%08x\n",
                    feature, format, sample_count, support1, support2, (unsigned int)hr);
        }
    }
    else if (vkd3d_ps5_d3d12_log_claim_locked())
    {
        ERR("WINEPS5 d3d12 format feature=%s format=%u sample_count=%u "
                "support1=0x%08x support2=0x%08x hr=0x%08x\n",
                feature, format, sample_count, support1, support2, (unsigned int)hr);
    }

    pthread_mutex_unlock(&vkd3d_ps5_d3d12_log_lock);
}

void vkd3d_ps5_d3d12_event(const char *format, ...)
{
    char line[384];
    va_list args;

    va_start(args, format);
    if (vsnprintf(line, sizeof(line), format, args) < 0)
        line[0] = '\0';
    va_end(args);
    line[sizeof(line) - 1] = '\0';

    pthread_mutex_lock(&vkd3d_ps5_d3d12_log_lock);
    if (vkd3d_ps5_d3d12_log_claim_locked())
        ERR("WINEPS5 d3d12 %s\n", line);
    pthread_mutex_unlock(&vkd3d_ps5_d3d12_log_lock);
}

static int vkd3d_ps5_gpu_census_queue_index(D3D12_COMMAND_LIST_TYPE type)
{
    switch (type)
    {
        case D3D12_COMMAND_LIST_TYPE_DIRECT:
            return 0;
        case D3D12_COMMAND_LIST_TYPE_COMPUTE:
            return 1;
        case D3D12_COMMAND_LIST_TYPE_COPY:
            return 2;
        default:
            return -1;
    }
}

static void vkd3d_ps5_gpu_census_init_locked(uint64_t start_ns)
{
    size_t i;

    if (vkd3d_ps5_gpu_census_window_start_ns)
        return;

    vkd3d_ps5_gpu_census_window_start_ns = start_ns;
    for (i = 0; i < ARRAY_SIZE(vkd3d_ps5_gpu_census_queues); i++)
        vkd3d_ps5_gpu_census_queues[i].last_complete_ns = start_ns;
}

static double vkd3d_ps5_gpu_census_busy_pct(uint64_t busy_ns, uint64_t window_ns)
{
    double busy_pct;

    if (!window_ns)
        return 0.0;
    busy_pct = 100.0 * (double)busy_ns / (double)window_ns;
    return busy_pct < 100.0 ? busy_pct : 100.0;
}

static double vkd3d_ps5_gpu_census_mean_ms(
        const struct vkd3d_ps5_gpu_census_queue *queue)
{
    return queue->submissions
            ? 1.0e-6 * (double)queue->latency_sum_ns / (double)queue->submissions
            : 0.0;
}

static void vkd3d_ps5_gpu_census_emit_locked(uint64_t now_ns)
{
    struct vkd3d_ps5_gpu_census_queue *direct = &vkd3d_ps5_gpu_census_queues[0];
    struct vkd3d_ps5_gpu_census_queue *compute = &vkd3d_ps5_gpu_census_queues[1];
    struct vkd3d_ps5_gpu_census_queue *copy = &vkd3d_ps5_gpu_census_queues[2];
    uint64_t window_ns;
    size_t i;

    if (!vkd3d_ps5_gpu_census_window_start_ns ||
            now_ns < vkd3d_ps5_gpu_census_window_start_ns)
        return;
    window_ns = now_ns - vkd3d_ps5_gpu_census_window_start_ns;
    if (window_ns < VKD3D_PS5_GPU_CENSUS_WINDOW_NS)
        return;

    ERR("WINEPS5 vkd3d pso window_s=10 created=%"PRIu64" compile_ms_total=%.3f "
            "compile_ms_max=%.3f draw_time=%"PRIu64" cache_hits=%"PRIu64" cache_misses=%"PRIu64" "
            "spikes_with_compile=%"PRIu64"\n",
            vkd3d_ps5_pso.created, vkd3d_ps5_pso.total_ns * 1e-6,
            vkd3d_ps5_pso.max_ns * 1e-6, vkd3d_ps5_pso.draw_time,
            vkd3d_ps5_pso.cache_hits, vkd3d_ps5_pso.cache_misses, vkd3d_ps5_pso.spikes_with_compile);
    if (vkd3d_ps5_hitch_active)
    {
        const struct wineps5_pso_profile *p = &vkd3d_ps5_pso_profile;
        ERR("WINEPS5 vkd3d pso profile hit_ms_total=%.3f hit_ms_max=%.3f "
                "miss_ms_total=%.3f miss_ms_max=%.3f slow=%"PRIu64" slow_hits=%"PRIu64" failed=%"PRIu64" "
                "shaders=%"PRIu64" dxbc_spirv_ms_total=%.3f dxbc_spirv_ms_max=%.3f "
                "feedback_valid=%"PRIu64" feedback_app_hits=%"PRIu64" driver_ms_total=%.3f "
                "driver_stage_ms_total=%.3f details=%"PRIu64" suppressed=%"PRIu64"\n",
                p->hit_ns * 1e-6, p->hit_max_ns * 1e-6, p->miss_ns * 1e-6, p->miss_max_ns * 1e-6,
                p->slow, p->slow_hits, p->failed, p->shaders, p->shader_ns * 1e-6, p->shader_max_ns * 1e-6,
                p->feedback_valid, p->feedback_app_hits, p->driver_ns * 1e-6, p->driver_stage_ns * 1e-6,
                p->details, p->suppressed);
        ERR("WINEPS5 vkd3d pso library create=%"PRIu64"/%"PRIu64"/%08x "
                "store=%"PRIu64"/%"PRIu64"/%08x load=%"PRIu64"/%"PRIu64"/%08x "
                "serialize=%"PRIu64"/%"PRIu64"/%08x fields=ok/failed/last_hresult\n",
                p->library_ok[0], p->library_failed[0], p->library_last_error[0],
                p->library_ok[1], p->library_failed[1], p->library_last_error[1],
                p->library_ok[2], p->library_failed[2], p->library_last_error[2],
                p->library_ok[3], p->library_failed[3], p->library_last_error[3]);
        memset(&vkd3d_ps5_pso_profile, 0, sizeof(vkd3d_ps5_pso_profile));
    }
    memset(&vkd3d_ps5_pso, 0, sizeof(vkd3d_ps5_pso));
    ERR("WINEPS5 vkd3d gpu window_s=%.1f frames=%"PRIu64" "
            "queue=DIRECT submits=%"PRIu64" busy_pct=%.1f mean_ms=%.3f max_ms=%.3f wait_ms=%.3f "
            "queue=COMPUTE submits=%"PRIu64" busy_pct=%.1f mean_ms=%.3f max_ms=%.3f wait_ms=%.3f "
            "queue=COPY submits=%"PRIu64" busy_pct=%.1f mean_ms=%.3f max_ms=%.3f wait_ms=%.3f "
            "latency_wait_ms=%.3f present_latency_ms_mean=%.3f present_latency_ms_p50=%.3f present_latency_ms_p95=%.3f present_latency_ms_max=%.3f present_latency_samples=%"PRIu64" "
            "display_latency_ms_mean=%.3f display_latency_ms_p50=%.3f display_latency_ms_p95=%.3f display_latency_ms_max=%.3f display_latency_samples=%"PRIu64"\n",
            1.0e-9 * (double)window_ns, vkd3d_ps5_gpu_census_frames,
            direct->submissions, vkd3d_ps5_gpu_census_busy_pct(direct->busy_ns, window_ns),
            vkd3d_ps5_gpu_census_mean_ms(direct), 1.0e-6 * (double)direct->latency_max_ns,
            1.0e-6 * (double)direct->wait_ns,
            compute->submissions, vkd3d_ps5_gpu_census_busy_pct(compute->busy_ns, window_ns),
            vkd3d_ps5_gpu_census_mean_ms(compute), 1.0e-6 * (double)compute->latency_max_ns,
            1.0e-6 * (double)compute->wait_ns,
            copy->submissions, vkd3d_ps5_gpu_census_busy_pct(copy->busy_ns, window_ns),
            vkd3d_ps5_gpu_census_mean_ms(copy), 1.0e-6 * (double)copy->latency_max_ns,
            1.0e-6 * (double)copy->wait_ns,
            1.0e-6 * (double)vkd3d_ps5_gpu_census_latency_wait_ns,
            wineps5_present_latency_mean_ms(&vkd3d_ps5_present_latency),
            wineps5_present_latency_percentile_ms(&vkd3d_ps5_present_latency,50),
            wineps5_present_latency_percentile_ms(&vkd3d_ps5_present_latency,95),
            vkd3d_ps5_present_latency.max_ns * 1e-6,vkd3d_ps5_present_latency.count,
            wineps5_present_latency_mean_ms(&vkd3d_ps5_display_latency),
            wineps5_present_latency_percentile_ms(&vkd3d_ps5_display_latency,50),
            wineps5_present_latency_percentile_ms(&vkd3d_ps5_display_latency,95),
            vkd3d_ps5_display_latency.max_ns * 1e-6,vkd3d_ps5_display_latency.count);
    /* Absolute ages from the same game API timestamp, not stage durations.
     * Counts differ when captures coalesce or generated outputs are skipped. */
    ERR("WINEPS5 vkd3d fg stages window_s=%.1f capture_submit_ms_mean=%.3f copy_complete_ms_mean=%.3f worker_pickup_ms_mean=%.3f shared_complete_ms_mean=%.3f first_generated_ms_mean=%.3f real_present_ms_mean=%.3f display_complete_ms_mean=%.3f samples=%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64" completion=cpu_observed\n",
            1e-9 * (double)window_ns,
            wineps5_fg_block_mean_ms(&vkd3d_ps5_fg_stages.stages[WINEPS5_FG_STAGE_CAPTURE_SUBMIT]),
            wineps5_fg_block_mean_ms(&vkd3d_ps5_fg_stages.stages[WINEPS5_FG_STAGE_COPY_COMPLETE]),
            wineps5_fg_block_mean_ms(&vkd3d_ps5_fg_stages.stages[WINEPS5_FG_STAGE_WORKER_PICKUP]),
            wineps5_fg_block_mean_ms(&vkd3d_ps5_fg_stages.stages[WINEPS5_FG_STAGE_SHARED_COMPLETE]),
            wineps5_fg_block_mean_ms(&vkd3d_ps5_fg_stages.stages[WINEPS5_FG_STAGE_FIRST_GENERATED]),
            wineps5_fg_block_mean_ms(&vkd3d_ps5_fg_stages.stages[WINEPS5_FG_STAGE_REAL_PRESENT]),
            wineps5_fg_block_mean_ms(&vkd3d_ps5_fg_stages.stages[WINEPS5_FG_STAGE_DISPLAY_COMPLETE]),
            vkd3d_ps5_fg_stages.stages[0].count,vkd3d_ps5_fg_stages.stages[1].count,
            vkd3d_ps5_fg_stages.stages[2].count,vkd3d_ps5_fg_stages.stages[3].count,
            vkd3d_ps5_fg_stages.stages[4].count,vkd3d_ps5_fg_stages.stages[5].count,
            vkd3d_ps5_fg_stages.stages[6].count);

    memset(vkd3d_ps5_gpu_census_queues, 0, sizeof(vkd3d_ps5_gpu_census_queues));
    vkd3d_ps5_gpu_census_window_start_ns = now_ns;
    vkd3d_ps5_gpu_census_last_completion_ns = 0;
    vkd3d_ps5_gpu_census_frames = 0;
    vkd3d_ps5_gpu_census_latency_wait_ns = 0;
    memset(&vkd3d_ps5_present_latency,0,sizeof(vkd3d_ps5_present_latency));
    memset(&vkd3d_ps5_display_latency,0,sizeof(vkd3d_ps5_display_latency));
    memset(&vkd3d_ps5_fg_stages,0,sizeof(vkd3d_ps5_fg_stages));
    for (i = 0; i < ARRAY_SIZE(vkd3d_ps5_gpu_census_queues); i++)
        vkd3d_ps5_gpu_census_queues[i].last_complete_ns = now_ns;
}

void vkd3d_ps5_gpu_census_complete(D3D12_COMMAND_LIST_TYPE queue_type,
        uint64_t first_submit_ns, uint64_t submit_offset_sum_ns,
        uint32_t submit_count, uint64_t complete_ns)
{
    struct vkd3d_ps5_gpu_census_queue *queue;
    uint64_t busy_start_ns, batch_latency_ns, latency_sum_ns;
    int index;

    if (!submit_count || !first_submit_ns ||
            complete_ns < first_submit_ns ||
            (index = vkd3d_ps5_gpu_census_queue_index(queue_type)) < 0)
        return;

    batch_latency_ns = complete_ns - first_submit_ns;
    if (batch_latency_ns > UINT64_MAX / submit_count)
        latency_sum_ns = UINT64_MAX;
    else
    {
        latency_sum_ns = batch_latency_ns * submit_count;
        latency_sum_ns = latency_sum_ns >= submit_offset_sum_ns
                ? latency_sum_ns - submit_offset_sum_ns : 0;
    }

    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    vkd3d_ps5_gpu_census_init_locked(first_submit_ns);
    queue = &vkd3d_ps5_gpu_census_queues[index];

    busy_start_ns = max(first_submit_ns, queue->last_complete_ns);
    busy_start_ns = max(busy_start_ns, vkd3d_ps5_gpu_census_window_start_ns);
    if (complete_ns > busy_start_ns)
        queue->busy_ns += complete_ns - busy_start_ns;
    queue->last_complete_ns = max(queue->last_complete_ns, complete_ns);
    queue->submissions += submit_count;
    queue->latency_sum_ns += latency_sum_ns;
    queue->latency_max_ns = max(queue->latency_max_ns, batch_latency_ns);
    vkd3d_ps5_gpu_census_last_completion_ns = complete_ns;
    if (vkd3d_ps5_hitch_active)
        wineps5_hitch_gpu_complete(&vkd3d_ps5_hitch_gpu, index, batch_latency_ns, submit_count);

    vkd3d_ps5_gpu_census_emit_locked(complete_ns);
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
}

void vkd3d_ps5_gpu_census_queue_wait(D3D12_COMMAND_LIST_TYPE queue_type,
        uint64_t wait_ns, uint64_t now_ns)
{
    int index;

    if (!wait_ns || (index = vkd3d_ps5_gpu_census_queue_index(queue_type)) < 0)
        return;

    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    vkd3d_ps5_gpu_census_init_locked(now_ns >= wait_ns ? now_ns - wait_ns : now_ns);
    vkd3d_ps5_gpu_census_queues[index].wait_ns += wait_ns;
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
}

void vkd3d_ps5_gpu_census_present(uint64_t latency_wait_ns, uint64_t now_ns)
{
    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    vkd3d_ps5_gpu_census_init_locked(now_ns);
    if (wineps5_pso_spike(vkd3d_ps5_pso_last_present_ns, now_ns,
            max(vkd3d_ps5_pso_thread_compile_ns, vkd3d_ps5_pso_draw_compile_ns)))
        ++vkd3d_ps5_pso.spikes_with_compile;
    vkd3d_ps5_pso_last_present_ns = now_ns;
    vkd3d_ps5_gpu_census_frames++;
    vkd3d_ps5_gpu_census_latency_wait_ns += latency_wait_ns;

    if (now_ns - vkd3d_ps5_gpu_census_window_start_ns >= VKD3D_PS5_GPU_CENSUS_WINDOW_NS &&
            (!vkd3d_ps5_gpu_census_last_completion_ns ||
            now_ns - vkd3d_ps5_gpu_census_last_completion_ns >= VKD3D_PS5_GPU_CENSUS_WINDOW_NS))
        vkd3d_ps5_gpu_census_emit_locked(now_ns);
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
}

static void vkd3d_ps5_pso_record(uint64_t start_ns, uint64_t end_ns,
        VkResult vr, bool draw_time, bool disk_hit, const struct d3d12_pipeline_state *state,
        const char *kind, VkPipelineCreateFlags flags, uint32_t stages, const void *pnext)
{
    const VkPipelineCreationFeedbackCreateInfo *feedback = NULL;
    uint64_t driver_ns = 0, stage_ns = 0;
    uint32_t feedback_flags = 0, i;
    bool detail = false;

    if (vkd3d_ps5_hitch_active &&
            (feedback = vk_find_pnext(pnext, VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO)))
    {
        feedback_flags = feedback->pPipelineCreationFeedback->flags;
        if (feedback_flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)
            driver_ns = feedback->pPipelineCreationFeedback->duration;
        for (i = 0; i < feedback->pipelineStageCreationFeedbackCount; ++i)
            if (feedback->pPipelineStageCreationFeedbacks[i].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)
                stage_ns += feedback->pPipelineStageCreationFeedbacks[i].duration;
    }
    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    vkd3d_ps5_gpu_census_init_locked(start_ns);
    vkd3d_ps5_gpu_census_emit_locked(end_ns);
    wineps5_pso_note(&vkd3d_ps5_pso, end_ns - start_ns, vr == VK_SUCCESS, draw_time, disk_hit);
    if (vkd3d_ps5_hitch_active)
    {
        struct wineps5_pso_profile *p = &vkd3d_ps5_pso_profile;
        wineps5_pso_profile_note(p, end_ns - start_ns, vr == VK_SUCCESS, disk_hit);
        if (feedback_flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)
        {
            ++p->feedback_valid;
            p->feedback_app_hits += !!(feedback_flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
            p->driver_ns += driver_ns;
            p->driver_stage_ns += stage_ns;
        }
        detail = wineps5_pso_profile_detail(p, end_ns - start_ns);
    }
    /* A cache blob hit describes vkd3d's disk cache, not RADV's private cache.
     * Any successful Vulkan creation may still compile on the driver side. */
    if (vr == VK_SUCCESS)
    {
        vkd3d_ps5_pso_thread_compile_ns = end_ns;
        if (draw_time) vkd3d_ps5_pso_draw_compile_ns = end_ns;
    }
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
    if (detail)
        ERR("WINEPS5 vkd3d pso slow thread=%04x key=%016"PRIx64" api=%s kind=%s stages=%#x "
                "draw_time=%u blob_hit=%u app_blob=%u vk_ms=%.3f flags=%#x result=%d "
                "feedback_flags=%#x driver_ms=%.3f driver_stage_ms=%.3f\n",
                vkd3d_get_current_thread_id(),
                state ? vkd3d_pipeline_cache_compatibility_condense(&state->pipeline_cache_compat) : 0,
                state && state->ps5_creation_api ? state->ps5_creation_api : "internal",
                kind, stages,
                draw_time, disk_hit, state ? state->ps5_app_cached_blob : 0,
                (end_ns - start_ns) * 1e-6, flags, vr, feedback_flags, driver_ns * 1e-6, stage_ns * 1e-6);
}

void vkd3d_ps5_pso_shader(const struct d3d12_pipeline_state *state, VkShaderStageFlagBits stage,
        uint64_t start_ns, uint64_t end_ns, int result)
{
    struct wineps5_pso_profile *p = &vkd3d_ps5_pso_profile;
    uint64_t ns = end_ns - start_ns;
    bool detail;

    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    vkd3d_ps5_gpu_census_init_locked(start_ns);
    vkd3d_ps5_gpu_census_emit_locked(end_ns);
    ++p->shaders;
    p->shader_ns += ns;
    p->shader_max_ns = max(p->shader_max_ns, ns);
    detail = wineps5_pso_profile_detail(p, ns);
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
    if (detail)
        ERR("WINEPS5 vkd3d pso shader thread=%04x key=%016"PRIx64" stage=%#x "
                "dxbc_spirv_ms=%.3f result=%d\n", vkd3d_get_current_thread_id(),
                vkd3d_pipeline_cache_compatibility_condense(&state->pipeline_cache_compat),
                stage, ns * 1e-6, result);
}

HRESULT vkd3d_ps5_pso_library_result(enum wineps5_pso_library_op op, HRESULT hr)
{
    if (vkd3d_ps5_hitch_active)
    {
        struct wineps5_pso_profile *p = &vkd3d_ps5_pso_profile;
        pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
        if (SUCCEEDED(hr))
            ++p->library_ok[op];
        else
        {
            ++p->library_failed[op];
            p->library_last_error[op] = hr;
        }
        pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
    }
    return hr;
}

static void vkd3d_ps5_hitch_slot_write_begin(struct vkd3d_ps5_hitch_slot *s)
{
    __atomic_store_n(&s->sequence, s->sequence + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static void vkd3d_ps5_hitch_slot_write_end(struct vkd3d_ps5_hitch_slot *s)
{
    __atomic_store_n(&s->sequence, s->sequence + 1, __ATOMIC_RELEASE);
}

static void vkd3d_ps5_hitch_pipeline(uint64_t start_ns, uint64_t end_ns)
{
    struct vkd3d_ps5_hitch_slot *s;

    if (!vkd3d_ps5_hitch_active || !(s = vkd3d_ps5_hitch_tls.slot))
        return;
    vkd3d_ps5_hitch_slot_write_begin(s);
    s->totals.pipeline_ns += end_ns > start_ns ? end_ns - start_ns : 0;
    s->totals.pipeline_calls++;
    vkd3d_ps5_hitch_slot_write_end(s);
}

VkResult vkd3d_ps5_create_graphics_pipeline(struct d3d12_device *device, VkPipelineCache cache,
        const VkGraphicsPipelineCreateInfo *info, VkPipeline *pipeline, bool draw_time, bool disk_hit,
        const struct d3d12_pipeline_state *state)
{
    VKD3D_PS5_HITCH_SCOPE(WINEPS5_HITCH_PSO);
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkPipelineCreationFeedback stage_feedback[VKD3D_MAX_SHADER_STAGES], feedback;
    VkPipelineCreationFeedbackCreateInfo feedback_info;
    VkGraphicsPipelineCreateInfo profiled_info;
    uint64_t start_ns, end_ns;
    uint32_t stages = 0, i;
    VkResult vr;

    if (vkd3d_ps5_hitch_active)
    {
        for (i = 0; i < info->stageCount; ++i)
            stages |= info->pStages[i].stage;
        if (!vk_find_pnext(info->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO))
        {
            memset(&feedback, 0, sizeof(feedback));
            memset(stage_feedback, 0, sizeof(stage_feedback));
            memset(&feedback_info, 0, sizeof(feedback_info));
            feedback_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
            feedback_info.pNext = info->pNext;
            feedback_info.pPipelineCreationFeedback = &feedback;
            /* Linked libraries contribute additional stages that are not in
             * pStages. Request only aggregate feedback for that case. */
            if (!vk_find_pnext(info->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) &&
                    info->stageCount <= ARRAY_SIZE(stage_feedback))
            {
                feedback_info.pipelineStageCreationFeedbackCount = info->stageCount;
                feedback_info.pPipelineStageCreationFeedbacks = stage_feedback;
            }
            profiled_info = *info;
            profiled_info.pNext = &feedback_info;
            info = &profiled_info;
        }
    }
    start_ns = vkd3d_get_current_time_ns();
    vr = VK_CALL(vkCreateGraphicsPipelines(device->vk_device, cache, 1, info, NULL, pipeline));
    end_ns = vkd3d_get_current_time_ns();
    vkd3d_ps5_pso_record(start_ns, end_ns, vr, draw_time, disk_hit, state, "graphics", info->flags, stages, info->pNext);
    vkd3d_ps5_hitch_pipeline(start_ns, end_ns);
    return vr;
}

VkResult vkd3d_ps5_create_compute_pipeline(struct d3d12_device *device, VkPipelineCache cache,
        const VkComputePipelineCreateInfo *info, VkPipeline *pipeline, bool draw_time, bool disk_hit,
        const struct d3d12_pipeline_state *state)
{
    VKD3D_PS5_HITCH_SCOPE(WINEPS5_HITCH_PSO);
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkPipelineCreationFeedback stage_feedback, feedback;
    VkPipelineCreationFeedbackCreateInfo feedback_info;
    VkComputePipelineCreateInfo profiled_info;
    uint64_t start_ns, end_ns;
    VkResult vr;

    if (vkd3d_ps5_hitch_active &&
            !vk_find_pnext(info->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO))
    {
        memset(&feedback, 0, sizeof(feedback));
        memset(&stage_feedback, 0, sizeof(stage_feedback));
        memset(&feedback_info, 0, sizeof(feedback_info));
        feedback_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
        feedback_info.pNext = info->pNext;
        feedback_info.pPipelineCreationFeedback = &feedback;
        feedback_info.pipelineStageCreationFeedbackCount = 1;
        feedback_info.pPipelineStageCreationFeedbacks = &stage_feedback;
        profiled_info = *info;
        profiled_info.pNext = &feedback_info;
        info = &profiled_info;
    }
    start_ns = vkd3d_get_current_time_ns();
    vr = VK_CALL(vkCreateComputePipelines(device->vk_device, cache, 1, info, NULL, pipeline));
    end_ns = vkd3d_get_current_time_ns();
    vkd3d_ps5_pso_record(start_ns, end_ns, vr, draw_time, disk_hit, state,
            "compute", info->flags, VK_SHADER_STAGE_COMPUTE_BIT, info->pNext);
    vkd3d_ps5_hitch_pipeline(start_ns, end_ns);
    return vr;
}

void vkd3d_ps5_gpu_census_present_latency(uint64_t present_ns, uint64_t queue_present_ns)
{
    if (!present_ns || queue_present_ns < present_ns) return;
    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    vkd3d_ps5_gpu_census_init_locked(queue_present_ns);
    wineps5_present_latency_note(&vkd3d_ps5_present_latency,queue_present_ns - present_ns);
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
}
void vkd3d_ps5_gpu_census_display_latency(uint64_t present_ns, uint64_t completion_ns)
{
    uint64_t interval_ns;
    if (!wineps5_display_latency_interval(present_ns,completion_ns,&interval_ns)) return;
    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    vkd3d_ps5_gpu_census_init_locked(completion_ns);
    wineps5_present_latency_note(&vkd3d_ps5_display_latency,interval_ns);
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
}
void vkd3d_ps5_gpu_census_fg_stage(unsigned int stage, uint64_t present_ns, uint64_t event_ns)
{
    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    vkd3d_ps5_gpu_census_init_locked(event_ns);
    wineps5_fg_stage_note(&vkd3d_ps5_fg_stages,stage,present_ns,event_ns);
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
}

static void vkd3d_ps5_hitch_init_once(void)
{
    char value[8];

    vkd3d_ps5_hitch_active = vkd3d_get_env_var("WINEPS5_PROFILE", value, sizeof(value)) &&
            !strcmp(value, "1");
    if (vkd3d_ps5_hitch_active)
        ERR("WINEPS5 hitch enabled threshold_ms=%.0f max_lines_per_10s=%u\n",
                1.0e-6 * (double)WINEPS5_HITCH_THRESHOLD_NS, WINEPS5_HITCH_MAX_LINES);
}

void vkd3d_ps5_hitch_initialize(void)
{
    pthread_once(&vkd3d_ps5_hitch_once, vkd3d_ps5_hitch_init_once);
}

/* Wine's per-thread block on registered x64 title threads, else NULL. */
static struct wineps5_hitch_wine *vkd3d_ps5_hitch_wine(void)
{
#if defined(_WIN64) && defined(__x86_64__)
    struct wineps5_hitch_wine *wine = (struct wineps5_hitch_wine *)
            ((char *)NtCurrentTeb() + WINEPS5_HITCH_TEB64_OFFSET);
    return wine->magic == WINEPS5_HITCH_MAGIC ? wine : NULL;
#else
    return NULL;
#endif
}

/* Own Wine totals; an owner is running, so it has no wait in progress. */
static void vkd3d_ps5_hitch_wine_own(const struct wineps5_hitch_wine *wine, uint64_t now_ns,
        struct wineps5_hitch_wine_totals *out)
{
    if (wine)
        wineps5_hitch_wine_effective(wine, now_ns, out);
    else
        memset(out, 0, sizeof(*out));
}

/* Another thread's Wine totals as of now_ns and its latest CPU sample; false
 * once that thread exited. Under the lock. */
static bool vkd3d_ps5_hitch_wine_remote(const struct wineps5_hitch_wine *wine, uint32_t tid,
        uint64_t now_ns, struct wineps5_hitch_wine_totals *out, struct wineps5_hitch_cpu_sample *cpu)
{
    struct wineps5_hitch_wine copy;
    uint32_t before, after;
    unsigned int i;

    memset(&copy, 0, sizeof(copy));
    for (i = 0; i < 8; i++)
    {
        before = __atomic_load_n(&wine->sequence, __ATOMIC_ACQUIRE);
        copy.wait_ns = wine->wait_ns; copy.wait_count = wine->wait_count;
        copy.read_ns = wine->read_ns; copy.read_count = wine->read_count;
        copy.unwind_ns = wine->unwind_ns; copy.unwind_count = wine->unwind_count;
        copy.wait_start_ns = wine->wait_start_ns;
        copy.cpu_ns = wine->cpu_ns; copy.cpu_sample_ns = wine->cpu_sample_ns;
        copy.cpu_core = wine->cpu_core; copy.cpu_samples = wine->cpu_samples;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = __atomic_load_n(&wine->sequence, __ATOMIC_RELAXED);
        if (before == after && !(before & 1))
            break;
    }
    if (i == 8)
        vkd3d_ps5_hitch_torn++;
    if (__atomic_load_n(&wine->tid, __ATOMIC_ACQUIRE) != tid)
        return false;
    wineps5_hitch_wine_effective(&copy, now_ns, out);
    wineps5_hitch_cpu_read(&copy, cpu);
    if (cpu->sample_ns && cpu->core < 64)
        vkd3d_ps5_hitch_cores_seen |= UINT64_C(1) << cpu->core;
    return true;
}

/* A slot's running totals as of now_ns, including its open vkd3d call and
 * any wait in progress, plus its latest CPU sample; false once the owner
 * exited. Under the lock. */
static bool vkd3d_ps5_hitch_slot_snapshot(struct vkd3d_ps5_hitch_slot *s, uint64_t now_ns,
        struct wineps5_hitch_snapshot *out, struct wineps5_hitch_cpu_sample *cpu)
{
    struct wineps5_hitch_wine_totals wine;
    struct wineps5_hitch_scope_state scope;
    struct wineps5_hitch_thread totals;
    uint32_t before, after;
    unsigned int i;

    for (i = 0; i < 8; i++)
    {
        before = __atomic_load_n(&s->sequence, __ATOMIC_ACQUIRE);
        totals = s->totals;
        scope = s->scope;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = __atomic_load_n(&s->sequence, __ATOMIC_RELAXED);
        if (before == after && !(before & 1))
            break;
    }
    if (i == 8)
        vkd3d_ps5_hitch_torn++;
    if (s->wine)
    {
        if (!vkd3d_ps5_hitch_wine_remote(s->wine, s->tid, now_ns, &wine, cpu))
            return false;
    }
    else
    {
        memset(&wine, 0, sizeof(wine));
        memset(cpu, 0, sizeof(*cpu));
    }
    wineps5_hitch_effective(&totals, &scope, &wine, now_ns, out);
    return true;
}

/* This thread's slot, claimed on first use; NULL once the registry is full. */
static struct vkd3d_ps5_hitch_slot *vkd3d_ps5_hitch_self(void)
{
    struct vkd3d_ps5_hitch_tls *t = &vkd3d_ps5_hitch_tls;
    struct vkd3d_ps5_hitch_slot *slot = NULL;
    struct wineps5_hitch_wine *wine;
    unsigned int i;

    if (t->slot || t->unslotted)
        return t->slot;
    wine = vkd3d_ps5_hitch_wine();
    pthread_mutex_lock(&vkd3d_ps5_hitch_lock);
    for (i = 0; i < VKD3D_PS5_HITCH_SLOTS; i++)
    {
        if (vkd3d_ps5_hitch_slots[i].tid)
            continue;
        slot = &vkd3d_ps5_hitch_slots[i];
        memset(slot, 0, sizeof(*slot));
        slot->wine = wine;
        slot->tid = vkd3d_get_current_thread_id();
        break;
    }
    if (!slot)
        vkd3d_ps5_hitch_unslotted++;
    pthread_mutex_unlock(&vkd3d_ps5_hitch_lock);
    /* Start Wine's wait/read/stack-walk timing for this thread. */
    if (slot && wine)
        wine->armed = 1;
    t->slot = slot;
    t->unslotted = !slot;
    return slot;
}

void vkd3d_ps5_hitch_begin_slow(unsigned int bucket)
{
    struct vkd3d_ps5_hitch_tls *t = &vkd3d_ps5_hitch_tls;
    struct wineps5_hitch_wine_totals wine;
    struct vkd3d_ps5_hitch_slot *s;
    uint64_t now_ns;

    if (t->depth++ || !(s = vkd3d_ps5_hitch_self()))
        return;
    now_ns = vkd3d_get_current_time_ns();
    vkd3d_ps5_hitch_wine_own(s->wine, now_ns, &wine);
    vkd3d_ps5_hitch_slot_write_begin(s);
    wineps5_hitch_scope_open(&s->scope, bucket, now_ns, &wine);
    vkd3d_ps5_hitch_slot_write_end(s);
}

void vkd3d_ps5_hitch_end_slow(void)
{
    struct vkd3d_ps5_hitch_tls *t = &vkd3d_ps5_hitch_tls;
    struct wineps5_hitch_wine_totals wine;
    struct vkd3d_ps5_hitch_slot *s;
    uint64_t now_ns;

    if (!t->depth || --t->depth || !(s = t->slot))
        return;
    now_ns = vkd3d_get_current_time_ns();
    vkd3d_ps5_hitch_wine_own(s->wine, now_ns, &wine);
    vkd3d_ps5_hitch_slot_write_begin(s);
    wineps5_hitch_scope_close(&s->totals, &s->scope, now_ns, &wine);
    vkd3d_ps5_hitch_slot_write_end(s);
}

void vkd3d_ps5_hitch_vk_alloc_slow(uint64_t start_ns)
{
    uint64_t end_ns = vkd3d_get_current_time_ns();
    struct vkd3d_ps5_hitch_slot *s;

    if (!(s = vkd3d_ps5_hitch_tls.slot))
        return;
    vkd3d_ps5_hitch_slot_write_begin(s);
    s->totals.vk_alloc_ns += end_ns > start_ns ? end_ns - start_ns : 0;
    s->totals.vk_alloc_calls++;
    vkd3d_ps5_hitch_slot_write_end(s);
}

void vkd3d_ps5_hitch_vk_submit_slow(uint64_t submit_ns)
{
    uint64_t end_ns = vkd3d_get_current_time_ns();

    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    wineps5_hitch_gpu_submit(&vkd3d_ps5_hitch_gpu, end_ns > submit_ns ? end_ns - submit_ns : 0);
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
}

void vkd3d_ps5_hitch_copy_executed_slow(uint64_t bytes)
{
    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    vkd3d_ps5_hitch_gpu.copy_bytes += bytes;
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);
}

/* Main = the process initial thread (Wine's server_init_process_done thread,
 * which runs the game's entry point); every Wine block points at its block. */
static void vkd3d_ps5_hitch_find_main_locked(const struct vkd3d_ps5_hitch_slot *self)
{
    struct wineps5_hitch_wine *candidate;
    uint32_t tid;

    if (vkd3d_ps5_hitch_main.wine || !self || !self->wine)
        return;
    candidate = (struct wineps5_hitch_wine *)(uintptr_t)self->wine->main_block;
    if (!candidate || candidate->magic != WINEPS5_HITCH_MAGIC ||
            !(tid = __atomic_load_n(&candidate->tid, __ATOMIC_ACQUIRE)))
        return;
    vkd3d_ps5_hitch_main.wine = candidate;
    vkd3d_ps5_hitch_main.tid = tid;
    vkd3d_ps5_hitch_main.track.valid = false;
    candidate->armed = 1;
}

static void vkd3d_ps5_hitch_print_role(uint64_t seq, const struct vkd3d_ps5_hitch_role *r)
{
    const struct wineps5_hitch_thread *a = &r->delta.vkd3d;
    const struct wineps5_hitch_wine_totals *w = &r->delta.wine;
    struct wineps5_hitch_split split;
    struct wineps5_hitch_cpu cpu;
    char cpu_text[160];
    int core;

    wineps5_hitch_split(r->span_ns, &r->delta, &split);
    wineps5_hitch_cpu_interval(&r->b0, &r->b1, r->span_ns, &cpu);
    core = r->b1.valid && r->b1.core != WINEPS5_HITCH_NO_CORE ? (int)r->b1.core : -1;
    if (cpu.valid)
        snprintf(cpu_text, sizeof(cpu_text),
                "cpu_ms=%.3f cpu_err_ms=%.3f runnable_ms=%.3f cpu_samples=%"PRIu64" core=%d",
                1.0e-6 * (double)cpu.cpu_ns, 1.0e-6 * (double)cpu.err_ns,
                1.0e-6 * (double)wineps5_hitch_sub(split.busy_ns, cpu.cpu_ns), r->delta.wine.cpu_samples, core);
    else
        snprintf(cpu_text, sizeof(cpu_text), "cpu_ms=na cpu_err_ms=na runnable_ms=na cpu_samples=%"PRIu64" core=%d",
                r->delta.wine.cpu_samples, core);
    ERR("WINEPS5 hitch seq=%"PRIu64" role=%s thread=%04x span_ms=%.3f busy_ms=%.3f %s "
            "wait_ms=%.3f waits=%"PRIu64" present_wait_ms=%.3f vkd3d_wait_ms=%.3f "
            "vkd3d_resource_ms=%.3f vkd3d_heap_ms=%.3f vkd3d_desc_heap_ms=%.3f vkd3d_pso_ms=%.3f "
            "vkd3d_submit_ms=%.3f vkd3d_present_ms=%.3f vkd3d_other_ms=%.3f "
            "vkd3d_calls=%"PRIu64"/%"PRIu64"/%"PRIu64"/%"PRIu64"/%"PRIu64"/%"PRIu64"/%"PRIu64" "
            "vk_alloc_ms=%.3f vk_allocs=%"PRIu64" vk_pipeline_ms=%.3f vk_pipelines=%"PRIu64" "
            "read_ms=%.3f reads=%"PRIu64" unwind_ms=%.3f unwinds=%"PRIu64" "
            "other_ms=%.3f overlap_ms=%.3f\n",
            seq, r->name, r->tid, 1.0e-6 * (double)r->span_ns, 1.0e-6 * (double)split.busy_ns, cpu_text,
            1.0e-6 * (double)w->wait_ns, w->wait_count,
            1.0e-6 * (double)a->present_wait_ns, 1.0e-6 * (double)a->vkd3d_wait_ns,
            1.0e-6 * (double)a->bucket_ns[WINEPS5_HITCH_RESOURCE],
            1.0e-6 * (double)a->bucket_ns[WINEPS5_HITCH_HEAP],
            1.0e-6 * (double)a->bucket_ns[WINEPS5_HITCH_DESCRIPTOR_HEAP],
            1.0e-6 * (double)a->bucket_ns[WINEPS5_HITCH_PSO],
            1.0e-6 * (double)a->bucket_ns[WINEPS5_HITCH_SUBMIT],
            1.0e-6 * (double)a->bucket_ns[WINEPS5_HITCH_PRESENT],
            1.0e-6 * (double)a->bucket_ns[WINEPS5_HITCH_OTHER],
            a->bucket_calls[WINEPS5_HITCH_RESOURCE], a->bucket_calls[WINEPS5_HITCH_HEAP],
            a->bucket_calls[WINEPS5_HITCH_DESCRIPTOR_HEAP], a->bucket_calls[WINEPS5_HITCH_PSO],
            a->bucket_calls[WINEPS5_HITCH_SUBMIT], a->bucket_calls[WINEPS5_HITCH_PRESENT],
            a->bucket_calls[WINEPS5_HITCH_OTHER],
            1.0e-6 * (double)a->vk_alloc_ns, a->vk_alloc_calls,
            1.0e-6 * (double)a->pipeline_ns, a->pipeline_calls,
            1.0e-6 * (double)w->read_ns, w->read_count,
            1.0e-6 * (double)w->unwind_ns, w->unwind_count,
            1.0e-6 * (double)split.other_ns, 1.0e-6 * (double)split.overlap_ns);
}

static void vkd3d_ps5_hitch_print_event(const struct vkd3d_ps5_hitch_event *e)
{
    unsigned int i;

    ERR("WINEPS5 hitch seq=%"PRIu64" frame_ms=%.3f presenter=%04x main=%04x busiest=%04x threads=%u "
            "gpu_direct_max_ms=%.3f gpu_compute_max_ms=%.3f gpu_copy_max_ms=%.3f "
            "gpu_completed=%"PRIu64"/%"PRIu64"/%"PRIu64" "
            "vk_submit_ms=%.3f vk_submit_max_ms=%.3f vk_submits=%"PRIu64" copy_mb=%.3f\n",
            e->seq, 1.0e-6 * (double)e->frame_ns, e->presenter, e->main, e->busiest, e->live,
            1.0e-6 * (double)e->gpu.max_ns[0], 1.0e-6 * (double)e->gpu.max_ns[1],
            1.0e-6 * (double)e->gpu.max_ns[2],
            e->gpu.completed[0], e->gpu.completed[1], e->gpu.completed[2],
            1.0e-6 * (double)e->gpu.vk_submit_ns, 1.0e-6 * (double)e->gpu.vk_submit_max_ns, e->gpu.vk_submits,
            (double)e->gpu.copy_bytes / (1024.0 * 1024.0));
    for (i = 0; i < e->role_count; i++)
        vkd3d_ps5_hitch_print_role(e->seq, &e->roles[i]);
}

static void vkd3d_ps5_hitch_add_role(struct vkd3d_ps5_hitch_event *e, const char *name, uint32_t tid,
        int slot, uint64_t span_ns, const struct wineps5_hitch_snapshot *delta,
        const struct wineps5_hitch_cpu_bound *b0, const struct wineps5_hitch_cpu_bound *b1)
{
    struct vkd3d_ps5_hitch_role *r = &e->roles[e->role_count++];

    r->name = name;
    r->tid = tid;
    r->slot = slot;
    r->span_ns = span_ns;
    r->delta = *delta;
    r->b0 = *b0;
    r->b1 = *b1;
}

/* Frame boundary: the end of any Present on any thread (WoW presents from
 * rotating threads). Snapshots every attributed thread at each boundary. */
void vkd3d_ps5_hitch_present(uint64_t now_ns)
{
    struct wineps5_hitch_cpu_bound main_prev_bound = {0};
    struct wineps5_hitch_snapshot snapshot, main_delta;
    int present_index = -1, main_index = -1, busiest;
    struct vkd3d_ps5_hitch_event *pending, flush;
    struct vkd3d_ps5_hitch_slot *self, *s;
    struct wineps5_hitch_cpu_sample cpu;
    struct wineps5_hitch_wine_totals wine;
    struct wineps5_hitch_limiter closed;
    struct wineps5_hitch_split split;
    struct wineps5_hitch_gpu gpu;
    uint64_t frame_ns, main_span = 0, unslotted, torn, cores_seen = 0;
    bool log = false, roll = false, main_had = false, main_stepped = false;
    unsigned int i, live = 0;
    uint32_t main_tid;

    if (!vkd3d_ps5_hitch_active)
        return;
    self = vkd3d_ps5_hitch_self();
    pending = &vkd3d_ps5_hitch_pending;
    flush.active = false;

    pthread_mutex_lock(&vkd3d_ps5_hitch_lock);
    frame_ns = wineps5_hitch_clock_step(&vkd3d_ps5_hitch_clock, now_ns);
    vkd3d_ps5_hitch_find_main_locked(self);
    main_tid = vkd3d_ps5_hitch_main.tid;

    for (i = 0; i < VKD3D_PS5_HITCH_SLOTS; i++)
    {
        s = &vkd3d_ps5_hitch_slots[i];
        vkd3d_ps5_hitch_eligible[i] = vkd3d_ps5_hitch_stepped[i] = false;
        if (!s->tid)
            continue;
        if (!vkd3d_ps5_hitch_slot_snapshot(s, now_ns, &snapshot, &cpu))
        {
            /* The owner exited; its TEB block now belongs to nobody or a new thread. */
            memset(s, 0, sizeof(*s));
            continue;
        }
        live++;
        vkd3d_ps5_hitch_stepped[i] = true;
        if (s == self)
            present_index = i;
        if (main_tid && s->tid == main_tid)
            main_index = i;
        if (wineps5_hitch_track_step(&s->track, &snapshot, &cpu, now_ns,
                &vkd3d_ps5_hitch_deltas[i], &vkd3d_ps5_hitch_spans[i], &vkd3d_ps5_hitch_prev_bounds[i]))
        {
            wineps5_hitch_split(vkd3d_ps5_hitch_spans[i], &vkd3d_ps5_hitch_deltas[i], &split);
            vkd3d_ps5_hitch_busy[i] = split.busy_ns;
            vkd3d_ps5_hitch_eligible[i] = true;
        }
    }

    /* Main may never call vkd3d: then only its Wine totals are known. */
    if (main_tid && main_index < 0)
    {
        if (vkd3d_ps5_hitch_wine_remote(vkd3d_ps5_hitch_main.wine, main_tid, now_ns, &wine, &cpu))
        {
            memset(&snapshot, 0, sizeof(snapshot));
            snapshot.wine = wine;
            main_stepped = true;
            main_had = wineps5_hitch_track_step(&vkd3d_ps5_hitch_main.track, &snapshot, &cpu, now_ns,
                    &main_delta, &main_span, &main_prev_bound);
        }
        else
        {
            memset(&vkd3d_ps5_hitch_main, 0, sizeof(vkd3d_ps5_hitch_main));
            main_tid = 0;
        }
    }

    /* Finish the hitch recorded at the previous Present: samples read now
     * tighten the bound on each role thread's CPU time at that hitch's end. */
    if (pending->active)
    {
        flush = *pending;
        pending->active = false;
        for (i = 0; i < flush.role_count; i++)
        {
            struct vkd3d_ps5_hitch_role *r = &flush.roles[i];

            if (r->slot >= 0 && vkd3d_ps5_hitch_stepped[r->slot] && vkd3d_ps5_hitch_slots[r->slot].tid == r->tid)
                r->b1 = vkd3d_ps5_hitch_prev_bounds[r->slot];
            else if (r->slot < 0 && main_stepped && main_tid == r->tid)
                r->b1 = main_prev_bound;
        }
    }

    /* GPU work completed in this interval; the accumulator restarts here. */
    pthread_mutex_lock(&vkd3d_ps5_gpu_census_lock);
    gpu = vkd3d_ps5_hitch_gpu;
    memset(&vkd3d_ps5_hitch_gpu, 0, sizeof(vkd3d_ps5_hitch_gpu));
    pthread_mutex_unlock(&vkd3d_ps5_gpu_census_lock);

    if (frame_ns)
    {
        if ((roll = wineps5_hitch_window_roll(&vkd3d_ps5_hitch_limiter, now_ns, &closed)))
        {
            cores_seen = vkd3d_ps5_hitch_cores_seen;
            vkd3d_ps5_hitch_cores_seen = 0;
        }
        log = wineps5_hitch_admit(&vkd3d_ps5_hitch_limiter, frame_ns);
    }

    if (log)
    {
        memset(pending, 0, sizeof(*pending));
        pending->active = true;
        pending->seq = vkd3d_ps5_hitch_clock.seq;
        pending->frame_ns = frame_ns;
        pending->presenter = vkd3d_get_current_thread_id();
        pending->main = main_tid;
        pending->live = live;
        pending->gpu = gpu;
        if (present_index >= 0 && vkd3d_ps5_hitch_eligible[present_index])
            vkd3d_ps5_hitch_add_role(pending, present_index == main_index ? "present+main" : "present",
                    vkd3d_ps5_hitch_slots[present_index].tid, present_index,
                    vkd3d_ps5_hitch_spans[present_index], &vkd3d_ps5_hitch_deltas[present_index],
                    &vkd3d_ps5_hitch_prev_bounds[present_index],
                    &vkd3d_ps5_hitch_slots[present_index].track.bound);
        if (main_index >= 0 && main_index != present_index && vkd3d_ps5_hitch_eligible[main_index])
            vkd3d_ps5_hitch_add_role(pending, "main", main_tid, main_index,
                    vkd3d_ps5_hitch_spans[main_index], &vkd3d_ps5_hitch_deltas[main_index],
                    &vkd3d_ps5_hitch_prev_bounds[main_index], &vkd3d_ps5_hitch_slots[main_index].track.bound);
        else if (main_had)
            vkd3d_ps5_hitch_add_role(pending, "main", main_tid, -1, main_span, &main_delta,
                    &main_prev_bound, &vkd3d_ps5_hitch_main.track.bound);
        busiest = wineps5_hitch_pick_busiest(vkd3d_ps5_hitch_busy, vkd3d_ps5_hitch_eligible,
                VKD3D_PS5_HITCH_SLOTS, present_index, main_index);
        if (busiest >= 0)
        {
            pending->busiest = vkd3d_ps5_hitch_slots[busiest].tid;
            vkd3d_ps5_hitch_add_role(pending, "busiest", pending->busiest, busiest,
                    vkd3d_ps5_hitch_spans[busiest], &vkd3d_ps5_hitch_deltas[busiest],
                    &vkd3d_ps5_hitch_prev_bounds[busiest], &vkd3d_ps5_hitch_slots[busiest].track.bound);
        }
    }
    unslotted = vkd3d_ps5_hitch_unslotted;
    torn = vkd3d_ps5_hitch_torn;
    pthread_mutex_unlock(&vkd3d_ps5_hitch_lock);

    if (flush.active)
        vkd3d_ps5_hitch_print_event(&flush);
    if (roll && closed.hitches)
    {
        ERR("WINEPS5 hitch window_s=%.1f frames=%"PRIu64" hitches=%"PRIu64" logged=%"PRIu64
                " suppressed=%"PRIu64" max_frame_ms=%.3f threads=%u main=%04x unslotted=%"PRIu64
                " torn=%"PRIu64" cores_seen=0x%"PRIx64"\n",
                1.0e-9 * (double)(now_ns - closed.window_start_ns), closed.frames, closed.hitches,
                closed.logged, closed.suppressed, 1.0e-6 * (double)closed.max_frame_ns,
                live, main_tid, unslotted, torn, cores_seen);
    }
}
void vkd3d_hdr_pso_event(const char *format, ...)
{
    char line[VKD3D_HDR_TRACE_MAX_LINE_LENGTH];
    va_list args;
    int length;

    vkd3d_hdr_trace_initialize();
    if (!vkd3d_hdr_trace_is_enabled)
        return;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0)
        return;
    line[sizeof(line) - 1] = '\0';

    pthread_mutex_lock(&vkd3d_hdr_trace_lock);
    if (vkd3d_hdr_pso_trace_line_count < VKD3D_HDR_PSO_TRACE_MAX_LINES)
    {
        vkd3d_hdr_pso_trace_line_count++;
        ERR("WINEPS5_HDR_TRACE VKD3D pso_c2 %s\n", line);
    }
    else if (!vkd3d_hdr_pso_trace_limit_reported)
    {
        vkd3d_hdr_pso_trace_limit_reported = true;
        ERR("WINEPS5_HDR_TRACE VKD3D pso_c2_limit=%u\n",
                VKD3D_HDR_PSO_TRACE_MAX_LINES);
    }
    pthread_mutex_unlock(&vkd3d_hdr_trace_lock);
}


void vkd3d_hdr_dump_manifest(const char *format, ...)
{
    char line[VKD3D_HDR_MANIFEST_LINE_LENGTH];
    char session[VKD3D_PATH_MAX];
    char filename[VKD3D_PATH_MAX + 32];
    bool report_cap = false;
    va_list args;
    size_t size;
    FILE *file;
    int length;

    if (!vkd3d_hdr_trace_enabled() ||
            !vkd3d_get_env_var("VKD3D_HDR_DUMP_SESSION", session, sizeof(session)))
        return;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0)
        return;

    size = min((size_t)length, sizeof(line) - 2);
    if (!size || line[size - 1] != '\n')
        line[size++] = '\n';
    line[size] = '\0';

    pthread_mutex_lock(&vkd3d_hdr_manifest_lock);
    if (size > VKD3D_HDR_MANIFEST_CAP -
            min(vkd3d_hdr_manifest_bytes, (size_t)VKD3D_HDR_MANIFEST_CAP))
    {
        report_cap = !vkd3d_hdr_manifest_cap_reported;
        vkd3d_hdr_manifest_cap_reported = true;
        pthread_mutex_unlock(&vkd3d_hdr_manifest_lock);
        if (report_cap)
            vkd3d_hdr_trace("manifest_cap_hit=1 cap_bytes=%u", VKD3D_HDR_MANIFEST_CAP);
        return;
    }

    snprintf(filename, sizeof(filename), "%s/pipelines.tsv", session);
    if ((file = fopen(filename, "ab")))
    {
        if (fwrite(line, 1, size, file) == size)
            vkd3d_hdr_manifest_bytes += size;
        else
            ERR("Failed to append HDR pipeline manifest.\n");
        fclose(file);
    }
    else if (!vkd3d_hdr_manifest_open_reported)
    {
        vkd3d_hdr_manifest_open_reported = true;
        ERR("Failed to open HDR pipeline manifest %s.\n", filename);
    }
    pthread_mutex_unlock(&vkd3d_hdr_manifest_lock);
}

#define VKD3D_HDR_EVENT_ENTRY_COUNT 128
#define VKD3D_HDR_EVENT_LINE_LENGTH 768
#define VKD3D_HDR_EVENT_LOG_LIMIT 200

struct vkd3d_hdr_event_entry
{
    char line[VKD3D_HDR_EVENT_LINE_LENGTH];
    uint64_t start_ns;
    uint32_t count;
};

static pthread_mutex_t vkd3d_hdr_event_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vkd3d_hdr_event_entry vkd3d_hdr_event_entries[VKD3D_HDR_EVENT_ENTRY_COUNT];
static unsigned int vkd3d_hdr_event_entry_count;
static unsigned int vkd3d_hdr_event_log_count;
static bool vkd3d_hdr_event_entry_limit_reported;
static bool vkd3d_hdr_event_log_limit_reported;

static void vkd3d_hdr_event_emit(const char *line, uint32_t count)
{
    bool emit = false;
    bool limit = false;

    pthread_mutex_lock(&vkd3d_hdr_event_lock);
    if (vkd3d_hdr_event_log_count < VKD3D_HDR_EVENT_LOG_LIMIT - 1)
    {
        vkd3d_hdr_event_log_count++;
        emit = true;
    }
    else if (!vkd3d_hdr_event_log_limit_reported)
    {
        vkd3d_hdr_event_log_limit_reported = true;
        vkd3d_hdr_event_log_count++;
        limit = true;
    }
    pthread_mutex_unlock(&vkd3d_hdr_event_lock);

    if (emit)
    {
        if (count)
            ERR("WINEPS5_HDR_TRACE VKD3D event %s count_1s=%u\n", line, count);
        else
            ERR("WINEPS5_HDR_TRACE VKD3D event %s\n", line);
    }
    else if (limit)
    {
        ERR("WINEPS5_HDR_TRACE VKD3D event_limit=%u\n", VKD3D_HDR_EVENT_LOG_LIMIT);
    }
}

void vkd3d_hdr_counted_event(const char *format, ...)
{
    char line[VKD3D_HDR_EVENT_LINE_LENGTH];
    char emit_line[VKD3D_HDR_EVENT_LINE_LENGTH];
    struct vkd3d_hdr_event_entry *entry = NULL;
    bool report_entry_limit = false;
    bool emit_first = false;
    uint32_t emit_count = 0;
    uint64_t now;
    va_list args;
    unsigned int i;
    int length;

    if (!vkd3d_hdr_trace_enabled())
        return;

    va_start(args, format);
    length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0)
        return;
    line[sizeof(line) - 1] = '\0';
    now = vkd3d_get_current_time_ns();

    pthread_mutex_lock(&vkd3d_hdr_event_lock);
    for (i = 0; i < vkd3d_hdr_event_entry_count; i++)
    {
        if (!strcmp(vkd3d_hdr_event_entries[i].line, line))
        {
            entry = &vkd3d_hdr_event_entries[i];
            break;
        }
    }

    if (!entry && vkd3d_hdr_event_entry_count < VKD3D_HDR_EVENT_ENTRY_COUNT)
    {
        entry = &vkd3d_hdr_event_entries[vkd3d_hdr_event_entry_count++];
        strcpy(entry->line, line);
        entry->start_ns = now;
        emit_first = true;
    }
    else if (!entry && !vkd3d_hdr_event_entry_limit_reported)
    {
        vkd3d_hdr_event_entry_limit_reported = true;
        report_entry_limit = true;
    }

    if (entry)
    {
        entry->count++;
        if (now - entry->start_ns >= UINT64_C(1000000000))
        {
            emit_count = entry->count;
            entry->count = 0;
            entry->start_ns = now;
        }
        strcpy(emit_line, entry->line);
    }
    pthread_mutex_unlock(&vkd3d_hdr_event_lock);

    if (emit_first)
        vkd3d_hdr_event_emit(emit_line, 0);
    if (emit_count)
        vkd3d_hdr_event_emit(emit_line, emit_count);
    if (report_entry_limit)
        vkd3d_hdr_event_emit("combo_limit=128", 0);
}

#define COLOR         (VK_IMAGE_ASPECT_COLOR_BIT)
#define DEPTH         (VK_IMAGE_ASPECT_DEPTH_BIT)
#define STENCIL       (VK_IMAGE_ASPECT_STENCIL_BIT)
#define DEPTH_STENCIL (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
#define PLANAR        (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT)
#define TYPELESS      VKD3D_FORMAT_TYPE_TYPELESS
#define SINT          VKD3D_FORMAT_TYPE_SINT
#define UINT          VKD3D_FORMAT_TYPE_UINT

static const struct vkd3d_format_footprint nv12_copy_footprints[] =
{
    { DXGI_FORMAT_R8_TYPELESS, 1, 1, 1, 0, 0 },
    { DXGI_FORMAT_R8G8_TYPELESS, 1, 1, 2, 1, 1 },
};

static const struct vkd3d_format_footprint p016_copy_footprints[] =
{
    { DXGI_FORMAT_R16_TYPELESS, 1, 1, 2, 0, 0 },
    { DXGI_FORMAT_R16G16_TYPELESS, 1, 1, 4, 1, 1 },
};

static const struct vkd3d_format vkd3d_formats[] =
{
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, VK_FORMAT_R32G32B32A32_SFLOAT,      16, 1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R32G32B32A32_FLOAT,    VK_FORMAT_R32G32B32A32_SFLOAT,      16, 1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R32G32B32A32_UINT,     VK_FORMAT_R32G32B32A32_UINT,        16, 1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R32G32B32A32_SINT,     VK_FORMAT_R32G32B32A32_SINT,        16, 1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_R32G32B32_TYPELESS,    VK_FORMAT_R32G32B32_SFLOAT,         12, 1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R32G32B32_FLOAT,       VK_FORMAT_R32G32B32_SFLOAT,         12, 1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R32G32B32_UINT,        VK_FORMAT_R32G32B32_UINT,           12, 1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R32G32B32_SINT,        VK_FORMAT_R32G32B32_SINT,           12, 1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, VK_FORMAT_R16G16B16A16_SFLOAT,      8,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R16G16B16A16_FLOAT,    VK_FORMAT_R16G16B16A16_SFLOAT,      8,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R16G16B16A16_UNORM,    VK_FORMAT_R16G16B16A16_UNORM,       8,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R16G16B16A16_UINT,     VK_FORMAT_R16G16B16A16_UINT,        8,  1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R16G16B16A16_SNORM,    VK_FORMAT_R16G16B16A16_SNORM,       8,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R16G16B16A16_SINT,     VK_FORMAT_R16G16B16A16_SINT,        8,  1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_R32G32_TYPELESS,       VK_FORMAT_R32G32_SFLOAT,            8,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R32G32_FLOAT,          VK_FORMAT_R32G32_SFLOAT,            8,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R32G32_UINT,           VK_FORMAT_R32G32_UINT,              8,  1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R32G32_SINT,           VK_FORMAT_R32G32_SINT,              8,  1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_R10G10B10A2_TYPELESS,  VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R10G10B10A2_UNORM,     VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R10G10B10A2_UINT,      VK_FORMAT_A2B10G10R10_UINT_PACK32,  4,  1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R11G11B10_FLOAT,       VK_FORMAT_B10G11R11_UFLOAT_PACK32,  4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R8G8_TYPELESS,         VK_FORMAT_R8G8_UNORM,               2,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R8G8_UNORM,            VK_FORMAT_R8G8_UNORM,               2,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R8G8_UINT,             VK_FORMAT_R8G8_UINT,                2,  1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R8G8_SNORM,            VK_FORMAT_R8G8_SNORM,               2,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R8G8_SINT,             VK_FORMAT_R8G8_SINT,                2,  1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_R8G8B8A8_TYPELESS,     VK_FORMAT_R8G8B8A8_UNORM,           4,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R8G8B8A8_UNORM,        VK_FORMAT_R8G8B8A8_UNORM,           4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,   VK_FORMAT_R8G8B8A8_SRGB,            4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R8G8B8A8_UINT,         VK_FORMAT_R8G8B8A8_UINT,            4,  1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R8G8B8A8_SNORM,        VK_FORMAT_R8G8B8A8_SNORM,           4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R8G8B8A8_SINT,         VK_FORMAT_R8G8B8A8_SINT,            4,  1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_R16G16_TYPELESS,       VK_FORMAT_R16G16_SFLOAT,            4,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R16G16_FLOAT,          VK_FORMAT_R16G16_SFLOAT,            4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R16G16_UNORM,          VK_FORMAT_R16G16_UNORM,             4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R16G16_UINT,           VK_FORMAT_R16G16_UINT,              4,  1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R16G16_SNORM,          VK_FORMAT_R16G16_SNORM,             4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R16G16_SINT,           VK_FORMAT_R16G16_SINT,              4,  1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_R32_TYPELESS,          VK_FORMAT_R32_UINT,                 4,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_D32_FLOAT,             VK_FORMAT_D32_SFLOAT,               4,  1, 1,  1, DEPTH, 1},
    {DXGI_FORMAT_R32_FLOAT,             VK_FORMAT_R32_SFLOAT,               4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R32_UINT,              VK_FORMAT_R32_UINT,                 4,  1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R32_SINT,              VK_FORMAT_R32_SINT,                 4,  1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_R16_TYPELESS,          VK_FORMAT_R16_UINT,                 2,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R16_FLOAT,             VK_FORMAT_R16_SFLOAT,               2,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_D16_UNORM,             VK_FORMAT_D16_UNORM,                2,  1, 1,  1, DEPTH, 1},
    {DXGI_FORMAT_R16_UNORM,             VK_FORMAT_R16_UNORM,                2,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R16_UINT,              VK_FORMAT_R16_UINT,                 2,  1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R16_SNORM,             VK_FORMAT_R16_SNORM,                2,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R16_SINT,              VK_FORMAT_R16_SINT,                 2,  1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_R8_TYPELESS,           VK_FORMAT_R8_UNORM,                 1,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_R8_UNORM,              VK_FORMAT_R8_UNORM,                 1,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R8_UINT,               VK_FORMAT_R8_UINT,                  1,  1, 1,  1, COLOR, 1, UINT},
    {DXGI_FORMAT_R8_SNORM,              VK_FORMAT_R8_SNORM,                 1,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R8_SINT,               VK_FORMAT_R8_SINT,                  1,  1, 1,  1, COLOR, 1, SINT},
    {DXGI_FORMAT_A8_UNORM,              VK_FORMAT_A8_UNORM_KHR,             1,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_B8G8R8A8_UNORM,        VK_FORMAT_B8G8R8A8_UNORM,           4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_B8G8R8X8_UNORM,        VK_FORMAT_B8G8R8A8_UNORM,           4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS,     VK_FORMAT_B8G8R8A8_UNORM,           4,  1, 1,  1, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,   VK_FORMAT_B8G8R8A8_SRGB,            4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_B8G8R8X8_TYPELESS,     VK_FORMAT_B8G8R8A8_UNORM,           4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,   VK_FORMAT_B8G8R8A8_SRGB,            4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_R9G9B9E5_SHAREDEXP,    VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,   4,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_B5G6R5_UNORM,          VK_FORMAT_R5G6B5_UNORM_PACK16,      2,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_B5G5R5A1_UNORM,        VK_FORMAT_A1R5G5B5_UNORM_PACK16,    2,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_BC1_TYPELESS,          VK_FORMAT_BC1_RGBA_UNORM_BLOCK,     1,  4, 4,  8, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_BC1_UNORM,             VK_FORMAT_BC1_RGBA_UNORM_BLOCK,     1,  4, 4,  8, COLOR, 1},
    {DXGI_FORMAT_BC1_UNORM_SRGB,        VK_FORMAT_BC1_RGBA_SRGB_BLOCK,      1,  4, 4,  8, COLOR, 1},
    {DXGI_FORMAT_BC2_TYPELESS,          VK_FORMAT_BC2_UNORM_BLOCK,          1,  4, 4, 16, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_BC2_UNORM,             VK_FORMAT_BC2_UNORM_BLOCK,          1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_BC2_UNORM_SRGB,        VK_FORMAT_BC2_SRGB_BLOCK,           1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_BC3_TYPELESS,          VK_FORMAT_BC3_UNORM_BLOCK,          1,  4, 4, 16, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_BC3_UNORM,             VK_FORMAT_BC3_UNORM_BLOCK,          1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_BC3_UNORM_SRGB,        VK_FORMAT_BC3_SRGB_BLOCK,           1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_BC4_TYPELESS,          VK_FORMAT_BC4_UNORM_BLOCK,          1,  4, 4,  8, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_BC4_UNORM,             VK_FORMAT_BC4_UNORM_BLOCK,          1,  4, 4,  8, COLOR, 1},
    {DXGI_FORMAT_BC4_SNORM,             VK_FORMAT_BC4_SNORM_BLOCK,          1,  4, 4,  8, COLOR, 1},
    {DXGI_FORMAT_BC5_TYPELESS,          VK_FORMAT_BC5_UNORM_BLOCK,          1,  4, 4, 16, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_BC5_UNORM,             VK_FORMAT_BC5_UNORM_BLOCK,          1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_BC5_SNORM,             VK_FORMAT_BC5_SNORM_BLOCK,          1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_BC6H_TYPELESS,         VK_FORMAT_BC6H_UFLOAT_BLOCK,        1,  4, 4, 16, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_BC6H_UF16,             VK_FORMAT_BC6H_UFLOAT_BLOCK,        1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_BC6H_SF16,             VK_FORMAT_BC6H_SFLOAT_BLOCK,        1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_BC7_TYPELESS,          VK_FORMAT_BC7_UNORM_BLOCK,          1,  4, 4, 16, COLOR, 1, TYPELESS},
    {DXGI_FORMAT_BC7_UNORM,             VK_FORMAT_BC7_UNORM_BLOCK,          1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_BC7_UNORM_SRGB,        VK_FORMAT_BC7_SRGB_BLOCK,           1,  4, 4, 16, COLOR, 1},
    {DXGI_FORMAT_B4G4R4A4_UNORM,        VK_FORMAT_A4R4G4B4_UNORM_PACK16,    2,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_A4B4G4R4_UNORM,        VK_FORMAT_R4G4B4A4_UNORM_PACK16,    2,  1, 1,  1, COLOR, 1},

    {DXGI_FORMAT_NV12,                  VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 0,  1, 1,  1, PLANAR, 2, TYPELESS, false, nv12_copy_footprints},
    /* P010 and P016 are functionally equivalent, there is no way to interpret data as R10X6 in D3D. */
    {DXGI_FORMAT_P010,                  VK_FORMAT_G16_B16R16_2PLANE_420_UNORM, 0,  1, 1,  1, PLANAR, 2, TYPELESS, false, p016_copy_footprints},
    {DXGI_FORMAT_P016,                  VK_FORMAT_G16_B16R16_2PLANE_420_UNORM, 0,  1, 1,  1, PLANAR, 2, TYPELESS, false, p016_copy_footprints},

    {DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE, VK_FORMAT_R32G32_UINT, 8,  1, 1,  1, COLOR, 1},
    {DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE, VK_FORMAT_R32G32_UINT, 8,  1, 1,  1, COLOR, 1},
};

static const struct vkd3d_format_footprint depth_stencil_copy_footprints[] =
{
    { DXGI_FORMAT_R32_TYPELESS, 1, 1, 4, 0, 0 },
    { DXGI_FORMAT_R8_TYPELESS, 1, 1, 1, 0, 0 },
};

/* Each depth/stencil format is only compatible with itself in Vulkan. */
static const struct vkd3d_format vkd3d_depth_stencil_formats[] =
{
    {DXGI_FORMAT_R32G8X24_TYPELESS,        VK_FORMAT_D32_SFLOAT_S8_UINT, 8,  1, 1, 1, DEPTH_STENCIL, 2, TYPELESS, false, depth_stencil_copy_footprints},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT,     VK_FORMAT_D32_SFLOAT_S8_UINT, 8,  1, 1, 1, DEPTH_STENCIL, 2, 0, false, depth_stencil_copy_footprints},
    {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, VK_FORMAT_D32_SFLOAT_S8_UINT, 8,  1, 1, 1, DEPTH,         2},
    {DXGI_FORMAT_X32_TYPELESS_G8X24_UINT,  VK_FORMAT_D32_SFLOAT_S8_UINT, 8,  1, 1, 1, STENCIL,       2},
    {DXGI_FORMAT_R32_TYPELESS,             VK_FORMAT_D32_SFLOAT,         4,  1, 1, 1, DEPTH,         1, TYPELESS},
    {DXGI_FORMAT_R32_FLOAT,                VK_FORMAT_D32_SFLOAT,         4,  1, 1, 1, DEPTH,         1},
    {DXGI_FORMAT_R24G8_TYPELESS,           VK_FORMAT_D24_UNORM_S8_UINT,  4,  1, 1, 1, DEPTH_STENCIL, 2, TYPELESS, false, depth_stencil_copy_footprints},
    {DXGI_FORMAT_D24_UNORM_S8_UINT,        VK_FORMAT_D24_UNORM_S8_UINT,  4,  1, 1, 1, DEPTH_STENCIL, 2, 0, false, depth_stencil_copy_footprints},
    {DXGI_FORMAT_R24_UNORM_X8_TYPELESS,    VK_FORMAT_D24_UNORM_S8_UINT,  4,  1, 1, 1, DEPTH,         2},
    {DXGI_FORMAT_X24_TYPELESS_G8_UINT,     VK_FORMAT_D24_UNORM_S8_UINT,  4,  1, 1, 1, STENCIL,       2},
    {DXGI_FORMAT_R16_TYPELESS,             VK_FORMAT_D16_UNORM,          2,  1, 1, 1, DEPTH,         1, TYPELESS},
    {DXGI_FORMAT_R16_UNORM,                VK_FORMAT_D16_UNORM,          2,  1, 1, 1, DEPTH,         1},
};
#undef COLOR
#undef DEPTH
#undef STENCIL
#undef DEPTH_STENCIL
#undef TYPELESS
#undef SINT
#undef UINT

static const struct dxgi_format_compatibility_list
{
    DXGI_FORMAT image_format;
    DXGI_FORMAT view_formats[VKD3D_MAX_COMPATIBLE_FORMAT_COUNT];
    DXGI_FORMAT uint_format; /* for ClearUAVUint */
    DXGI_FORMAT typeless_format;
}
dxgi_format_compatibility_list[] =
{
    {DXGI_FORMAT_R32G32B32A32_TYPELESS,
            {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_SINT},
            DXGI_FORMAT_R32G32B32A32_UINT,
            DXGI_FORMAT_R32G32B32A32_TYPELESS},
    {DXGI_FORMAT_R32G32B32A32_FLOAT, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_R32G32B32A32_UINT,
            DXGI_FORMAT_R32G32B32A32_TYPELESS},
    {DXGI_FORMAT_R32G32B32A32_UINT,
            {DXGI_FORMAT_R32G32B32A32_SINT},
            DXGI_FORMAT_R32G32B32A32_UINT,
            DXGI_FORMAT_R32G32B32A32_TYPELESS},
    {DXGI_FORMAT_R32G32B32A32_SINT,
            {DXGI_FORMAT_R32G32B32A32_UINT},
            DXGI_FORMAT_R32G32B32A32_UINT,
            DXGI_FORMAT_R32G32B32A32_TYPELESS},

    {DXGI_FORMAT_R32G32B32_TYPELESS,
            {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32_SINT},
            DXGI_FORMAT_R32G32B32_UINT,
            DXGI_FORMAT_R32G32B32_TYPELESS},
    {DXGI_FORMAT_R32G32B32_FLOAT, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_R32G32B32_UINT,
            DXGI_FORMAT_R32G32B32_TYPELESS},
    {DXGI_FORMAT_R32G32B32_UINT,
            {DXGI_FORMAT_R32G32B32_SINT},
            DXGI_FORMAT_R32G32B32_UINT,
            DXGI_FORMAT_R32G32B32_TYPELESS},
    {DXGI_FORMAT_R32G32B32_SINT,
            {DXGI_FORMAT_R32G32B32_UINT},
            DXGI_FORMAT_R32G32B32_UINT,
            DXGI_FORMAT_R32G32B32_TYPELESS},

    {DXGI_FORMAT_R16G16B16A16_TYPELESS,
            {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT},
            DXGI_FORMAT_R16G16B16A16_UINT,
            DXGI_FORMAT_R16G16B16A16_TYPELESS},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_R16G16B16A16_UINT,
            DXGI_FORMAT_R16G16B16A16_TYPELESS},
    {DXGI_FORMAT_R16G16B16A16_UINT,
            {DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM},
            DXGI_FORMAT_R16G16B16A16_UINT,
            DXGI_FORMAT_R16G16B16A16_TYPELESS},
    {DXGI_FORMAT_R16G16B16A16_SINT,
            {DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM},
            DXGI_FORMAT_R16G16B16A16_UINT,
            DXGI_FORMAT_R16G16B16A16_TYPELESS},
    {DXGI_FORMAT_R16G16B16A16_UNORM,
            {DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT},
            DXGI_FORMAT_R16G16B16A16_UINT,
            DXGI_FORMAT_R16G16B16A16_TYPELESS},
    {DXGI_FORMAT_R16G16B16A16_SNORM,
            {DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT},
            DXGI_FORMAT_R16G16B16A16_UINT,
            DXGI_FORMAT_R16G16B16A16_TYPELESS},

    {DXGI_FORMAT_R32G32_TYPELESS,
            {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_SINT},
            DXGI_FORMAT_R32G32_UINT,
            DXGI_FORMAT_R32G32_TYPELESS},
    {DXGI_FORMAT_R32G32_FLOAT, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_R32G32_UINT,
            DXGI_FORMAT_R32G32_TYPELESS},
    {DXGI_FORMAT_R32G32_UINT,
            {DXGI_FORMAT_R32G32_SINT},
            DXGI_FORMAT_R32G32_UINT,
            DXGI_FORMAT_R32G32_TYPELESS},
    {DXGI_FORMAT_R32G32_SINT,
            {DXGI_FORMAT_R32G32_UINT},
            DXGI_FORMAT_R32G32_UINT,
            DXGI_FORMAT_R32G32_TYPELESS},

    {DXGI_FORMAT_R32G8X24_TYPELESS,
            {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R32G8X24_TYPELESS},
    {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
            {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R32G8X24_TYPELESS},
    {DXGI_FORMAT_X32_TYPELESS_G8X24_UINT,
            {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R32G8X24_TYPELESS},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
            {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R32G8X24_TYPELESS},

    {DXGI_FORMAT_R10G10B10A2_TYPELESS,
            {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT},
            DXGI_FORMAT_R10G10B10A2_UINT,
            DXGI_FORMAT_R10G10B10A2_TYPELESS},
    {DXGI_FORMAT_R10G10B10A2_UINT,
            {DXGI_FORMAT_R10G10B10A2_UNORM},
            DXGI_FORMAT_R10G10B10A2_UINT,
            DXGI_FORMAT_R10G10B10A2_TYPELESS},
    {DXGI_FORMAT_R10G10B10A2_UNORM,
            {DXGI_FORMAT_R10G10B10A2_UINT},
            DXGI_FORMAT_R10G10B10A2_UINT,
            DXGI_FORMAT_R10G10B10A2_TYPELESS},

    {DXGI_FORMAT_R11G11B10_FLOAT, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_R32_UINT},

    {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_R32_UINT},

    {DXGI_FORMAT_R8G8_TYPELESS,
            {DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM},
            DXGI_FORMAT_R8G8_UINT,
            DXGI_FORMAT_R8G8_TYPELESS},
    {DXGI_FORMAT_R8G8_UINT,
            {DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM},
            DXGI_FORMAT_R8G8_UINT,
            DXGI_FORMAT_R8G8_TYPELESS},
    {DXGI_FORMAT_R8G8_SINT,
            {DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM},
            DXGI_FORMAT_R8G8_UINT,
            DXGI_FORMAT_R8G8_TYPELESS},
    {DXGI_FORMAT_R8G8_UNORM,
            {DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SINT},
            DXGI_FORMAT_R8G8_UINT,
            DXGI_FORMAT_R8G8_TYPELESS},
    {DXGI_FORMAT_R8G8_SNORM,
            {DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SINT},
            DXGI_FORMAT_R8G8_UINT,
            DXGI_FORMAT_R8G8_TYPELESS},

    {DXGI_FORMAT_R8G8B8A8_TYPELESS,
            {DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_SNORM},
            DXGI_FORMAT_R8G8B8A8_UINT,
            DXGI_FORMAT_R8G8B8A8_TYPELESS},
    {DXGI_FORMAT_R8G8B8A8_UINT,
            {DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_SNORM},
            DXGI_FORMAT_R8G8B8A8_UINT,
            DXGI_FORMAT_R8G8B8A8_TYPELESS},
    {DXGI_FORMAT_R8G8B8A8_SINT,
            {DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_SNORM},
            DXGI_FORMAT_R8G8B8A8_UINT,
            DXGI_FORMAT_R8G8B8A8_TYPELESS},
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            {DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R8G8B8A8_UNORM},
            DXGI_FORMAT_R8G8B8A8_UINT,
            DXGI_FORMAT_R8G8B8A8_TYPELESS},
    {DXGI_FORMAT_R8G8B8A8_UNORM,
            {DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
            DXGI_FORMAT_R8G8B8A8_UINT,
            DXGI_FORMAT_R8G8B8A8_TYPELESS},
    {DXGI_FORMAT_R8G8B8A8_SNORM,
            {DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT},
            DXGI_FORMAT_R8G8B8A8_UINT,
            DXGI_FORMAT_R8G8B8A8_TYPELESS},

    {DXGI_FORMAT_R16G16_TYPELESS,
            {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM},
            DXGI_FORMAT_R16G16_UINT,
            DXGI_FORMAT_R16G16_TYPELESS},
    {DXGI_FORMAT_R16G16_FLOAT, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_R16G16_UINT,
            DXGI_FORMAT_R16G16_TYPELESS},
    {DXGI_FORMAT_R16G16_UINT,
            {DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM},
            DXGI_FORMAT_R16G16_UINT,
            DXGI_FORMAT_R16G16_TYPELESS},
    {DXGI_FORMAT_R16G16_SINT,
            {DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM},
            DXGI_FORMAT_R16G16_UINT,
            DXGI_FORMAT_R16G16_TYPELESS},
    {DXGI_FORMAT_R16G16_UNORM,
            {DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT},
            DXGI_FORMAT_R16G16_UINT,
            DXGI_FORMAT_R16G16_TYPELESS},
    {DXGI_FORMAT_R16G16_SNORM,
            {DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SINT},
            DXGI_FORMAT_R16G16_UINT,
            DXGI_FORMAT_R16G16_TYPELESS},

    {DXGI_FORMAT_R32_TYPELESS,
            {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT},
            DXGI_FORMAT_R32_UINT,
            DXGI_FORMAT_R32_TYPELESS},
    {DXGI_FORMAT_R32_FLOAT, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_R32_UINT,
            DXGI_FORMAT_R32_TYPELESS},
    {DXGI_FORMAT_R32_UINT,
            {DXGI_FORMAT_R32_SINT},
            DXGI_FORMAT_R32_UINT,
            DXGI_FORMAT_R32_TYPELESS},
    {DXGI_FORMAT_R32_SINT,
            {DXGI_FORMAT_R32_UINT},
            DXGI_FORMAT_R32_UINT,
            DXGI_FORMAT_R32_TYPELESS},

    {DXGI_FORMAT_D32_FLOAT, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R32_TYPELESS},

    {DXGI_FORMAT_R24G8_TYPELESS,
            {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R24G8_TYPELESS},
    {DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
            {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R24G8_TYPELESS},
    {DXGI_FORMAT_X24_TYPELESS_G8_UINT,
            {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R24G8_TYPELESS},
    {DXGI_FORMAT_D24_UNORM_S8_UINT,
            {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R24G8_TYPELESS},

    {DXGI_FORMAT_R16_TYPELESS,
            {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM},
            DXGI_FORMAT_R16_UINT,
            DXGI_FORMAT_R16_TYPELESS},
    {DXGI_FORMAT_R16_FLOAT, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_R16_UINT,
            DXGI_FORMAT_R16_TYPELESS},
    {DXGI_FORMAT_R16_UINT,
            {DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM},
            DXGI_FORMAT_R16_UINT,
            DXGI_FORMAT_R16_TYPELESS},
    {DXGI_FORMAT_R16_SINT,
            {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM},
            DXGI_FORMAT_R16_UINT,
            DXGI_FORMAT_R16_TYPELESS},
    {DXGI_FORMAT_R16_UNORM,
            {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT},
            DXGI_FORMAT_R16_UINT,
            DXGI_FORMAT_R16_TYPELESS},
    {DXGI_FORMAT_R16_SNORM,
            {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT},
            DXGI_FORMAT_R16_UINT,
            DXGI_FORMAT_R16_TYPELESS},

    {DXGI_FORMAT_D16_UNORM, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_R16_TYPELESS},

    {DXGI_FORMAT_R8_TYPELESS,
            {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM},
            DXGI_FORMAT_R8_UINT,
            DXGI_FORMAT_R8_TYPELESS},
    {DXGI_FORMAT_R8_UINT,
            {DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM},
            DXGI_FORMAT_R8_UINT,
            DXGI_FORMAT_R8_TYPELESS},
    {DXGI_FORMAT_R8_SINT,
            {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM},
            DXGI_FORMAT_R8_UINT,
            DXGI_FORMAT_R8_TYPELESS},
    {DXGI_FORMAT_R8_UNORM,
            {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT},
            DXGI_FORMAT_R8_UINT,
            DXGI_FORMAT_R8_TYPELESS},
    {DXGI_FORMAT_R8_SNORM,
            {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT},
            DXGI_FORMAT_R8_UINT,
            DXGI_FORMAT_R8_TYPELESS},

    /* A8 has no corresponding UINT format */
    {DXGI_FORMAT_A8_UNORM, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_A8_UNORM},

    /* There's no BGRA8_UINT either, so use UNORM directly */
    {DXGI_FORMAT_B8G8R8A8_TYPELESS,
            {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_TYPELESS},
    {DXGI_FORMAT_B8G8R8A8_UNORM,
            {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_TYPELESS},
    {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            {DXGI_FORMAT_B8G8R8A8_UNORM},
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_TYPELESS},

    {DXGI_FORMAT_B8G8R8X8_TYPELESS,
            {DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB},
            DXGI_FORMAT_B8G8R8X8_UNORM,
            DXGI_FORMAT_B8G8R8X8_TYPELESS},
    {DXGI_FORMAT_B8G8R8X8_UNORM,
            {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB},
            DXGI_FORMAT_B8G8R8X8_UNORM,
            DXGI_FORMAT_B8G8R8X8_TYPELESS},
    {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,
            {DXGI_FORMAT_B8G8R8X8_UNORM},
            DXGI_FORMAT_B8G8R8X8_UNORM,
            DXGI_FORMAT_B8G8R8X8_TYPELESS},

    {DXGI_FORMAT_BC1_TYPELESS,
            {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC1_TYPELESS},
    {DXGI_FORMAT_BC1_UNORM,
            {DXGI_FORMAT_BC1_UNORM_SRGB},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC1_TYPELESS},
    {DXGI_FORMAT_BC1_UNORM_SRGB,
            {DXGI_FORMAT_BC1_UNORM},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC1_TYPELESS},

    {DXGI_FORMAT_BC2_TYPELESS,
            {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC2_TYPELESS},
    {DXGI_FORMAT_BC2_UNORM,
            {DXGI_FORMAT_BC2_UNORM_SRGB},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC2_TYPELESS},
    {DXGI_FORMAT_BC2_UNORM_SRGB,
            {DXGI_FORMAT_BC2_UNORM},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC2_TYPELESS},

    {DXGI_FORMAT_BC3_TYPELESS,
            {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC3_TYPELESS},
    {DXGI_FORMAT_BC3_UNORM,
            {DXGI_FORMAT_BC3_UNORM_SRGB},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC3_TYPELESS},
    {DXGI_FORMAT_BC3_UNORM_SRGB,
            {DXGI_FORMAT_BC3_UNORM},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC3_TYPELESS},

    {DXGI_FORMAT_BC4_TYPELESS,
            {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_SNORM},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC4_TYPELESS},
    {DXGI_FORMAT_BC4_UNORM, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC4_TYPELESS},
    {DXGI_FORMAT_BC4_SNORM, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC4_TYPELESS},

    {DXGI_FORMAT_BC5_TYPELESS,
            {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_SNORM},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC5_TYPELESS},
    {DXGI_FORMAT_BC5_UNORM, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC5_TYPELESS},
    {DXGI_FORMAT_BC5_SNORM, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC5_TYPELESS},

    {DXGI_FORMAT_BC6H_TYPELESS,
            {DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_SF16},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC6H_TYPELESS},
    {DXGI_FORMAT_BC6H_UF16, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC6H_TYPELESS},
    {DXGI_FORMAT_BC6H_SF16, {DXGI_FORMAT_UNKNOWN},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC6H_TYPELESS},

    {DXGI_FORMAT_BC7_TYPELESS,
            {DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC7_TYPELESS},
    {DXGI_FORMAT_BC7_UNORM,
            {DXGI_FORMAT_BC7_UNORM_SRGB},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC7_TYPELESS},
    {DXGI_FORMAT_BC7_UNORM_SRGB,
            {DXGI_FORMAT_BC7_UNORM},
            DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_BC7_TYPELESS},

    {DXGI_FORMAT_NV12,
            {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UINT}},
    {DXGI_FORMAT_P010,
            {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_UINT}},
    {DXGI_FORMAT_P016,
            {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_UINT}},

    /* Internal implementation detail. We desire 64-bit atomics and R32G32 UAV will trigger that compat
     * similar to other 64-bit images. */
    {DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE,
            {DXGI_FORMAT_R32G32_UINT}},
    {DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE,
            {DXGI_FORMAT_R32G32_UINT}},
};

void vkd3d_format_compatibility_list_add_format(struct vkd3d_format_compatibility_list *list, VkFormat vk_format)
{
    unsigned int i;
    bool found = false;

    for (i = 0; i < list->format_count && !found; i++)
        found = list->vk_formats[i] == vk_format;

    if (!found)
    {
        if (list->format_count < ARRAY_SIZE(list->vk_formats))
            list->vk_formats[list->format_count++] = vk_format;
        else
            WARN("Format compatiblity list overflowed.\n");
    }
}

static HRESULT vkd3d_init_format_compatibility_lists(struct d3d12_device *device)
{
    struct vkd3d_format_compatibility_list *lists, *dst;
    const struct dxgi_format_compatibility_list *src;
    unsigned int count;
    unsigned int i, j;

    device->format_compatibility_list_count = 0;
    device->format_compatibility_lists = NULL;

    count = 0;
    for (i = 0; i < ARRAY_SIZE(dxgi_format_compatibility_list); ++i)
        count = max(count, dxgi_format_compatibility_list[i].image_format + 1);

    if (!(lists = vkd3d_calloc(count, sizeof(*lists))))
        return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE(dxgi_format_compatibility_list); ++i)
    {
        src = &dxgi_format_compatibility_list[i];
        dst = &lists[src->image_format];

        dst->uint_format = src->uint_format;
        dst->typeless_format = src->typeless_format;

        vkd3d_format_compatibility_list_add_format(dst, vkd3d_get_vk_format(src->image_format));

        for (j = 0; j < ARRAY_SIZE(src->view_formats) && src->view_formats[j]; j++)
            vkd3d_format_compatibility_list_add_format(dst, vkd3d_get_vk_format(src->view_formats[j]));
    }

    device->format_compatibility_list_count = count;
    device->format_compatibility_lists = lists;
    return S_OK;
}

static void vkd3d_cleanup_format_compatibility_lists(struct d3d12_device *device)
{
    vkd3d_free((void *)device->format_compatibility_lists);

    device->format_compatibility_lists = NULL;
    device->format_compatibility_list_count = 0;
}

static void vkd3d_get_vk_format_properties(struct d3d12_device *device, VkFormat vk_format, VkFormatProperties3 *properties3)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkFormatProperties2 properties;

    memset(properties3, 0, sizeof(*properties3));
    properties3->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;

    memset(&properties, 0, sizeof(properties));
    properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    properties.pNext = properties3;

    VK_CALL(vkGetPhysicalDeviceFormatProperties2(device->vk_physical_device, vk_format, &properties));
}

static void vkd3d_init_format_sample_counts(struct d3d12_device *device, struct vkd3d_format *format)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkPhysicalDeviceSparseImageFormatInfo2 sparse_info;
    VkPhysicalDeviceImageFormatInfo2 info;
    VkSampleCountFlagBits sample_count;
    VkImageFormatProperties2 props;
    uint32_t info_count;

    format->supported_sample_counts = 0;
    format->supported_sparse_sample_counts = 0;

    memset(&props, 0, sizeof(props));
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    info.format = format->vk_format;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = format->vk_image_tiling;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (VK_CALL(vkGetPhysicalDeviceImageFormatProperties2(device->vk_physical_device, &info, &props)))
        return;

    format->supported_sample_counts = props.imageFormatProperties.sampleCounts;

    /* Planar and depth-stencil formats do not support sparse in D3D12 */
    if (format->plane_count > 1 || !device->device_info.features2.features.sparseResidencyImage2D)
        return;

    info.flags |= VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
            VK_IMAGE_CREATE_SPARSE_ALIASED_BIT |
            VK_IMAGE_CREATE_SPARSE_BINDING_BIT;

    if (VK_CALL(vkGetPhysicalDeviceImageFormatProperties2(device->vk_physical_device, &info, &props)))
        return;

    while (props.imageFormatProperties.sampleCounts)
    {
        /* VUID 01094. Samples must be marked as supported by ImageFormatProperties. */
        sample_count = 1u << vkd3d_bitmask_iter32(&props.imageFormatProperties.sampleCounts);

        memset(&sparse_info, 0, sizeof(sparse_info));
        sparse_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2;
        sparse_info.format = info.format;
        sparse_info.type = info.type;
        sparse_info.samples = sample_count;
        sparse_info.usage = info.usage;
        sparse_info.tiling = info.tiling;

        VK_CALL(vkGetPhysicalDeviceSparseImageFormatProperties2(
                device->vk_physical_device, &sparse_info, &info_count, NULL));

        if (info_count > 0)
            format->supported_sparse_sample_counts |= sample_count;
    }
}

static HRESULT vkd3d_init_depth_stencil_formats(struct d3d12_device *device)
{
    struct vkd3d_format *formats, *format;
    VkFormatProperties3 d24s8_properties;
    VkFormatProperties3 properties;
    unsigned int i;

    if (!(formats = vkd3d_calloc(VKD3D_MAX_DXGI_FORMAT + 1, sizeof(*formats))))
        return E_OUTOFMEMORY;

    vkd3d_get_vk_format_properties(device, VK_FORMAT_D24_UNORM_S8_UINT, &d24s8_properties);

    if (!(d24s8_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
    {
        /* AMD doesn't support VK_FORMAT_D24_UNORM_S8_UINT. */
        WARN("Mapping VK_FORMAT_D24_UNORM_S8_UINT to VK_FORMAT_D32_SFLOAT_S8_UINT.\n");
    }

    for (i = 0; i < ARRAY_SIZE(vkd3d_depth_stencil_formats); ++i)
    {
        assert(vkd3d_depth_stencil_formats[i].dxgi_format <= VKD3D_MAX_DXGI_FORMAT);
        format = &formats[vkd3d_depth_stencil_formats[i].dxgi_format];
        *format = vkd3d_depth_stencil_formats[i];
        if (format->vk_format == VK_FORMAT_D24_UNORM_S8_UINT &&
            !(d24s8_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        {
            format->vk_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
            format->is_emulated = true;
        }

        vkd3d_get_vk_format_properties(device, format->vk_format, &properties);

        /* We cannot cast depth stencil formats in Vulkan, so features == castable. */
        format->vk_image_tiling = VK_IMAGE_TILING_OPTIMAL;
        format->vk_format_features = properties.optimalTilingFeatures;
        format->vk_format_features_castable = properties.optimalTilingFeatures;
        format->vk_format_features_buffer = 0;

        vkd3d_init_format_sample_counts(device, format);
    }

    device->depth_stencil_formats = formats;

    return S_OK;
}

static void vkd3d_cleanup_depth_stencil_formats(struct d3d12_device *device)
{
    vkd3d_free((void *)device->depth_stencil_formats);

    device->depth_stencil_formats = NULL;
}

static HRESULT vkd3d_init_formats(struct d3d12_device *device)
{
    const struct vkd3d_format_compatibility_list *list;
    struct vkd3d_format *formats, *format;
    VkFormatProperties3 properties;
    DXGI_FORMAT dxgi_format;
    unsigned int i, j;
    bool emulate_a8;

    static const VkFormatFeatureFlags2 feature_mask =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

    if (!(formats = vkd3d_calloc(VKD3D_MAX_DXGI_FORMAT + 1, sizeof(*formats))))
        return E_OUTOFMEMORY;

    if (device->device_info.maintenance_5_features.maintenance5)
    {
        const VkFormatFeatureFlags a8_required_features =
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
                VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;

        VkFormatProperties3 a8_properties, r8_properties;
        VkFormatFeatureFlags r8_unique_features;

        vkd3d_get_vk_format_properties(device, VK_FORMAT_A8_UNORM_KHR, &a8_properties);
        vkd3d_get_vk_format_properties(device, VK_FORMAT_R8_UNORM, &r8_properties);

        /* Only use the native A8_UNORM format if support is at least as good
         * as for the R8_UNORM fallback format for relevant features. */
        r8_unique_features = (r8_properties.optimalTilingFeatures | r8_properties.bufferFeatures) &
                ~(a8_properties.optimalTilingFeatures | a8_properties.bufferFeatures);

        emulate_a8 = (r8_unique_features & a8_required_features) != 0;
    }
    else
        emulate_a8 = true;

    if (emulate_a8)
        WARN("Mapping VK_FORMAT_A8_UNORM_KHR to VK_FORMAT_R8_UNORM.\n");

    for (i = 0; i < ARRAY_SIZE(vkd3d_formats); ++i)
    {
        assert(vkd3d_formats[i].dxgi_format <= VKD3D_MAX_DXGI_FORMAT);
        dxgi_format = vkd3d_formats[i].dxgi_format;
        format = &formats[dxgi_format];
        *format = vkd3d_formats[i];

        if (format->vk_format == VK_FORMAT_A8_UNORM_KHR && emulate_a8)
        {
            format->vk_format = VK_FORMAT_R8_UNORM;
            format->is_emulated = true;
        }

        vkd3d_get_vk_format_properties(device, format->vk_format, &properties);

        if ((properties.optimalTilingFeatures & feature_mask) || !(properties.linearTilingFeatures & feature_mask))
        {
            format->vk_image_tiling = VK_IMAGE_TILING_OPTIMAL;
            format->vk_format_features = properties.optimalTilingFeatures;
            format->vk_format_features_castable = properties.optimalTilingFeatures;
        }
        else
        {
            format->vk_image_tiling = VK_IMAGE_TILING_LINEAR;
            format->vk_format_features = properties.linearTilingFeatures;
            format->vk_format_features_castable = properties.linearTilingFeatures;
        }

        format->vk_format_features_buffer = properties.bufferFeatures;

        if (dxgi_format < device->format_compatibility_list_count)
        {
            list = &device->format_compatibility_lists[dxgi_format];
            for (j = 0; j < list->format_count; j++)
            {
                vkd3d_get_vk_format_properties(device, list->vk_formats[j], &properties);
                format->vk_format_features_castable |= format->vk_image_tiling == VK_IMAGE_TILING_OPTIMAL
                        ? properties.optimalTilingFeatures
                        : properties.linearTilingFeatures;
            }
        }

        vkd3d_init_format_sample_counts(device, format);
    }

    device->formats = formats;

    return S_OK;
}

static void vkd3d_cleanup_formats(struct d3d12_device *device)
{
    vkd3d_free((void *)device->formats);

    device->formats = NULL;
}

HRESULT vkd3d_init_format_info(struct d3d12_device *device)
{
    HRESULT hr;

    if (FAILED(hr = vkd3d_init_depth_stencil_formats(device)))
        return hr;

    if (FAILED(hr = vkd3d_init_format_compatibility_lists(device)))
    {
        vkd3d_cleanup_depth_stencil_formats(device);
        return hr;
    }

    if (FAILED(hr = vkd3d_init_formats(device)))
    {
        vkd3d_cleanup_depth_stencil_formats(device);
        vkd3d_cleanup_format_compatibility_lists(device);
    }

    return hr;
}

void vkd3d_cleanup_format_info(struct d3d12_device *device)
{
    vkd3d_cleanup_depth_stencil_formats(device);
    vkd3d_cleanup_format_compatibility_lists(device);
    vkd3d_cleanup_formats(device);
}

/* We use overrides for depth/stencil formats. This is required in order to
 * properly support typeless formats because depth/stencil formats are only
 * compatible with themselves in Vulkan.
 */
static const struct vkd3d_format *vkd3d_get_depth_stencil_format(const struct d3d12_device *device,
        DXGI_FORMAT dxgi_format)
{
    const struct vkd3d_format *format;

    assert(device);
    format = &device->depth_stencil_formats[dxgi_format];

    return format->dxgi_format ? format : NULL;
}

const struct vkd3d_format *vkd3d_get_format(const struct d3d12_device *device,
        DXGI_FORMAT dxgi_format, bool depth_stencil)
{
    const struct vkd3d_format *format;

    if (!is_valid_format(dxgi_format))
    {
        ERR("Invalid format %d.\n", dxgi_format);
        return NULL;
    }

    /* If we request a depth-stencil format (or typeless variant) that is planar,
     * there cannot be any ambiguity which format to select, we must choose a depth-stencil format.
     * For single aspect formats,
     * there are cases where we need to choose either COLOR or DEPTH aspect variants based on depth_stencil argument,
     * but there cannot be any such issue for DEPTH_STENCIL types.
     * This fixes issues where e.g. R24_UNORM_X8_TYPELESS format is used without ALLOW_DEPTH_STENCIL. */
    format = vkd3d_get_depth_stencil_format(device, dxgi_format);
    if (format && (depth_stencil || format->plane_count > 1))
        return format;

    format = &device->formats[dxgi_format];

    return format->dxgi_format ? format : NULL;
}

struct vkd3d_format_footprint vkd3d_format_footprint_for_plane(const struct vkd3d_format *format, unsigned int plane_idx)
{
    if (format->plane_footprints)
    {
        return format->plane_footprints[plane_idx];
    }
    else
    {
        struct vkd3d_format_footprint footprint;
        footprint.dxgi_format = format->dxgi_format;
        footprint.block_width = format->block_width;
        footprint.block_height = format->block_height;
        footprint.subsample_x_log2 = 0;
        footprint.subsample_y_log2 = 0;
        footprint.block_byte_count = format->byte_count * format->block_byte_count;
        return footprint;
    }
}

VkFormat vkd3d_internal_get_vk_format(const struct d3d12_device *device, DXGI_FORMAT dxgi_format)
{
    const struct vkd3d_format *format;

    if ((format = vkd3d_get_format(device, dxgi_format, false)))
        return format->vk_format;

    return VK_FORMAT_UNDEFINED;
}

void vkd3d_format_copy_data(const struct vkd3d_format *format, const uint8_t *src,
        unsigned int src_row_pitch, unsigned int src_slice_pitch, uint8_t *dst, unsigned int dst_row_pitch,
        unsigned int dst_slice_pitch, unsigned int w, unsigned int h, unsigned int d)
{
    unsigned int row_block_count, row_count, row_size, slice, row;
    unsigned int slice_count = d;
    const uint8_t *src_row;
    uint8_t *dst_row;

    row_block_count = (w + format->block_width - 1) / format->block_width;
    row_count = (h + format->block_height - 1) / format->block_height;
    row_size = row_block_count * format->byte_count * format->block_byte_count;

    for (slice = 0; slice < slice_count; ++slice)
    {
        for (row = 0; row < row_count; ++row)
        {
            src_row = &src[slice * src_slice_pitch + row * src_row_pitch];
            dst_row = &dst[slice * dst_slice_pitch + row * dst_row_pitch];
            memcpy(dst_row, src_row, row_size);
        }
    }
}

VkFormat vkd3d_get_vk_format(DXGI_FORMAT format)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(vkd3d_formats); ++i)
    {
        if (vkd3d_formats[i].dxgi_format == format)
            return vkd3d_formats[i].vk_format;
    }

    return VK_FORMAT_UNDEFINED;
}

/* Get some size-based low bits for memory prioritization in the same
   way as d3d12; d3d12 bumps certain resource priorities to
   D3D12_RESIDENCY_PRIORITY_HIGH + size-based bits (10MB resolution)
   see: https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency#default-priority-algorithm */
uint32_t vkd3d_get_priority_adjust(VkDeviceSize size)
{
    return min((size / (10 * 1048576)), 0xffffUL);
}

static float vkd3d_lerp_u32_to_float(uint32_t uval, uint32_t ustart, uint32_t uend, float fstart, float fend)
{
    float a;

    if (uval <= ustart) return fstart;
    else if (uval >= uend) return fend;

    a = (float)(uval - ustart) / (float)(uend - ustart);
    return fstart * (1.0f - a) + (fend * a);
}

/* map from 32-bit d3d prio to float (0..1) vk prio. */
float vkd3d_convert_to_vk_prio(D3D12_RESIDENCY_PRIORITY d3d12prio)
{
    float result;

    /* align D3D12_RESIDENCY_PRIORITY_NORMAL (the default d3d12 prio) with
       0.5 (the default vk prio) so neither kind wins without explicit prio */
    if (d3d12prio <= D3D12_RESIDENCY_PRIORITY_NORMAL)
    {
        result = vkd3d_lerp_u32_to_float(d3d12prio,
            0, D3D12_RESIDENCY_PRIORITY_NORMAL,
            0.001f, 0.500f);
    }
    else if (d3d12prio <= D3D12_RESIDENCY_PRIORITY_HIGH)
    {
        result = vkd3d_lerp_u32_to_float(d3d12prio,
            D3D12_RESIDENCY_PRIORITY_NORMAL, D3D12_RESIDENCY_PRIORITY_HIGH,
            0.500f, 0.700f);
    }
    else if (d3d12prio <= D3D12_RESIDENCY_PRIORITY_HIGH+0xffff)
    {
        result = vkd3d_lerp_u32_to_float(d3d12prio,
            D3D12_RESIDENCY_PRIORITY_HIGH, D3D12_RESIDENCY_PRIORITY_HIGH+0xffff,
            0.700f, 0.800f);
    }
    else
    {
        result = vkd3d_lerp_u32_to_float(d3d12prio,
            D3D12_RESIDENCY_PRIORITY_HIGH+0xffff, UINT32_MAX,
            0.800f, 1.000f);
    }

    /* Note: A naive conversion from a UINT32 d3d priority to a float32 vk priority
       loses around 9 of the 16 lower-order bits which encode size-based subranking
       in the D3D12_RESIDENCY_PRIORITY_HIGH to HIGH+0xffff domain.  The above expansion
       of that domain into a proportionally wider range works around this. */

    /* 0.0f is reserved for explicitly evicted resources */
    return max(min(result, 1.f), 0.001f);
}

DXGI_FORMAT vkd3d_get_dxgi_format(VkFormat format)
{
    DXGI_FORMAT dxgi_format;
    VkFormat vk_format;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(vkd3d_formats); ++i)
    {
        vk_format = vkd3d_formats[i].vk_format;
        dxgi_format = vkd3d_formats[i].dxgi_format;
        if (vk_format == format && vkd3d_formats[i].type != VKD3D_FORMAT_TYPE_TYPELESS)
            return dxgi_format;
    }

    FIXME("Unhandled Vulkan format %#x.\n", format);
    return DXGI_FORMAT_UNKNOWN;
}

bool is_valid_feature_level(D3D_FEATURE_LEVEL feature_level)
{
    static const D3D_FEATURE_LEVEL valid_feature_levels[] =
    {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(valid_feature_levels); ++i)
    {
        if (valid_feature_levels[i] == feature_level)
            return true;
    }

    return false;
}

bool is_write_resource_state(D3D12_RESOURCE_STATES state)
{
    return state & (D3D12_RESOURCE_STATE_RENDER_TARGET
            | D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            | D3D12_RESOURCE_STATE_DEPTH_WRITE
            | D3D12_RESOURCE_STATE_STREAM_OUT
            | D3D12_RESOURCE_STATE_COPY_DEST
            | D3D12_RESOURCE_STATE_RESOLVE_DEST);
}

bool is_valid_resource_state(D3D12_RESOURCE_STATES state)
{
    const D3D12_RESOURCE_STATES valid_states =
            D3D12_RESOURCE_STATE_COMMON |
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
            D3D12_RESOURCE_STATE_INDEX_BUFFER |
            D3D12_RESOURCE_STATE_RENDER_TARGET |
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
            D3D12_RESOURCE_STATE_DEPTH_WRITE |
            D3D12_RESOURCE_STATE_DEPTH_READ |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_STREAM_OUT |
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
            D3D12_RESOURCE_STATE_COPY_DEST |
            D3D12_RESOURCE_STATE_COPY_SOURCE |
            D3D12_RESOURCE_STATE_RESOLVE_DEST |
            D3D12_RESOURCE_STATE_RESOLVE_SOURCE |
            D3D12_RESOURCE_STATE_GENERIC_READ |
            D3D12_RESOURCE_STATE_PRESENT |
            D3D12_RESOURCE_STATE_PREDICATION |
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE |
            D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;

    if (state & ~valid_states)
    {
        WARN("Invalid resource states %#x.\n", state & ~valid_states);
        return false;
    }

    /* Exactly one bit must be set for write states. */
    if (is_write_resource_state(state) && !is_power_of_two(state))
    {
        WARN("Write state cannot be mixed with other states: %#x.\n", state);
        return false;
    }

    return true;
}

bool is_valid_format(DXGI_FORMAT dxgi_format)
{
    if (dxgi_format >= DXGI_FORMAT_UNKNOWN && dxgi_format <= DXGI_FORMAT_B4G4R4A4_UNORM)
        return true;
    if (dxgi_format >= DXGI_FORMAT_P208 && dxgi_format <= DXGI_FORMAT_A4B4G4R4_UNORM)
        return true;
    return false;
}

HRESULT return_interface(void *iface, REFIID iface_iid,
        REFIID requested_iid, void **object)
{
    IUnknown *unknown = iface;
    HRESULT hr;

    /* Don't check IID here, we always have to return S_FALSE. */
    if (!object)
    {
        IUnknown_Release(unknown);
        return S_FALSE;
    }

    if (IsEqualGUID(iface_iid, requested_iid))
    {
        *object = unknown;
        return S_OK;
    }

    hr = IUnknown_QueryInterface(unknown, requested_iid, object);
    IUnknown_Release(unknown);
    return hr;
}

const char *debug_dxgi_format(DXGI_FORMAT format)
{
    #define ENUM_NAME(x) \
        case x: return #x;

    switch(format)
    {
        ENUM_NAME(DXGI_FORMAT_UNKNOWN)
        ENUM_NAME(DXGI_FORMAT_R32G32B32A32_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R32G32B32A32_FLOAT)
        ENUM_NAME(DXGI_FORMAT_R32G32B32A32_UINT)
        ENUM_NAME(DXGI_FORMAT_R32G32B32A32_SINT)
        ENUM_NAME(DXGI_FORMAT_R32G32B32_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R32G32B32_FLOAT)
        ENUM_NAME(DXGI_FORMAT_R32G32B32_UINT)
        ENUM_NAME(DXGI_FORMAT_R32G32B32_SINT)
        ENUM_NAME(DXGI_FORMAT_R16G16B16A16_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R16G16B16A16_FLOAT)
        ENUM_NAME(DXGI_FORMAT_R16G16B16A16_UNORM)
        ENUM_NAME(DXGI_FORMAT_R16G16B16A16_UINT)
        ENUM_NAME(DXGI_FORMAT_R16G16B16A16_SNORM)
        ENUM_NAME(DXGI_FORMAT_R16G16B16A16_SINT)
        ENUM_NAME(DXGI_FORMAT_R32G32_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R32G32_FLOAT)
        ENUM_NAME(DXGI_FORMAT_R32G32_UINT)
        ENUM_NAME(DXGI_FORMAT_R32G32_SINT)
        ENUM_NAME(DXGI_FORMAT_R32G8X24_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
        ENUM_NAME(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_X32_TYPELESS_G8X24_UINT)
        ENUM_NAME(DXGI_FORMAT_R10G10B10A2_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R10G10B10A2_UNORM)
        ENUM_NAME(DXGI_FORMAT_R10G10B10A2_UINT)
        ENUM_NAME(DXGI_FORMAT_R11G11B10_FLOAT)
        ENUM_NAME(DXGI_FORMAT_R8G8B8A8_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R8G8B8A8_UNORM)
        ENUM_NAME(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        ENUM_NAME(DXGI_FORMAT_R8G8B8A8_UINT)
        ENUM_NAME(DXGI_FORMAT_R8G8B8A8_SNORM)
        ENUM_NAME(DXGI_FORMAT_R8G8B8A8_SINT)
        ENUM_NAME(DXGI_FORMAT_R16G16_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R16G16_FLOAT)
        ENUM_NAME(DXGI_FORMAT_R16G16_UNORM)
        ENUM_NAME(DXGI_FORMAT_R16G16_UINT)
        ENUM_NAME(DXGI_FORMAT_R16G16_SNORM)
        ENUM_NAME(DXGI_FORMAT_R16G16_SINT)
        ENUM_NAME(DXGI_FORMAT_R32_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_D32_FLOAT)
        ENUM_NAME(DXGI_FORMAT_R32_FLOAT)
        ENUM_NAME(DXGI_FORMAT_R32_UINT)
        ENUM_NAME(DXGI_FORMAT_R32_SINT)
        ENUM_NAME(DXGI_FORMAT_R24G8_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_D24_UNORM_S8_UINT)
        ENUM_NAME(DXGI_FORMAT_R24_UNORM_X8_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_X24_TYPELESS_G8_UINT)
        ENUM_NAME(DXGI_FORMAT_R8G8_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R8G8_UNORM)
        ENUM_NAME(DXGI_FORMAT_R8G8_UINT)
        ENUM_NAME(DXGI_FORMAT_R8G8_SNORM)
        ENUM_NAME(DXGI_FORMAT_R8G8_SINT)
        ENUM_NAME(DXGI_FORMAT_R16_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R16_FLOAT)
        ENUM_NAME(DXGI_FORMAT_D16_UNORM)
        ENUM_NAME(DXGI_FORMAT_R16_UNORM)
        ENUM_NAME(DXGI_FORMAT_R16_UINT)
        ENUM_NAME(DXGI_FORMAT_R16_SNORM)
        ENUM_NAME(DXGI_FORMAT_R16_SINT)
        ENUM_NAME(DXGI_FORMAT_R8_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_R8_UNORM)
        ENUM_NAME(DXGI_FORMAT_R8_UINT)
        ENUM_NAME(DXGI_FORMAT_R8_SNORM)
        ENUM_NAME(DXGI_FORMAT_R8_SINT)
        ENUM_NAME(DXGI_FORMAT_A8_UNORM)
        ENUM_NAME(DXGI_FORMAT_R1_UNORM)
        ENUM_NAME(DXGI_FORMAT_R9G9B9E5_SHAREDEXP)
        ENUM_NAME(DXGI_FORMAT_R8G8_B8G8_UNORM)
        ENUM_NAME(DXGI_FORMAT_G8R8_G8B8_UNORM)
        ENUM_NAME(DXGI_FORMAT_BC1_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_BC1_UNORM)
        ENUM_NAME(DXGI_FORMAT_BC1_UNORM_SRGB)
        ENUM_NAME(DXGI_FORMAT_BC2_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_BC2_UNORM)
        ENUM_NAME(DXGI_FORMAT_BC2_UNORM_SRGB)
        ENUM_NAME(DXGI_FORMAT_BC3_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_BC3_UNORM)
        ENUM_NAME(DXGI_FORMAT_BC3_UNORM_SRGB)
        ENUM_NAME(DXGI_FORMAT_BC4_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_BC4_UNORM)
        ENUM_NAME(DXGI_FORMAT_BC4_SNORM)
        ENUM_NAME(DXGI_FORMAT_BC5_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_BC5_UNORM)
        ENUM_NAME(DXGI_FORMAT_BC5_SNORM)
        ENUM_NAME(DXGI_FORMAT_B5G6R5_UNORM)
        ENUM_NAME(DXGI_FORMAT_B5G5R5A1_UNORM)
        ENUM_NAME(DXGI_FORMAT_B8G8R8A8_UNORM)
        ENUM_NAME(DXGI_FORMAT_B8G8R8X8_UNORM)
        ENUM_NAME(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM)
        ENUM_NAME(DXGI_FORMAT_B8G8R8A8_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        ENUM_NAME(DXGI_FORMAT_B8G8R8X8_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_B8G8R8X8_UNORM_SRGB)
        ENUM_NAME(DXGI_FORMAT_BC6H_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_BC6H_UF16)
        ENUM_NAME(DXGI_FORMAT_BC6H_SF16)
        ENUM_NAME(DXGI_FORMAT_BC7_TYPELESS)
        ENUM_NAME(DXGI_FORMAT_BC7_UNORM)
        ENUM_NAME(DXGI_FORMAT_BC7_UNORM_SRGB)
        ENUM_NAME(DXGI_FORMAT_AYUV)
        ENUM_NAME(DXGI_FORMAT_Y410)
        ENUM_NAME(DXGI_FORMAT_Y416)
        ENUM_NAME(DXGI_FORMAT_NV12)
        ENUM_NAME(DXGI_FORMAT_P010)
        ENUM_NAME(DXGI_FORMAT_P016)
        ENUM_NAME(DXGI_FORMAT_420_OPAQUE)
        ENUM_NAME(DXGI_FORMAT_YUY2)
        ENUM_NAME(DXGI_FORMAT_Y210)
        ENUM_NAME(DXGI_FORMAT_Y216)
        ENUM_NAME(DXGI_FORMAT_NV11)
        ENUM_NAME(DXGI_FORMAT_AI44)
        ENUM_NAME(DXGI_FORMAT_IA44)
        ENUM_NAME(DXGI_FORMAT_P8)
        ENUM_NAME(DXGI_FORMAT_A8P8)
        ENUM_NAME(DXGI_FORMAT_B4G4R4A4_UNORM)
        ENUM_NAME(DXGI_FORMAT_P208)
        ENUM_NAME(DXGI_FORMAT_V208)
        ENUM_NAME(DXGI_FORMAT_V408)
        ENUM_NAME(DXGI_FORMAT_UNDOCUMENTED_ASTC_FIRST)
        ENUM_NAME(DXGI_FORMAT_UNDOCUMENTED_ASTC_LAST)
        ENUM_NAME(DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE)
        ENUM_NAME(DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE)
        ENUM_NAME(DXGI_FORMAT_A4B4G4R4_UNORM)
        ENUM_NAME(DXGI_FORMAT_FORCE_UINT)
    }
    #undef ENUM_NAME

    return vkd3d_dbg_sprintf("Unknown DXGI_FORMAT (%u)",
        (uint32_t) format);
}

const char *debug_d3d12_box(const D3D12_BOX *box)
{
    if (!box)
        return "(null)";

    return vkd3d_dbg_sprintf("(%u, %u, %u)-(%u, %u, %u)",
            box->left, box->top, box->front,
            box->right, box->bottom, box->back);
}

static const char *debug_d3d12_shader_component(D3D12_SHADER_COMPONENT_MAPPING component)
{
    switch (component)
    {
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0:
            return "r";
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1:
            return "g";
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2:
            return "b";
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3:
            return "a";
        case D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0:
            return "0";
        case D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1:
            return "1";
    }

    FIXME("Invalid component mapping %#x.\n", component);
    return "invalid";
}

const char *debug_d3d12_shader_component_mapping(unsigned int mapping)
{
    return vkd3d_dbg_sprintf("{%s, %s, %s, %s}",
            debug_d3d12_shader_component(D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(0, mapping)),
            debug_d3d12_shader_component(D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(1, mapping)),
            debug_d3d12_shader_component(D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(2, mapping)),
            debug_d3d12_shader_component(D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(3, mapping)));
}

const char *debug_vk_extent_3d(VkExtent3D extent)
{
    return vkd3d_dbg_sprintf("(%u, %u, %u)",
            (unsigned int)extent.width,
            (unsigned int)extent.height,
            (unsigned int)extent.depth);
}

const char *debug_vk_queue_flags(VkQueueFlags flags, char buffer[VKD3D_DEBUG_FLAGS_BUFFER_SIZE])
{
    buffer[0] = '\0';
#define FLAG_TO_STR(f) if (flags & f) { strcat(buffer, " | "#f); flags &= ~f; }
    FLAG_TO_STR(VK_QUEUE_GRAPHICS_BIT)
    FLAG_TO_STR(VK_QUEUE_COMPUTE_BIT)
    FLAG_TO_STR(VK_QUEUE_TRANSFER_BIT)
    FLAG_TO_STR(VK_QUEUE_SPARSE_BINDING_BIT)
    FLAG_TO_STR(VK_QUEUE_PROTECTED_BIT)
    FLAG_TO_STR(VK_QUEUE_OPTICAL_FLOW_BIT_NV)
#undef FLAG_TO_STR
    if (flags)
        FIXME("Unrecognized flag(s) %#x.\n", flags);

    if (!buffer[0])
        return "0";
    return vkd3d_dbg_sprintf("%s", &buffer[3]);
}

const char *debug_vk_memory_heap_flags(VkMemoryHeapFlags flags, char buffer[VKD3D_DEBUG_FLAGS_BUFFER_SIZE])
{
    buffer[0] = '\0';
#define FLAG_TO_STR(f) if (flags & f) { strcat(buffer, " | "#f); flags &= ~f; }
    FLAG_TO_STR(VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
    FLAG_TO_STR(VK_MEMORY_HEAP_MULTI_INSTANCE_BIT)
#undef FLAG_TO_STR
    if (flags)
        FIXME("Unrecognized flag(s) %#x.\n", flags);

    if (!buffer[0])
        return "0";
    return vkd3d_dbg_sprintf("%s", &buffer[3]);
}

const char *debug_vk_memory_property_flags(VkMemoryPropertyFlags flags, char buffer[VKD3D_DEBUG_FLAGS_BUFFER_SIZE])
{
    buffer[0] = '\0';
#define FLAG_TO_STR(f) if (flags & f) { strcat(buffer, " | "#f); flags &= ~f; }
    FLAG_TO_STR(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    FLAG_TO_STR(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
    FLAG_TO_STR(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    FLAG_TO_STR(VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
    FLAG_TO_STR(VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
    FLAG_TO_STR(VK_MEMORY_PROPERTY_PROTECTED_BIT)
    FLAG_TO_STR(VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD)
    FLAG_TO_STR(VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)
#undef FLAG_TO_STR
    if (flags)
        FIXME("Unrecognized flag(s) %#x.\n", flags);

    if (!buffer[0])
        return "0";
    return vkd3d_dbg_sprintf("%s", &buffer[3]);
}

HRESULT hresult_from_errno(int rc)
{
    switch (rc)
    {
        case 0:
            return S_OK;
        case ENOMEM:
            return E_OUTOFMEMORY;
        case EINVAL:
            return E_INVALIDARG;
        default:
            FIXME("Unhandled errno %d.\n", rc);
            return E_FAIL;
    }
}

HRESULT hresult_from_vk_result(VkResult vr)
{
    /* Wine tends to dispatch Vulkan calls to their own syscall stack.
     * Crashes are captured and return this magic VkResult.
     * Report it explicitly here so it's easier to debug when it happens. */
    if (vr == -1073741819)
    {
        ERR("Detected segfault in Wine syscall handler.\n");
        /* HACK: For ad-hoc debugging can also trigger backtrace printing here. */
        return E_POINTER;
    }

    switch (vr)
    {
        case VK_SUCCESS:
            return S_OK;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            WARN("Out of device memory.\n");
            /* fall-through */
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return E_OUTOFMEMORY;
        case VK_ERROR_VALIDATION_FAILED_EXT:
            /* NV driver sometimes returns this on invalid API usage. */
            return E_INVALIDARG;
        default:
            FIXME("Unhandled VkResult %d.\n", vr);
            /* fall-through */
        case VK_ERROR_DEVICE_LOST:
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return E_FAIL;
    }
}

HRESULT hresult_from_vkd3d_result(int vkd3d_result)
{
    switch (vkd3d_result)
    {
        case VKD3D_OK:
            return S_OK;
        case VKD3D_ERROR_INVALID_SHADER:
            WARN("Invalid shader bytecode.\n");
            /* fall-through */
        case VKD3D_ERROR:
            return E_FAIL;
        case VKD3D_ERROR_OUT_OF_MEMORY:
            return E_OUTOFMEMORY;
        case VKD3D_ERROR_INVALID_ARGUMENT:
            return E_INVALIDARG;
        case VKD3D_ERROR_NOT_IMPLEMENTED:
            return E_NOTIMPL;
        default:
            FIXME("Unhandled vkd3d result %d.\n", vkd3d_result);
            return E_FAIL;
    }
}

#define LOAD_GLOBAL_PFN(name) \
    if (!(procs->name = (void *)vkGetInstanceProcAddr(NULL, #name))) \
    { \
        ERR("Could not get global proc addr for '" #name "'.\n"); \
        return E_FAIL; \
    }
#define MAYBE_LOAD_GLOBAL_PFN(name) \
    procs->name = (void *)vkGetInstanceProcAddr(NULL, #name);

HRESULT vkd3d_load_vk_global_procs(struct vkd3d_vk_global_procs *procs,
        PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr)
{
    memset(procs, 0, sizeof(*procs));

    procs->vkGetInstanceProcAddr = vkGetInstanceProcAddr;

    LOAD_GLOBAL_PFN(vkCreateInstance)
    LOAD_GLOBAL_PFN(vkEnumerateInstanceExtensionProperties)
    LOAD_GLOBAL_PFN(vkEnumerateInstanceLayerProperties)
    MAYBE_LOAD_GLOBAL_PFN(vkEnumerateInstanceVersion)

    TRACE("Loaded global Vulkan procs.\n");
    return S_OK;
}

#define LOAD_INSTANCE_PFN(name) \
    if (!(procs->name = (void *)global_procs->vkGetInstanceProcAddr(instance, #name))) \
    { \
        ERR("Could not get instance proc addr for '" #name "'.\n"); \
        return E_FAIL; \
    }
#define LOAD_INSTANCE_OPTIONAL_PFN(name) \
    procs->name = (void *)global_procs->vkGetInstanceProcAddr(instance, #name);

HRESULT vkd3d_load_vk_instance_procs(struct vkd3d_vk_instance_procs *procs,
        const struct vkd3d_vk_global_procs *global_procs, VkInstance instance)
{
    memset(procs, 0, sizeof(*procs));

#define VK_INSTANCE_PFN     LOAD_INSTANCE_PFN
#define VK_INSTANCE_EXT_PFN LOAD_INSTANCE_OPTIONAL_PFN
#include "vulkan_procs.h"

    TRACE("Loaded procs for VkInstance %p.\n", instance);
    return S_OK;
}

#define COPY_PARENT_PFN(name) procs->name = parent_procs->name;
#define LOAD_DEVICE_PFN(name) \
    if (!(procs->name = (void *)procs->vkGetDeviceProcAddr(device, #name))) \
    { \
        ERR("Could not get device proc addr for '" #name "'.\n"); \
        return E_FAIL; \
    }
#define LOAD_DEVICE_OPTIONAL_PFN(name) \
    procs->name = (void *)procs->vkGetDeviceProcAddr(device, #name);

HRESULT vkd3d_load_vk_device_procs(struct vkd3d_vk_device_procs *procs,
        const struct vkd3d_vk_instance_procs *parent_procs, VkDevice device)
{
    memset(procs, 0, sizeof(*procs));

#define VK_INSTANCE_PFN       COPY_PARENT_PFN
#define VK_INSTANCE_EXT_PFN   COPY_PARENT_PFN
#define VK_DEVICE_PFN         LOAD_DEVICE_PFN
#define VK_DEVICE_EXT_PFN     LOAD_DEVICE_OPTIONAL_PFN
#include "vulkan_procs.h"

    TRACE("Loaded procs for VkDevice %p.\n", device);
    return S_OK;
}

static struct vkd3d_private_data *vkd3d_private_store_get_private_data(
        const struct vkd3d_private_store *store, const GUID *tag)
{
    struct vkd3d_private_data *data;

    LIST_FOR_EACH_ENTRY(data, &store->content, struct vkd3d_private_data, entry)
    {
        if (IsEqualGUID(&data->tag, tag))
            return data;
    }

    return NULL;
}

HRESULT vkd3d_private_store_set_private_data(struct vkd3d_private_store *store,
        const GUID *tag, const void *data, unsigned int data_size, bool is_object)
{
    struct vkd3d_private_data *d, *old_data;
    const void *ptr = data;

    if (!data)
    {
        if ((d = vkd3d_private_store_get_private_data(store, tag)))
        {
            vkd3d_private_data_destroy(d);
            return S_OK;
        }

        return S_FALSE;
    }

    if (is_object)
    {
        if (data_size != sizeof(IUnknown *))
            return E_INVALIDARG;
        ptr = &data;
    }

    if (!(d = vkd3d_malloc(offsetof(struct vkd3d_private_data, data[data_size]))))
        return E_OUTOFMEMORY;

    d->tag = *tag;
    d->size = data_size;
    d->is_object = is_object;
    memcpy(d->data, ptr, data_size);
    if (is_object)
        IUnknown_AddRef(d->object);

    if ((old_data = vkd3d_private_store_get_private_data(store, tag)))
        vkd3d_private_data_destroy(old_data);
    list_add_tail(&store->content, &d->entry);

    return S_OK;
}

HRESULT vkd3d_get_private_data(struct vkd3d_private_store *store,
        const GUID *tag, unsigned int *out_size, void *out)
{
    const struct vkd3d_private_data *data;
    unsigned int size;
    HRESULT hr;

    if (!out_size)
        return E_INVALIDARG;

    if (FAILED(hr = vkd3d_private_data_lock(store)))
        return hr;

    if (!(data = vkd3d_private_store_get_private_data(store, tag)))
    {
        *out_size = 0;
        hr = DXGI_ERROR_NOT_FOUND;
        goto done;
    }

    size = *out_size;
    *out_size = data->size;
    if (!out)
        goto done;

    if (size < data->size)
    {
        hr = DXGI_ERROR_MORE_DATA;
        goto done;
    }

    if (data->is_object)
        IUnknown_AddRef(data->object);
    memcpy(out, data->data, data->size);

done:
    vkd3d_private_data_unlock(store);
    return hr;
}

HRESULT STDMETHODCALLTYPE d3d12_object_SetName(ID3D12Object *iface, const WCHAR *name)
{
    size_t size = 0;

    TRACE("iface %p, name %s.\n", iface, debugstr_w(name));

    if (name)
        size = sizeof(WCHAR) * (vkd3d_wcslen(name) + 1);

    return ID3D12Object_SetPrivateData(iface, &WKPDID_D3DDebugObjectNameW, size, name);
}

HRESULT vkd3d_set_vk_object_name(struct d3d12_device *device, uint64_t vk_object,
        VkObjectType vk_object_type, const char *name)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkDebugUtilsObjectNameInfoEXT info;
    VkResult vr;

    if (!device->vk_info.EXT_debug_utils)
        return VK_SUCCESS;

    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.pNext = NULL;
    info.objectType = vk_object_type;
    info.objectHandle = vk_object;
    info.pObjectName = name;

    vr = VK_CALL(vkSetDebugUtilsObjectNameEXT(device->vk_device, &info));
    return hresult_from_vk_result(vr);
}

static struct d3d_blob *impl_from_ID3DBlob(ID3DBlob *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_blob, ID3DBlob_iface);
}

static HRESULT STDMETHODCALLTYPE d3d_blob_QueryInterface(ID3DBlob *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3DBlob)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D10Blob_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d_blob_AddRef(ID3DBlob *iface)
{
    struct d3d_blob *blob = impl_from_ID3DBlob(iface);
    ULONG refcount = InterlockedIncrement(&blob->refcount);

    TRACE("%p increasing refcount to %u.\n", blob, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d_blob_Release(ID3DBlob *iface)
{
    struct d3d_blob *blob = impl_from_ID3DBlob(iface);
    ULONG refcount = InterlockedDecrement(&blob->refcount);

    TRACE("%p decreasing refcount to %u.\n", blob, refcount);

    if (!refcount)
    {
        vkd3d_free(blob->buffer);

        vkd3d_free(blob);
    }

    return refcount;
}

static void * STDMETHODCALLTYPE d3d_blob_GetBufferPointer(ID3DBlob *iface)
{
    struct d3d_blob *blob = impl_from_ID3DBlob(iface);

    TRACE("iface %p.\n", iface);

    return blob->buffer;
}

static SIZE_T STDMETHODCALLTYPE d3d_blob_GetBufferSize(ID3DBlob *iface)
{
    struct d3d_blob *blob = impl_from_ID3DBlob(iface);

    TRACE("iface %p.\n", iface);

    return blob->size;
}

static CONST_VTBL struct ID3D10BlobVtbl d3d_blob_vtbl =
{
    /* IUnknown methods */
    d3d_blob_QueryInterface,
    d3d_blob_AddRef,
    d3d_blob_Release,
    /* ID3DBlob methods */
    d3d_blob_GetBufferPointer,
    d3d_blob_GetBufferSize
};

static void d3d_blob_init(struct d3d_blob *blob, void *buffer, SIZE_T size)
{
    blob->ID3DBlob_iface.lpVtbl = &d3d_blob_vtbl;
    blob->refcount = 1;

    blob->buffer = buffer;
    blob->size = size;
}

HRESULT d3d_blob_create(void *buffer, SIZE_T size, struct d3d_blob **blob)
{
    struct d3d_blob *object;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    d3d_blob_init(object, buffer, size);

    TRACE("Created blob object %p.\n", object);

    *blob = object;

    return S_OK;
}


static struct d3d_destruction_notifier *impl_from_ID3DDestructionNotifier(ID3DDestructionNotifier *iface)
{
    return CONTAINING_RECORD(iface, struct d3d_destruction_notifier, ID3DDestructionNotifier_iface);
}

static HRESULT STDMETHODCALLTYPE d3d_destruction_notifier_QueryInterface(ID3DDestructionNotifier *iface, REFIID riid, void **object)
{
    struct d3d_destruction_notifier *notifier = impl_from_ID3DDestructionNotifier(iface);

    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    return IUnknown_QueryInterface(notifier->parent, riid, object);
}

static ULONG STDMETHODCALLTYPE d3d_destruction_notifier_AddRef(ID3DDestructionNotifier *iface)
{
    struct d3d_destruction_notifier *notifier = impl_from_ID3DDestructionNotifier(iface);
    ULONG refcount = IUnknown_AddRef(notifier->parent);

    TRACE("%p increasing refcount to %u.\n", notifier, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d_destruction_notifier_Release(ID3DDestructionNotifier *iface)
{
    struct d3d_destruction_notifier *notifier = impl_from_ID3DDestructionNotifier(iface);
    ULONG refcount = IUnknown_Release(notifier->parent);

    TRACE("%p decreasing refcount to %u.\n", notifier, refcount);

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d_destruction_notifier_RegisterDestructionCallback(
        ID3DDestructionNotifier *iface, PFN_DESTRUCTION_CALLBACK callback, void *data,
        UINT *callback_id)
{
    struct d3d_destruction_notifier *notifier = impl_from_ID3DDestructionNotifier(iface);
    struct d3d_destruction_callback_entry *entry;

    TRACE("iface %p, callback %p, data %p, callback_id %p.\n", iface, (void*)callback, data, callback_id);

    if (!callback)
        return DXGI_ERROR_INVALID_CALL;

    pthread_mutex_lock(&notifier->mutex);

    if (!vkd3d_array_reserve((void**)&notifier->callbacks, &notifier->callback_size,
            notifier->callback_count + 1, sizeof(*notifier->callbacks)))
    {
        ERR("Failed to allocate callback array.\n");
        pthread_mutex_unlock(&notifier->mutex);
        return E_OUTOFMEMORY;
    }

    entry = &notifier->callbacks[notifier->callback_count++];
    entry->callback = callback;
    entry->userdata = data;
    entry->callback_id = 0;

    if (callback_id)
    {
        entry->callback_id = ++notifier->next_callback_id;
        *callback_id = entry->callback_id;
    }

    pthread_mutex_unlock(&notifier->mutex);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d_destruction_notifier_UnregisterDestructionCallback(
        ID3DDestructionNotifier *iface, UINT callback_id)
{
    struct d3d_destruction_notifier *notifier = impl_from_ID3DDestructionNotifier(iface);
    unsigned int i, entry_index;

    TRACE("iface %p, callback_id %u.\n", iface, callback_id);

    if (!callback_id)
        return DXGI_ERROR_NOT_FOUND;

    pthread_mutex_lock(&notifier->mutex);

    entry_index = -1u;

    for (i = 0; i < notifier->callback_count; i++)
    {
        if (notifier->callbacks[i].callback_id == callback_id)
        {
            entry_index = i;
            break;
        }
    }

    if (entry_index == -1u)
    {
        pthread_mutex_unlock(&notifier->mutex);
        return DXGI_ERROR_NOT_FOUND;
    }

    notifier->callbacks[entry_index] = notifier->callbacks[--notifier->callback_count];

    pthread_mutex_unlock(&notifier->mutex);
    return S_OK;
}

static CONST_VTBL struct ID3DDestructionNotifierVtbl d3d_destruction_notifier_vtbl =
{
    /* IUnknown methods */
    d3d_destruction_notifier_QueryInterface,
    d3d_destruction_notifier_AddRef,
    d3d_destruction_notifier_Release,
    /* ID3DDestructionNotifier methods */
    d3d_destruction_notifier_RegisterDestructionCallback,
    d3d_destruction_notifier_UnregisterDestructionCallback
};

void d3d_destruction_notifier_init(struct d3d_destruction_notifier *notifier, IUnknown *parent)
{
    memset(notifier, 0, sizeof(*notifier));

    notifier->ID3DDestructionNotifier_iface.lpVtbl = &d3d_destruction_notifier_vtbl;
    notifier->parent = parent;

    pthread_mutex_init(&notifier->mutex, NULL);
}

void d3d_destruction_notifier_free(struct d3d_destruction_notifier *notifier)
{
    d3d_destruction_notifier_notify(notifier);

    pthread_mutex_destroy(&notifier->mutex);
}

void d3d_destruction_notifier_notify(struct d3d_destruction_notifier *notifier)
{
    size_t i;

    for (i = 0; i < notifier->callback_count; i++)
    {
        const struct d3d_destruction_callback_entry *entry = &notifier->callbacks[i];

        entry->callback(entry->userdata);
    }

    vkd3d_free(notifier->callbacks);

    notifier->callbacks = NULL;
    notifier->callback_count = 0u;
    notifier->callback_size = 0u;
}

