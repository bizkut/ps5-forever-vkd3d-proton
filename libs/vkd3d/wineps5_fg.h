/* SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
 * SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported from Eden 67bada77: frame_gen/{lossless_dll,lsfg_translate}.cpp,
 * renderer_vulkan/present/{lsfg_common.h,frame_gen_pacer.cpp}.
 * Pure helpers: no Vulkan, Windows, allocation or proprietary shader bytes. */
#ifndef WINEPS5_FG_H
#define WINEPS5_FG_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* AUTO: Lossless when the user's C:\Lossless.dll is usable, else FSR3, which
 * needs no file; the engine reports the method it runs. */
enum wineps5_fg_method { WINEPS5_FG_METHOD_LOSSLESS, WINEPS5_FG_METHOD_FSR3, WINEPS5_FG_METHOD_AUTO };
enum wineps5_fg_mode { WINEPS5_FG_MODE_BETWEEN, WINEPS5_FG_MODE_AHEAD };
static inline const char *wineps5_fg_method_name(enum wineps5_fg_method method)
{ return method == WINEPS5_FG_METHOD_FSR3 ? "fsr3" : method == WINEPS5_FG_METHOD_AUTO ? "auto" : "lossless"; }
static inline const char *wineps5_fg_mode_name(enum wineps5_fg_mode mode)
{ return mode == WINEPS5_FG_MODE_AHEAD ? "ahead" : "between"; }
/* Neither the Eden LS shaders nor this ps5-framegen revision extrapolates.
 * Swan's optical-flow driver clamps phase at 1; its separate own algorithm
 * (not ps5-framegen) is the method which supports ahead. */
static inline bool wineps5_fg_mode_supported(enum wineps5_fg_method method, enum wineps5_fg_mode mode)
{
    (void)method;
    return mode == WINEPS5_FG_MODE_BETWEEN;
}
struct wineps5_fg_settings
{
    bool enabled;
    unsigned int mult;
    float flow;
    bool automatic;
    float on_below, off_above;
    enum wineps5_fg_method method;
    enum wineps5_fg_mode mode;
};
static inline bool wineps5_fg_reuse_engine(const struct wineps5_fg_settings *old,
        const struct wineps5_fg_settings *next)
{
    return (next->enabled || next->automatic) && old->mult == next->mult &&
            old->flow == next->flow && old->method == next->method && old->mode == next->mode;
}
static inline struct wineps5_fg_settings wineps5_fg_defaults(void)
{
    return (struct wineps5_fg_settings){false,2,1.0f,false,70.0f,80.0f,
            WINEPS5_FG_METHOD_AUTO,WINEPS5_FG_MODE_BETWEEN};
}
static inline bool wineps5_fg_parse(char *text, struct wineps5_fg_settings *s)
{
    char *p = text, *key, *value, *end;
    unsigned int seen = 0, bit;
    *s = wineps5_fg_defaults();
    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        key = p;
        while (*p && *p != '=') ++p;
        if (!*p) return false;
        *p++ = 0; value = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        if (*p) *p++ = 0;
        if (!strcmp(key, "fg"))
        {
            bit = 1;
            if (!strcmp(value, "on")) s->enabled = true;
            else if (!strcmp(value, "auto")) s->automatic = true;
            else if (strcmp(value, "off")) return false;
        }
        else if (!strcmp(key, "mult"))
        {
            bit = 2;
            if (value[0] < '2' || value[0] > '4' || value[1]) return false;
            s->mult = value[0] - '0';
        }
        else if (!strcmp(key, "flow"))
        {
            bit = 4;
            s->flow = strtof(value, &end);
            if (end == value || *end || !isfinite(s->flow) || s->flow < 0.25f || s->flow > 1.0f) return false;
        }
        else if (!strcmp(key, "on_below") || !strcmp(key, "off_above"))
        {
            float fps = strtof(value, &end);
            if (end == value || *end || !isfinite(fps) || fps <= 0) return false;
            bit = !strcmp(key, "on_below") ? 8 : 16;
            if (bit == 8) s->on_below = fps;
            else s->off_above = fps;
        }
        else if (!strcmp(key, "method"))
        {
            bit = 32;
            if (!strcmp(value, "lossless")) s->method = WINEPS5_FG_METHOD_LOSSLESS;
            else if (!strcmp(value, "fsr3")) s->method = WINEPS5_FG_METHOD_FSR3;
            else if (!strcmp(value, "auto")) s->method = WINEPS5_FG_METHOD_AUTO;
            else return false;
        }
        else if (!strcmp(key, "mode"))
        {
            bit = 64;
            if (!strcmp(value, "between")) s->mode = WINEPS5_FG_MODE_BETWEEN;
            else if (!strcmp(value, "ahead")) s->mode = WINEPS5_FG_MODE_AHEAD;
            else return false;
        }
        else return false;
        if (seen & bit) return false;
        seen |= bit;
    }
    return (seen & 1) != 0 && s->on_below < s->off_above;
}
static inline unsigned int wineps5_fg_shader_id(unsigned int index)
{
    return index < 2 ? 255 + index : index < 25 ? 278 + index : 0;
}
static inline unsigned int wineps5_fg_slot(unsigned int count, unsigned int generation)
{
    return (count - 1) * count / 2 + generation;
}
static inline float wineps5_fg_timestamp(unsigned int slot)
{
    unsigned int count = 1;
    while (count < 3 && wineps5_fg_slot(count + 1, 0) <= slot) ++count;
    return (float)(slot - wineps5_fg_slot(count, 0) + 1) / (count + 1);
}
/* Eden's stable (set,binding) ordering, using bounded stack metadata. The native
 * modules retain their descriptor sets (all set 0); no DXBC translation. */
static inline bool wineps5_fg_adopt(uint32_t *w, size_t n)
{
    struct { uint32_t id, set, binding; size_t literal; } slots[32], t;
    size_t p, q, len, count = 0, i, j;
    uint32_t op;
    if (n < 5 || w[0] != UINT32_C(0x07230203)) return false;
    for (p = 5; p < n; p += len)
    {
        len = w[p] >> 16; op = w[p] & 65535;
        if (!len || len > n - p) return false;
        if (op == 71 && len >= 4 && w[p + 2] == 33)
        {
            if (count == 32) return false;
            slots[count].id = w[p + 1]; slots[count].binding = w[p + 3];
            slots[count].set = 0; slots[count++].literal = p + 3;
        }
    }
    for (p = 5; p < n; p += len)
    {
        len = w[p] >> 16;
        if ((w[p] & 65535) == 54) break;
        if ((w[p] & 65535) == 71 && len >= 4 && w[p + 2] == 34)
            for (i = 0; i < count; ++i) if (slots[i].id == w[p + 1]) slots[i].set = w[p + 3];
    }
    for (i = 1; i < count; ++i)
    {
        t = slots[i]; j = i;
        while (j && (slots[j - 1].set > t.set || (slots[j - 1].set == t.set && slots[j - 1].binding > t.binding)))
        { slots[j] = slots[j - 1]; --j; }
        slots[j] = t;
    }
    for (q = 0; q < count; ++q) w[slots[q].literal] = (uint32_t)q;
    return true;
}
struct wineps5_fg_blob { const unsigned char *data; size_t size; };
static inline bool wineps5_fg_u16(struct wineps5_fg_blob b, size_t p, uint16_t *v)
{
    if (p > b.size || b.size - p < 2) return false;
    *v = (uint16_t)b.data[p] | (uint16_t)b.data[p + 1] << 8; return true;
}
static inline bool wineps5_fg_u32(struct wineps5_fg_blob b, size_t p, uint32_t *v)
{
    if (p > b.size || b.size - p < 4) return false;
    *v = (uint32_t)b.data[p] | (uint32_t)b.data[p + 1] << 8 |
            (uint32_t)b.data[p + 2] << 16 | (uint32_t)b.data[p + 3] << 24; return true;
}
static inline bool wineps5_fg_rva(struct wineps5_fg_blob b, size_t table, unsigned int sections, uint32_t rva, size_t size, size_t *offset)
{
    unsigned int i;
    uint32_t va, raw, raw_size;
    size_t p;
    for (i = 0; i < sections; ++i)
    {
        p = table + i * 40;
        if (!wineps5_fg_u32(b, p + 12, &va) || !wineps5_fg_u32(b, p + 16, &raw_size) || !wineps5_fg_u32(b, p + 20, &raw)) return false;
        if (rva < va || rva - va > raw_size || size > raw_size - (rva - va)) continue;
        p = (size_t)raw + (rva - va);
        if (p > b.size || size > b.size - p) return false;
        *offset = p; return true;
    }
    return false;
}
static inline bool wineps5_fg_resource_entry(struct wineps5_fg_blob resource, size_t directory, uint32_t wanted, bool first, uint32_t *out)
{
    uint16_t named, ids;
    uint32_t name, value;
    unsigned int i;
    if (!wineps5_fg_u16(resource, directory + 12, &named) || !wineps5_fg_u16(resource, directory + 14, &ids)) return false;
    for (i = 0; i < (unsigned int)named + ids; ++i)
    {
        if (!wineps5_fg_u32(resource, directory + 16 + i * 8, &name) ||
                !wineps5_fg_u32(resource, directory + 20 + i * 8, &value)) return false;
        if (!(name & UINT32_C(0x80000000)) && (first || name == wanted)) { *out = value; return true; }
    }
    return false;
}
/* Find native RCDATA id +49 without loading executable code from Lossless.dll. */
static inline bool wineps5_fg_resource(struct wineps5_fg_blob b, unsigned int id, struct wineps5_fg_blob *out)
{
    uint16_t magic, sections, optional_size;
    uint32_t pe, signature, rva, bytes, type, name, leaf;
    size_t optional, table, base, offset;
    struct wineps5_fg_blob resource;
    if (!wineps5_fg_u16(b, 0, &magic) || magic != 0x5a4d || !wineps5_fg_u32(b, 60, &pe) ||
            !wineps5_fg_u32(b, pe, &signature) || signature != 0x4550 ||
            !wineps5_fg_u16(b, (size_t)pe + 6, &sections) || !wineps5_fg_u16(b, (size_t)pe + 20, &optional_size)) return false;
    optional = (size_t)pe + 24; table = optional + optional_size;
    if (!wineps5_fg_u16(b, optional, &magic) || (magic != 0x10b && magic != 0x20b)) return false;
    offset = optional + (magic == 0x10b ? 96 : 112) + 16;
    if (offset + 8 > table || !wineps5_fg_u32(b, offset, &rva) || !wineps5_fg_u32(b, offset + 4, &bytes) ||
            !bytes || !wineps5_fg_rva(b, table, sections, rva, bytes, &base)) return false;
    resource = (struct wineps5_fg_blob){b.data + base, bytes};
    if (!wineps5_fg_resource_entry(resource, 0, 10, false, &type) || !(type & UINT32_C(0x80000000)) ||
            !wineps5_fg_resource_entry(resource, type & UINT32_C(0x7fffffff), id + 49, false, &name) || !(name & UINT32_C(0x80000000)) ||
            !wineps5_fg_resource_entry(resource, name & UINT32_C(0x7fffffff), 0, true, &leaf) || (leaf & UINT32_C(0x80000000)) ||
            !wineps5_fg_u32(resource, leaf, &rva) || !wineps5_fg_u32(resource, (size_t)leaf + 4, &bytes) ||
            !wineps5_fg_rva(b, table, sections, rva, bytes, &offset)) return false;
    *out = (struct wineps5_fg_blob){b.data + offset, bytes}; return true;
}
struct wineps5_fg_pacer { uint64_t last, stable_until; double interval, credit; };
/* Eden smoothing, burst rejection, 10 Hz floor, one-second stabilization and
 * output-credit refresh cap. Fixed user multiplier replaces auto-probing. */
static inline unsigned int wineps5_fg_plan(struct wineps5_fg_pacer *p, uint64_t now, unsigned int mult)
{
    double dt, desired;
    unsigned int outputs, generated;
    if (!p->last) { p->last = now; p->stable_until = now + UINT64_C(1000000000); return 0; }
    dt = (now - p->last) * 1e-9; p->last = now;
    if (dt <= 0 || dt > 0.1)
    { p->interval = p->credit = 0; p->stable_until = now + UINT64_C(1000000000); return 0; }
    if (p->interval > 0 && 1.0 / dt > fmax(3.0 / p->interval, 239.76)) { p->credit = 0; return 0; }
    p->interval = p->interval > 0 ? p->interval + (dt - p->interval) * 0.25 : dt;
    if (now < p->stable_until) return 0;
    desired = p->interval * 119.88;
    if (desired <= 1) { p->credit = 0; return 0; }
    p->credit += desired;
    outputs = (unsigned int)floor(p->credit + 1e-4);
    generated = outputs > 1 ? outputs - 1 : 0;
    if (generated > mult - 1) generated = mult - 1;
    p->credit = fmax(0, p->credit - generated - 1);
    if (generated == mult - 1 && p->credit >= 1) p->credit = fmod(p->credit, 1);
    return generated;
}
struct wineps5_fg_schedule { uint64_t first, real, step; };
/* The newest pair must be GPU-ready before scheduling its first interpolation.
 * Real N follows the generated outputs, with exactly one step of hold per
 * generated frame, not an already-expired CPU capture+interval deadline. */
static inline struct wineps5_fg_schedule wineps5_fg_pair_schedule(uint64_t ready,
        uint64_t interval, unsigned int generations)
{
    struct wineps5_fg_schedule s;
    s.step = interval / (generations + 1);
    if (s.step < UINT64_C(8341675)) s.step = UINT64_C(8341675);
    s.first = ready; /* No cross-pair alignment delay before first generated. */
    s.real = s.first + generations * s.step;
    return s;
}
/* One look-ahead job, never a queue of prepared stale frames. */
static inline bool wineps5_fg_prefetch_eligible(bool pending, bool prepared, bool work_pending, bool stopping)
{
    return pending && !prepared && !work_pending && !stopping;
}
/* Driver submission cannot be preempted. Use measured recording/submission
 * cost to reject a predicted held-real overrun beyond one millisecond. */
static inline bool wineps5_fg_prefetch_fits(uint64_t now, uint64_t deadline, uint64_t cost)
{
    return now < deadline && cost <= deadline - now + UINT64_C(1000000);
}
struct wineps5_fg_block_stats { uint64_t count, total_ns, max_ns; };
static inline void wineps5_fg_block_note(struct wineps5_fg_block_stats *s, uint64_t ns)
{
    ++s->count; s->total_ns += ns;
    if (ns > s->max_ns) s->max_ns = ns;
}
static inline double wineps5_fg_block_mean_ms(const struct wineps5_fg_block_stats *s)
{
    return s->count ? s->total_ns * 1e-6 / s->count : 0;
}
/* Three completed game-Present intervals, each approximately one second.
 * These timestamps come from the game API, never generated/output frames. */
struct wineps5_fg_auto
{
    uint64_t bucket_start_ns, next_eval_ns, last_change_ns;
    unsigned int frames, count, cursor;
    double rates[3], game_fps;
    bool enabled;
};
static inline void wineps5_fg_auto_note(struct wineps5_fg_auto *a, uint64_t now)
{
    uint64_t elapsed;
    unsigned int i;
    if (!a->bucket_start_ns) { a->bucket_start_ns = now; return; }
    if (now <= a->bucket_start_ns) return;
    ++a->frames;
    elapsed = now - a->bucket_start_ns;
    if (elapsed < UINT64_C(1000000000)) return;
    a->rates[a->cursor] = a->frames * 1e9 / elapsed;
    a->cursor = (a->cursor + 1) % 3;
    if (a->count < 3) ++a->count;
    a->game_fps = 0;
    for (i = 0; i < a->count; ++i) a->game_fps += a->rates[i];
    a->game_fps /= a->count;
    a->bucket_start_ns = now; a->frames = 0;
}
static inline bool wineps5_fg_auto_decide(struct wineps5_fg_auto *a,
        const struct wineps5_fg_settings *s, uint64_t now)
{
    bool enabled;
    if (now < a->next_eval_ns) return false;
    a->next_eval_ns = now + UINT64_C(1000000000);
    if (a->count < 3 || (a->last_change_ns && now - a->last_change_ns < UINT64_C(5000000000))) return false;
    enabled = a->enabled;
    if (!enabled && a->game_fps < s->on_below) enabled = true;
    else if (enabled && a->game_fps > s->off_above) enabled = false;
    if (enabled == a->enabled) return false;
    a->enabled = enabled; a->last_change_ns = now;
    return true;
}

/* Fixed 0.25 ms histogram, with an overflow bucket at 512 ms. Mean/max are
 * exact; percentile upper edges are conservative, capped at exact max.
 * The histogram lives in the common GPU census, active in both FG modes. */
#define WINEPS5_PRESENT_LATENCY_BUCKETS 2049
struct wineps5_present_latency
{
    uint32_t bins[WINEPS5_PRESENT_LATENCY_BUCKETS];
    uint64_t count, total_ns, max_ns;
};
static inline void wineps5_present_latency_note(struct wineps5_present_latency *h, uint64_t ns)
{
    uint64_t bin = ns / UINT64_C(250000);
    if (bin >= WINEPS5_PRESENT_LATENCY_BUCKETS) bin = WINEPS5_PRESENT_LATENCY_BUCKETS - 1;
    ++h->bins[bin]; ++h->count; h->total_ns += ns;
    if (ns > h->max_ns) h->max_ns = ns;
}
static inline double wineps5_present_latency_mean_ms(const struct wineps5_present_latency *h)
{
    return h->count ? h->total_ns * 1e-6 / h->count : 0;
}
static inline double wineps5_present_latency_percentile_ms(const struct wineps5_present_latency *h, unsigned int percent)
{
    uint64_t rank, cumulative = 0, upper;
    unsigned int i;
    if (!h->count) return 0;
    rank = (h->count * percent + 99) / 100;
    if (!rank) rank = 1;
    for (i = 0; i < WINEPS5_PRESENT_LATENCY_BUCKETS; ++i)
    {
        cumulative += h->bins[i];
        if (cumulative < rank) continue;
        upper = i == WINEPS5_PRESENT_LATENCY_BUCKETS - 1 ? h->max_ns : (uint64_t)(i + 1) * UINT64_C(250000);
        if (upper > h->max_ns) upper = h->max_ns;
        return upper * 1e-6;
    }
    return h->max_ns * 1e-6;
}
/* Real WSI present correlations only, bounded independently of game latency.
 * A full ring omits a measurement rather than blocking presentation. */
#define WINEPS5_DISPLAY_LATENCY_CAPACITY 64
struct wineps5_display_latency_entry { uint64_t id, present_ns; bool fg; };
struct wineps5_display_latency_ring
{
    struct wineps5_display_latency_entry entries[WINEPS5_DISPLAY_LATENCY_CAPACITY];
    unsigned int head, count;
};
static inline bool wineps5_display_latency_push(struct wineps5_display_latency_ring *r,
        uint64_t id, uint64_t present_ns, bool fg)
{
    unsigned int slot;
    if (!id || !present_ns || r->count == WINEPS5_DISPLAY_LATENCY_CAPACITY) return false;
    slot = (r->head + r->count) % WINEPS5_DISPLAY_LATENCY_CAPACITY;
    r->entries[slot] = (struct wineps5_display_latency_entry){id,present_ns,fg};
    ++r->count;
    return true;
}
static inline void wineps5_display_latency_pop(struct wineps5_display_latency_ring *r)
{
    if (!r->count) return;
    r->head = (r->head + 1) % WINEPS5_DISPLAY_LATENCY_CAPACITY;
    --r->count;
}
static inline bool wineps5_display_latency_interval(uint64_t present_ns, uint64_t completion_ns,
        uint64_t *interval_ns)
{
    if (!present_ns || completion_ns < present_ns) return false;
    *interval_ns = completion_ns - present_ns;
    return true;
}
enum wineps5_fg_stage
{
    WINEPS5_FG_STAGE_CAPTURE_SUBMIT,
    WINEPS5_FG_STAGE_COPY_COMPLETE,
    WINEPS5_FG_STAGE_WORKER_PICKUP,
    WINEPS5_FG_STAGE_SHARED_COMPLETE,
    WINEPS5_FG_STAGE_FIRST_GENERATED,
    WINEPS5_FG_STAGE_REAL_PRESENT,
    WINEPS5_FG_STAGE_DISPLAY_COMPLETE,
    WINEPS5_FG_STAGE_COUNT,
};
struct wineps5_fg_stage_stats { struct wineps5_fg_block_stats stages[WINEPS5_FG_STAGE_COUNT]; };
static inline void wineps5_fg_stage_note(struct wineps5_fg_stage_stats *s, unsigned int stage,
        uint64_t present_ns, uint64_t event_ns)
{
    uint64_t interval_ns;
    if (stage >= WINEPS5_FG_STAGE_COUNT ||
            !wineps5_display_latency_interval(present_ns,event_ns,&interval_ns)) return;
    wineps5_fg_block_note(&s->stages[stage],interval_ns);
}
#endif
