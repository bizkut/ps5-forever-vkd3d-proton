#ifndef WINEPS5_HITCH_H
#define WINEPS5_HITCH_H

/* PS5 G5d per-hitch attribution (WINEPS5_PROFILE=1). The frame boundary is any
 * swapchain Present (WoW presents from rotating threads), so a hitch is a
 * global Present-to-Present interval over 50 ms. Every attributed thread keeps
 * monotonically increasing totals (vkd3d buckets in d3d12core, Wine
 * wait/read/stack-walk time in its TEB block); at each global Present the
 * presenting thread snapshots all of them, including a wait or vkd3d call
 * still in progress, and differences against the previous Present.
 * Pure helpers only; host-tested in tests/wineps5_fg.c. */

#include <stdbool.h>
#include <stdint.h>

#define WINEPS5_HITCH_THRESHOLD_NS UINT64_C(50000000)
#define WINEPS5_HITCH_WINDOW_NS UINT64_C(10000000000)
#define WINEPS5_HITCH_MAX_LINES 8u

/* Mirror of ps5-wine include/wine/ps5_suspend.h struct wine_ps5_hitch_counters
 * at its fixed x64 TEB offset (Wine C_ASSERTs the offset; both host tests pin
 * the layout). Wine stamps magic/tid/main_block on registered title threads and
 * clears tid at thread exit; every update is bracketed by an odd/even
 * sequence. Timestamps are QueryPerformanceCounter nanoseconds, the clock of
 * vkd3d_get_current_time_ns on Windows. */
#define WINEPS5_HITCH_TEB64_OFFSET 0x568u
#define WINEPS5_HITCH_MAGIC UINT32_C(0x48545350) /* "PSTH" */
#define WINEPS5_HITCH_NO_CORE UINT32_C(0xffffffff)

struct wineps5_hitch_wine
{
    uint32_t magic;
    volatile uint32_t armed;
    volatile uint32_t tid;
    volatile uint32_t sequence;
    uint64_t wait_ns, wait_count, read_ns, read_count, unwind_ns, unwind_count;
    uint64_t wait_start_ns;
    uint64_t main_block;
    uint64_t cpu_ns, cpu_sample_ns; /* owner CPU clock and when it was sampled */
    uint32_t cpu_core;              /* RDTSCP TSC_AUX at that sample */
    uint32_t cpu_samples;
};

struct wineps5_hitch_wine_totals
{
    uint64_t wait_ns, wait_count, read_ns, read_count, unwind_ns, unwind_count;
    uint64_t cpu_samples; /* CPU clock samples taken (transitions and profiler ticks) */
};

/* Totals as of now_ns, counting a wait still in progress. */
static inline void wineps5_hitch_wine_effective(const struct wineps5_hitch_wine *wine,
        uint64_t now_ns, struct wineps5_hitch_wine_totals *out)
{
    out->wait_ns = wine->wait_ns; out->wait_count = wine->wait_count;
    out->read_ns = wine->read_ns; out->read_count = wine->read_count;
    out->unwind_ns = wine->unwind_ns; out->unwind_count = wine->unwind_count;
    out->cpu_samples = wine->cpu_samples;
    if (wine->wait_start_ns && now_ns > wine->wait_start_ns)
        out->wait_ns += now_ns - wine->wait_start_ns;
}

static inline uint64_t wineps5_hitch_wine_nested(const struct wineps5_hitch_wine_totals *t)
{
    return t->wait_ns + t->read_ns + t->unwind_ns;
}

/* CPU time. The PS5 cannot read another thread's CPU clock, so the owner
 * samples its own at each outermost blocking transition and each 20-Hz
 * profiler tick. At a boundary t, a sample (s, c) with s <= t gives
 * c <= cpu(t) <= c + (t - s), or cpu(t) = c when the thread has been blocked
 * since the sample; a later sample with s >= t gives c - (s - t) <= cpu(t) <= c. */
struct wineps5_hitch_cpu_sample
{
    uint64_t cpu_ns, sample_ns; /* sample_ns 0: none */
    uint32_t core;
    bool blocked_since;         /* blocked continuously since the sample */
};

static inline void wineps5_hitch_cpu_read(const struct wineps5_hitch_wine *wine,
        struct wineps5_hitch_cpu_sample *out)
{
    out->cpu_ns = wine->cpu_ns;
    out->sample_ns = wine->cpu_sample_ns;
    out->core = wine->cpu_core;
    out->blocked_since = wine->wait_start_ns && wine->cpu_sample_ns >= wine->wait_start_ns;
}

struct wineps5_hitch_cpu_bound
{
    uint64_t lo, hi;
    uint32_t core; /* of the latest sample used */
    bool valid;
};

/* Tighten the bound on cpu(t) with one sample. */
static inline void wineps5_hitch_cpu_refine(struct wineps5_hitch_cpu_bound *b,
        const struct wineps5_hitch_cpu_sample *s, uint64_t t)
{
    uint64_t lo, hi;

    if (!s->sample_ns)
        return;
    if (s->sample_ns <= t)
    {
        lo = s->cpu_ns;
        hi = s->blocked_since ? s->cpu_ns : s->cpu_ns + (t - s->sample_ns);
    }
    else
    {
        lo = s->cpu_ns > s->sample_ns - t ? s->cpu_ns - (s->sample_ns - t) : 0;
        hi = s->cpu_ns;
    }
    if (!b->valid)
    {
        b->lo = lo;
        b->hi = hi;
        b->valid = true;
    }
    else
    {
        if (lo > b->lo) b->lo = lo;
        if (hi < b->hi) b->hi = hi;
        if (b->lo > b->hi) b->lo = b->hi; /* CPU and wall clocks disagree slightly */
    }
    b->core = s->core;
}

struct wineps5_hitch_cpu
{
    uint64_t cpu_ns; /* midpoint of the bounds, within [0, span] */
    uint64_t err_ns; /* half-width: true CPU time is cpu_ns +- err_ns */
    bool valid;
};

/* CPU time over (t0, t1] from bounds on cpu(t0) and cpu(t1). */
static inline void wineps5_hitch_cpu_interval(const struct wineps5_hitch_cpu_bound *b0,
        const struct wineps5_hitch_cpu_bound *b1, uint64_t span_ns, struct wineps5_hitch_cpu *out)
{
    uint64_t lo, hi;

    out->valid = b0->valid && b1->valid;
    out->cpu_ns = out->err_ns = 0;
    if (!out->valid)
        return;
    lo = b1->lo > b0->hi ? b1->lo - b0->hi : 0;
    hi = b1->hi > b0->lo ? b1->hi - b0->lo : 0;
    if (hi > span_ns) hi = span_ns;
    if (lo > hi) lo = hi;
    out->cpu_ns = lo + (hi - lo) / 2;
    out->err_ns = hi - out->cpu_ns;
}

enum wineps5_hitch_bucket
{
    WINEPS5_HITCH_RESOURCE,        /* committed/placed/reserved resource creation */
    WINEPS5_HITCH_HEAP,            /* ID3D12Heap creation */
    WINEPS5_HITCH_DESCRIPTOR_HEAP, /* descriptor heap creation */
    WINEPS5_HITCH_PSO,             /* pipeline-state creation and draw-time pipeline compiles */
    WINEPS5_HITCH_SUBMIT,          /* ExecuteCommandLists on the calling thread */
    WINEPS5_HITCH_PRESENT,         /* swapchain Present on the calling thread */
    WINEPS5_HITCH_OTHER,           /* other instrumented, non-per-draw D3D12 calls */
    WINEPS5_HITCH_BUCKET_COUNT
};

/* Running per-thread vkd3d totals. Bucket time is outermost-scope time minus
 * the Wine wait/read/unwind time nested in it, so buckets and Wine totals
 * never overlap. */
struct wineps5_hitch_thread
{
    uint64_t bucket_ns[WINEPS5_HITCH_BUCKET_COUNT];
    uint64_t bucket_calls[WINEPS5_HITCH_BUCKET_COUNT];
    uint64_t vkd3d_wait_ns;   /* Wine blocked time nested in any vkd3d scope */
    uint64_t present_wait_ns; /* ... of which inside Present */
    uint64_t vk_alloc_ns, vk_alloc_calls; /* Vulkan alloc/bind/create image+buffer, inside buckets */
    uint64_t pipeline_ns, pipeline_calls; /* vkCreate*Pipelines, inside buckets */
};

/* The outermost vkd3d scope a thread is in (start_ns 0: none). */
struct wineps5_hitch_scope_state
{
    uint64_t start_ns;
    uint64_t nested_ns; /* Wine wait+read+unwind at scope start */
    uint64_t wait_ns;   /* Wine wait at scope start */
    uint32_t bucket;
};

static inline void wineps5_hitch_scope_open(struct wineps5_hitch_scope_state *s, unsigned int bucket,
        uint64_t now_ns, const struct wineps5_hitch_wine_totals *wine)
{
    s->start_ns = now_ns;
    s->nested_ns = wineps5_hitch_wine_nested(wine);
    s->wait_ns = wine->wait_ns;
    s->bucket = bucket;
}

/* Exclusive time and nested wait of the open scope as of now_ns. */
static inline void wineps5_hitch_scope_progress(const struct wineps5_hitch_scope_state *s,
        uint64_t now_ns, const struct wineps5_hitch_wine_totals *wine, uint64_t *exclusive_ns, uint64_t *waited_ns)
{
    uint64_t elapsed = now_ns > s->start_ns ? now_ns - s->start_ns : 0;
    uint64_t nested = wineps5_hitch_wine_nested(wine);

    nested = nested > s->nested_ns ? nested - s->nested_ns : 0;
    *exclusive_ns = elapsed > nested ? elapsed - nested : 0;
    *waited_ns = wine->wait_ns > s->wait_ns ? wine->wait_ns - s->wait_ns : 0;
}

static inline void wineps5_hitch_scope_close(struct wineps5_hitch_thread *t, struct wineps5_hitch_scope_state *s,
        uint64_t now_ns, const struct wineps5_hitch_wine_totals *wine)
{
    uint64_t exclusive, waited;

    if (!s->start_ns || s->bucket >= WINEPS5_HITCH_BUCKET_COUNT)
    {
        s->start_ns = 0;
        return;
    }
    wineps5_hitch_scope_progress(s, now_ns, wine, &exclusive, &waited);
    t->bucket_ns[s->bucket] += exclusive;
    t->bucket_calls[s->bucket]++;
    t->vkd3d_wait_ns += waited;
    if (s->bucket == WINEPS5_HITCH_PRESENT)
        t->present_wait_ns += waited;
    s->start_ns = 0;
}

struct wineps5_hitch_snapshot
{
    struct wineps5_hitch_thread vkd3d;
    struct wineps5_hitch_wine_totals wine;
};

/* A thread's totals as of now_ns, including the open scope's progress. */
static inline void wineps5_hitch_effective(const struct wineps5_hitch_thread *totals,
        const struct wineps5_hitch_scope_state *scope, const struct wineps5_hitch_wine_totals *wine,
        uint64_t now_ns, struct wineps5_hitch_snapshot *out)
{
    uint64_t exclusive, waited;

    out->vkd3d = *totals;
    out->wine = *wine;
    if (!scope || !scope->start_ns || scope->bucket >= WINEPS5_HITCH_BUCKET_COUNT)
        return;
    wineps5_hitch_scope_progress(scope, now_ns, wine, &exclusive, &waited);
    out->vkd3d.bucket_ns[scope->bucket] += exclusive;
    out->vkd3d.vkd3d_wait_ns += waited;
    if (scope->bucket == WINEPS5_HITCH_PRESENT)
        out->vkd3d.present_wait_ns += waited;
}

static inline uint64_t wineps5_hitch_sub(uint64_t after, uint64_t before)
{
    return after > before ? after - before : 0;
}

/* Field-wise after - before, floored at 0 (a read still in progress inside a
 * vkd3d call can briefly count as vkd3d time). */
static inline void wineps5_hitch_snapshot_delta(const struct wineps5_hitch_snapshot *before,
        const struct wineps5_hitch_snapshot *after, struct wineps5_hitch_snapshot *out)
{
    unsigned int i;

    for (i = 0; i < WINEPS5_HITCH_BUCKET_COUNT; i++)
    {
        out->vkd3d.bucket_ns[i] = wineps5_hitch_sub(after->vkd3d.bucket_ns[i], before->vkd3d.bucket_ns[i]);
        out->vkd3d.bucket_calls[i] = wineps5_hitch_sub(after->vkd3d.bucket_calls[i], before->vkd3d.bucket_calls[i]);
    }
    out->vkd3d.vkd3d_wait_ns = wineps5_hitch_sub(after->vkd3d.vkd3d_wait_ns, before->vkd3d.vkd3d_wait_ns);
    out->vkd3d.present_wait_ns = wineps5_hitch_sub(after->vkd3d.present_wait_ns, before->vkd3d.present_wait_ns);
    out->vkd3d.vk_alloc_ns = wineps5_hitch_sub(after->vkd3d.vk_alloc_ns, before->vkd3d.vk_alloc_ns);
    out->vkd3d.vk_alloc_calls = wineps5_hitch_sub(after->vkd3d.vk_alloc_calls, before->vkd3d.vk_alloc_calls);
    out->vkd3d.pipeline_ns = wineps5_hitch_sub(after->vkd3d.pipeline_ns, before->vkd3d.pipeline_ns);
    out->vkd3d.pipeline_calls = wineps5_hitch_sub(after->vkd3d.pipeline_calls, before->vkd3d.pipeline_calls);
    out->wine.wait_ns = wineps5_hitch_sub(after->wine.wait_ns, before->wine.wait_ns);
    out->wine.wait_count = wineps5_hitch_sub(after->wine.wait_count, before->wine.wait_count);
    out->wine.read_ns = wineps5_hitch_sub(after->wine.read_ns, before->wine.read_ns);
    out->wine.read_count = wineps5_hitch_sub(after->wine.read_count, before->wine.read_count);
    out->wine.unwind_ns = wineps5_hitch_sub(after->wine.unwind_ns, before->wine.unwind_ns);
    out->wine.unwind_count = wineps5_hitch_sub(after->wine.unwind_count, before->wine.unwind_count);
    out->wine.cpu_samples = wineps5_hitch_sub(after->wine.cpu_samples, before->wine.cpu_samples);
}

/* Per-thread state carried from one global Present to the next. */
struct wineps5_hitch_track
{
    struct wineps5_hitch_snapshot prev;
    struct wineps5_hitch_cpu_bound bound; /* on cpu(prev_ns), refined by later samples */
    uint64_t prev_ns;
    bool valid;
};

/* Advance to now_ns; on true, delta and span_ns cover (prev_ns, now_ns]. A thread
 * first seen mid-interval only covers the part since it was first seen.
 * prev_bound receives the bound on cpu(prev_ns) refined with this read's CPU
 * sample; tr->bound then holds the (unrefined) bound on cpu(now_ns), which the
 * next step refines in turn. */
static inline bool wineps5_hitch_track_step(struct wineps5_hitch_track *tr,
        const struct wineps5_hitch_snapshot *now_snap, const struct wineps5_hitch_cpu_sample *cpu,
        uint64_t now_ns, struct wineps5_hitch_snapshot *delta, uint64_t *span_ns,
        struct wineps5_hitch_cpu_bound *prev_bound)
{
    bool had = tr->valid && now_ns >= tr->prev_ns;

    if (tr->valid)
        wineps5_hitch_cpu_refine(&tr->bound, cpu, tr->prev_ns);
    *prev_bound = tr->bound;
    if (had)
    {
        wineps5_hitch_snapshot_delta(&tr->prev, now_snap, delta);
        *span_ns = now_ns - tr->prev_ns;
    }
    tr->prev = *now_snap;
    tr->prev_ns = now_ns;
    tr->valid = true;
    tr->bound.valid = false;
    wineps5_hitch_cpu_refine(&tr->bound, cpu, now_ns);
    return had;
}

struct wineps5_hitch_split
{
    uint64_t vkd3d_ns;    /* sum of exclusive buckets */
    uint64_t measured_ns; /* vkd3d + Wine wait/read/unwind */
    uint64_t busy_ns;     /* span - wait */
    uint64_t other_ns;    /* span - measured, floored */
    uint64_t overlap_ns;  /* measured - span when it exceeds the span; expected ~0 */
};

static inline void wineps5_hitch_split(uint64_t span_ns, const struct wineps5_hitch_snapshot *d,
        struct wineps5_hitch_split *out)
{
    unsigned int i;

    out->vkd3d_ns = 0;
    for (i = 0; i < WINEPS5_HITCH_BUCKET_COUNT; i++)
        out->vkd3d_ns += d->vkd3d.bucket_ns[i];
    out->measured_ns = out->vkd3d_ns + d->wine.wait_ns + d->wine.read_ns + d->wine.unwind_ns;
    out->busy_ns = wineps5_hitch_sub(span_ns, d->wine.wait_ns);
    out->other_ns = wineps5_hitch_sub(span_ns, out->measured_ns);
    out->overlap_ns = wineps5_hitch_sub(out->measured_ns, span_ns);
}

/* Index of the candidate with the largest busy time, skipping the excluded
 * indices and candidates without a full delta; -1 if none. */
static inline int wineps5_hitch_pick_busiest(const uint64_t *busy_ns, const bool *eligible,
        unsigned int count, int exclude_a, int exclude_b)
{
    int best = -1;
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        if (!eligible[i] || (int)i == exclude_a || (int)i == exclude_b)
            continue;
        if (best < 0 || busy_ns[i] > busy_ns[best])
            best = i;
    }
    return best;
}

/* Global frame clock: any Present on any thread ends an interval. */
struct wineps5_hitch_clock
{
    uint64_t last_ns, seq;
};

static inline uint64_t wineps5_hitch_clock_step(struct wineps5_hitch_clock *c, uint64_t now_ns)
{
    uint64_t frame_ns = c->last_ns && now_ns > c->last_ns ? now_ns - c->last_ns : 0;

    c->last_ns = now_ns;
    c->seq++;
    return frame_ns;
}

static inline bool wineps5_hitch_is_hitch(uint64_t frame_ns)
{
    return frame_ns > WINEPS5_HITCH_THRESHOLD_NS;
}

/* GPU work observed during one frame interval (any thread). Latency is the
 * CPU-observed first-submit-to-completion time of a batch, as in the census. */
struct wineps5_hitch_gpu
{
    uint64_t max_ns[3];        /* DIRECT, COMPUTE, COPY */
    uint64_t completed[3];     /* submissions completed */
    uint64_t copy_bytes;       /* recorded copy bytes of COPY lists executed */
    uint64_t vk_submit_ns, vk_submit_max_ns, vk_submits; /* vkQueueSubmit2 on queue threads */
};

static inline void wineps5_hitch_gpu_complete(struct wineps5_hitch_gpu *g, unsigned int queue,
        uint64_t latency_ns, uint64_t submissions)
{
    if (queue >= 3)
        return;
    if (latency_ns > g->max_ns[queue])
        g->max_ns[queue] = latency_ns;
    g->completed[queue] += submissions;
}

static inline void wineps5_hitch_gpu_submit(struct wineps5_hitch_gpu *g, uint64_t duration_ns)
{
    g->vk_submit_ns += duration_ns;
    if (duration_ns > g->vk_submit_max_ns)
        g->vk_submit_max_ns = duration_ns;
    g->vk_submits++;
}

/* At most WINEPS5_HITCH_MAX_LINES hitch events per 10-s window; the rest are counted. */
struct wineps5_hitch_limiter
{
    uint64_t window_start_ns, frames, hitches, logged, suppressed, max_frame_ns;
};

/* Close the window if 10 s have passed; *closed receives its counts. */
static inline bool wineps5_hitch_window_roll(struct wineps5_hitch_limiter *l, uint64_t now_ns,
        struct wineps5_hitch_limiter *closed)
{
    if (!l->window_start_ns || now_ns < l->window_start_ns)
    {
        l->window_start_ns = now_ns;
        return false;
    }
    if (now_ns - l->window_start_ns < WINEPS5_HITCH_WINDOW_NS)
        return false;
    *closed = *l;
    l->window_start_ns = now_ns;
    l->frames = l->hitches = l->logged = l->suppressed = l->max_frame_ns = 0;
    return true;
}

/* Count one frame; true when it is a hitch that may be logged. */
static inline bool wineps5_hitch_admit(struct wineps5_hitch_limiter *l, uint64_t frame_ns)
{
    l->frames++;
    if (frame_ns > l->max_frame_ns)
        l->max_frame_ns = frame_ns;
    if (!wineps5_hitch_is_hitch(frame_ns))
        return false;
    l->hitches++;
    if (l->logged < WINEPS5_HITCH_MAX_LINES)
    {
        l->logged++;
        return true;
    }
    l->suppressed++;
    return false;
}

#endif
