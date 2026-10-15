#ifndef WINEPS5_PSO_H
#define WINEPS5_PSO_H

#include <stdbool.h>
#include <stdint.h>

struct wineps5_pso_stats
{
    uint64_t created, total_ns, max_ns, draw_time, cache_hits, cache_misses;
    uint64_t spikes_with_compile;
};

static inline void wineps5_pso_note(struct wineps5_pso_stats *s,
        uint64_t duration_ns, bool success, bool draw_time, bool disk_hit)
{
    s->total_ns += duration_ns;
    if (duration_ns > s->max_ns) s->max_ns = duration_ns;
    if (!success) return;
    ++s->created;
    s->draw_time += draw_time;
    s->cache_hits += disk_hit;
    s->cache_misses += !disk_hit;
}

/* Profile-only detail, separate from the always-on creation census. "Hit"
 * means the vkd3d SPIR-V/identifier blob, never a RADV disk-cache hit. */
#define WINEPS5_PSO_SLOW_NS UINT64_C(50000000)
#define WINEPS5_PSO_MAX_DETAILS 16
enum wineps5_pso_library_op
{
    WINEPS5_PSO_LIBRARY_CREATE,
    WINEPS5_PSO_LIBRARY_STORE,
    WINEPS5_PSO_LIBRARY_LOAD,
    WINEPS5_PSO_LIBRARY_SERIALIZE,
    WINEPS5_PSO_LIBRARY_OP_COUNT
};
struct wineps5_pso_profile
{
    uint64_t hit_ns, hit_max_ns, miss_ns, miss_max_ns;
    uint64_t slow, slow_hits, failed;
    uint64_t shaders, shader_ns, shader_max_ns;
    uint64_t feedback_valid, feedback_app_hits, driver_ns, driver_stage_ns;
    uint64_t details, suppressed;
    uint64_t library_ok[WINEPS5_PSO_LIBRARY_OP_COUNT], library_failed[WINEPS5_PSO_LIBRARY_OP_COUNT];
    uint32_t library_last_error[WINEPS5_PSO_LIBRARY_OP_COUNT];
};

static inline void wineps5_pso_profile_note(struct wineps5_pso_profile *s,
        uint64_t ns, bool success, bool disk_hit)
{
    if (!success)
    {
        ++s->failed;
        return;
    }
    if (disk_hit)
    {
        s->hit_ns += ns;
        if (ns > s->hit_max_ns) s->hit_max_ns = ns;
    }
    else
    {
        s->miss_ns += ns;
        if (ns > s->miss_max_ns) s->miss_max_ns = ns;
    }
    if (ns >= WINEPS5_PSO_SLOW_NS)
    {
        ++s->slow;
        s->slow_hits += disk_hit;
    }
}

static inline bool wineps5_pso_profile_detail(struct wineps5_pso_profile *s, uint64_t ns)
{
    if (ns < WINEPS5_PSO_SLOW_NS) return false;
    if (s->details < WINEPS5_PSO_MAX_DETAILS)
    {
        ++s->details;
        return true;
    }
    ++s->suppressed;
    return false;
}

static inline bool wineps5_pso_spike(uint64_t previous_ns, uint64_t now_ns,
        uint64_t compile_ns)
{
    return previous_ns && now_ns > previous_ns &&
            now_ns - previous_ns > UINT64_C(50000000) &&
            compile_ns > previous_ns && compile_ns <= now_ns;
}

#endif
