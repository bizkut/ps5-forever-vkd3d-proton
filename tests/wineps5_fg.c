/* SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
 * SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Host regression tests for the Eden 67bada77 C helper port. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../libs/vkd3d/wineps5_fg.h"
#include "../libs/vkd3d/wineps5_hud.h"
#include "../libs/vkd3d/wineps5_pso.h"
#include "../libs/vkd3d/wineps5_hitch.h"
static void put16(unsigned char *b, unsigned int p, uint16_t v)
{ b[p] = v; b[p + 1] = v >> 8; }
static void put32(unsigned char *b, unsigned int p, uint32_t v)
{ b[p] = v; b[p + 1] = v >> 8; b[p + 2] = v >> 16; b[p + 3] = v >> 24; }
static bool parse(const char *text, struct wineps5_fg_settings *s)
{ char buffer[128]; strcpy(buffer,text); return wineps5_fg_parse(buffer,s); }
static void test_config(void)
{
    struct wineps5_fg_settings s;
    assert(parse("fg=off",&s) && !s.enabled && s.mult == 2 && s.flow == 1);
    assert(s.method == WINEPS5_FG_METHOD_AUTO && s.mode == WINEPS5_FG_MODE_BETWEEN); /* Lossless, else FSR3 */
    assert(parse("fg=on method=auto",&s) && s.method == WINEPS5_FG_METHOD_AUTO);
    assert(parse("fg=on method=fsr3 mode=between mult=4",&s) &&
            s.method == WINEPS5_FG_METHOD_FSR3 && s.mode == WINEPS5_FG_MODE_BETWEEN && s.mult == 4);
    assert(parse("fg=auto mode=ahead method=fsr3 flow=0.5",&s) &&
            s.automatic && s.method == WINEPS5_FG_METHOD_FSR3 && s.mode == WINEPS5_FG_MODE_AHEAD);
    assert(parse("method=lossless fg=on mode=ahead",&s) &&
            s.method == WINEPS5_FG_METHOD_LOSSLESS && s.mode == WINEPS5_FG_MODE_AHEAD);
    assert(parse("fg=off method=fsr3",&s) && !s.enabled && !s.automatic);
    assert(!strcmp(wineps5_fg_method_name(WINEPS5_FG_METHOD_FSR3),"fsr3"));
    assert(!strcmp(wineps5_fg_method_name(WINEPS5_FG_METHOD_LOSSLESS),"lossless"));
    assert(!strcmp(wineps5_fg_method_name(WINEPS5_FG_METHOD_AUTO),"auto"));
    assert(wineps5_fg_mode_supported(WINEPS5_FG_METHOD_AUTO,WINEPS5_FG_MODE_BETWEEN));
    assert(!wineps5_fg_mode_supported(WINEPS5_FG_METHOD_AUTO,WINEPS5_FG_MODE_AHEAD));
    assert(!strcmp(wineps5_fg_mode_name(WINEPS5_FG_MODE_AHEAD),"ahead"));
    assert(!strcmp(wineps5_fg_mode_name(WINEPS5_FG_MODE_BETWEEN),"between"));
    assert(wineps5_fg_mode_supported(WINEPS5_FG_METHOD_FSR3,WINEPS5_FG_MODE_BETWEEN));
    assert(wineps5_fg_mode_supported(WINEPS5_FG_METHOD_LOSSLESS,WINEPS5_FG_MODE_BETWEEN));
    assert(!wineps5_fg_mode_supported(WINEPS5_FG_METHOD_FSR3,WINEPS5_FG_MODE_AHEAD));
    assert(!wineps5_fg_mode_supported(WINEPS5_FG_METHOD_LOSSLESS,WINEPS5_FG_MODE_AHEAD));
    assert(!parse("fg=on method=FSR3",&s));
    assert(!parse("fg=on method=unknown",&s));
    assert(!parse("fg=on method=fsr3 method=lossless",&s));
    assert(!parse("fg=on mode=",&s));
    assert(!parse("fg=on mode=AHEAD",&s));
    assert(!parse("fg=on mode=between mode=ahead",&s));
    /* This ps5-framegen revision has no quality presets, for either mode. */
    assert(!parse("fg=on method=fsr3 mode=between quality=performance",&s));
    assert(!parse("fg=on method=fsr3 mode=ahead quality=balanced",&s));
    assert(!parse("fg=on method=fsr3 mode=between quality=quality",&s));
    assert(!parse("fg=on method=lossless quality=quality",&s));
    assert(parse("flow=0.25\nmult=4 fg=on\r\n",&s) && s.enabled && s.mult == 4 && s.flow == 0.25f);
    assert(parse("fg=on mult=3 flow=0.75",&s) && s.mult == 3 && s.flow == 0.75f);
    assert(parse("fg=auto",&s) && s.automatic && !s.enabled && s.on_below == 70 && s.off_above == 80);
    assert(parse("fg=auto on_below=72.5 off_above=82.5",&s) && s.on_below == 72.5f && s.off_above == 82.5f);
    assert(!parse("fg=auto on_below=80 off_above=80",&s));
    assert(!parse("fg=auto on_below=90 off_above=80",&s));
    assert(!parse("fg=auto on_below=nan",&s));
    assert(!parse("fg=auto on_below=-1",&s));
    assert(!parse("fg=auto off_above=0",&s));
    assert(!parse("fg=auto on_below=60 on_below=65",&s));
    assert(!parse("fg=on fg=off",&s));
    assert(!parse("fg=ON",&s));
    assert(!parse("fg=on mult=5",&s));
    assert(!parse("fg=on mult=02",&s));
    assert(!parse("fg=on flow=nan",&s));
    assert(!parse("fg=on flow=inf",&s));
    assert(!parse("fg=on flow=0.24",&s));
    assert(!parse("fg=on flow=1.01",&s));
    assert(!parse("fg=on flow=",&s));
    assert(!parse("fg=on flow=0.5garbage",&s));
    assert(!parse("fg=on perf=1",&s));
    assert(!parse("mult=2",&s));
    assert(!parse("",&s));
}
static void test_reuse_and_pso(void)
{
    struct wineps5_fg_settings old, next;
    struct wineps5_pso_stats s = {0};
    struct wineps5_pso_profile p = {0};
    unsigned int i;
    assert(parse("fg=auto",&old) && parse("fg=on",&next));
    assert(wineps5_fg_reuse_engine(&old,&next));
    assert(parse("fg=auto on_below=60 off_above=90",&next));
    assert(wineps5_fg_reuse_engine(&old,&next));
    assert(parse("fg=off",&next) && !wineps5_fg_reuse_engine(&old,&next));
    assert(parse("fg=auto mult=3",&next) && !wineps5_fg_reuse_engine(&old,&next));
    assert(parse("fg=auto flow=0.5",&next) && !wineps5_fg_reuse_engine(&old,&next));
    assert(parse("fg=auto method=fsr3",&next) && !wineps5_fg_reuse_engine(&old,&next));
    assert(parse("fg=auto mode=ahead",&next) && !wineps5_fg_reuse_engine(&old,&next));
    wineps5_pso_note(&s,100,true,false,true);
    wineps5_pso_note(&s,200,true,true,false);
    wineps5_pso_note(&s,300,false,true,false);
    assert(s.created == 2 && s.total_ns == 600 && s.max_ns == 300);
    assert(s.draw_time == 1 && s.cache_hits == 1 && s.cache_misses == 1);
    wineps5_pso_profile_note(&p,100,true,true);
    wineps5_pso_profile_note(&p,50000000,true,true);
    wineps5_pso_profile_note(&p,190000000,true,false);
    wineps5_pso_profile_note(&p,200000000,false,true);
    assert(p.hit_ns == 50000100 && p.hit_max_ns == 50000000);
    assert(p.miss_ns == 190000000 && p.miss_max_ns == 190000000);
    assert(p.slow == 2 && p.slow_hits == 1 && p.failed == 1);
    assert(!wineps5_pso_profile_detail(&p, WINEPS5_PSO_SLOW_NS - 1));
    for (i = 0; i < WINEPS5_PSO_MAX_DETAILS; ++i)
        assert(wineps5_pso_profile_detail(&p, WINEPS5_PSO_SLOW_NS));
    assert(!wineps5_pso_profile_detail(&p, WINEPS5_PSO_SLOW_NS));
    assert(!wineps5_pso_profile_detail(&p, 190000000));
    assert(p.details == WINEPS5_PSO_MAX_DETAILS && p.suppressed == 2);
    assert(!wineps5_pso_spike(0,100000000,90000000));
    assert(!wineps5_pso_spike(1,50000001,100));
    assert(wineps5_pso_spike(1,50000002,100));
    assert(!wineps5_pso_spike(100,100000000,100));
    assert(!wineps5_pso_spike(100,100000000,100000001));
    assert(!wineps5_pso_spike(100,99,100));
    assert(wineps5_pso_spike(100,100000000,100000000));
}
static void test_adoption(void)
{
    uint32_t words[] = {0x07230203,0x10000,0,10,0,
        (4u << 16) | 71,1,33,9, (4u << 16) | 71,2,33,1,
        (4u << 16) | 71,3,33,4, (4u << 16) | 71,1,34,0,
        (4u << 16) | 71,2,34,1, (4u << 16) | 71,3,34,0};
    uint32_t bad[] = {0x07230203,0x10000,0,10,0,0};
    unsigned int i, count = 0;
    assert(wineps5_fg_shader_id(0) == 255 && wineps5_fg_shader_id(1) == 256);
    for (i = 2; i < 25; ++i) assert(wineps5_fg_shader_id(i) == 278 + i);
    assert(wineps5_fg_shader_id(25) == 0);
    assert(wineps5_fg_adopt(words,sizeof(words) / 4));
    assert(words[8] == 1 && words[12] == 2 && words[16] == 0);
    assert(!wineps5_fg_adopt(bad,sizeof(bad) / 4));
    bad[5] = (20u << 16) | 71; assert(!wineps5_fg_adopt(bad,sizeof(bad) / 4));
    bad[0] = 0; assert(!wineps5_fg_adopt(bad,5));
    for (i = 1; i <= 3; ++i)
    {
        unsigned int g;
        for (g = 0; g < i; ++g)
        {
            assert(wineps5_fg_slot(i,g) == count++);
            assert(fabsf(wineps5_fg_timestamp(wineps5_fg_slot(i,g)) - (float)(g + 1) / (i + 1)) < 1e-6f);
        }
    }
    assert(count == 6);
}
static void test_resources(void)
{
    unsigned char bytes[1024] = {0};
    struct wineps5_fg_blob b = {bytes,sizeof(bytes)}, out;
    /* A tiny PE32+ with .rsrc at RVA 0x1000/raw 0x200 and resource 304.
     * Synthetic SPIR-V only: no Lossless.dll bytes. */
    put16(bytes,0,0x5a4d); put32(bytes,60,0x80); put32(bytes,0x80,0x4550);
    put16(bytes,0x86,1); put16(bytes,0x94,0xf0); put16(bytes,0x98,0x20b);
    put32(bytes,0x118,0x1000); put32(bytes,0x11c,0x200);
    put32(bytes,0x190,0x200); put32(bytes,0x194,0x1000);
    put32(bytes,0x198,0x200); put32(bytes,0x19c,0x200);
    put16(bytes,0x20e,1); put32(bytes,0x210,10); put32(bytes,0x214,0x80000020);
    put16(bytes,0x22e,1); put32(bytes,0x230,304); put32(bytes,0x234,0x80000040);
    put16(bytes,0x24e,1); put32(bytes,0x250,1033); put32(bytes,0x254,0x60);
    put32(bytes,0x260,0x1080); put32(bytes,0x264,20); put32(bytes,0x280,0x07230203);
    assert(wineps5_fg_resource(b,255,&out) && out.size == 20 && out.data == bytes + 0x280);
    assert(!wineps5_fg_resource(b,256,&out));
    assert(!wineps5_fg_resource((struct wineps5_fg_blob){bytes,0x270},255,&out));
    put32(bytes,0x264,0xffffffff); assert(!wineps5_fg_resource(b,255,&out));
    put32(bytes,0x264,20); put32(bytes,0x234,0xffffffff); assert(!wineps5_fg_resource(b,255,&out));
    put32(bytes,0x234,0x80000040); put32(bytes,60,0xffffffff); assert(!wineps5_fg_resource(b,255,&out));
}
static void test_pacer(void)
{
    struct wineps5_fg_pacer p = {0};
    uint64_t now = 1, dt;
    unsigned int i, n, total, mult;
    assert(wineps5_fg_plan(&p,now,2) == 0);
    for (i = 0; i < 50; ++i) { now += 16666667; assert(wineps5_fg_plan(&p,now,2) == 0); }
    for (mult = 2; mult <= 4; ++mult)
    {
        memset(&p,0,sizeof(p)); now = 1; total = 0; dt = 33333333;
        for (i = 0; i < 400; ++i)
        {
            now += dt; n = wineps5_fg_plan(&p,now,mult);
            assert(n <= mult - 1);
            if (i >= 100) total += n;
        }
        assert(total > 295 * (mult - 1) && total <= 300 * (mult - 1));
    }
    memset(&p,0,sizeof(p)); now = 1; total = 0;
    for (i = 0; i < 1000; ++i)
    { now += 16666667; n = wineps5_fg_plan(&p,now,4); assert(n <= 1); if (i >= 100) total += n; }
    assert(total >= 895 && total <= 900);
    now += 1000000; assert(wineps5_fg_plan(&p,now,4) == 0); /* burst */
    now += 200000000; assert(wineps5_fg_plan(&p,now,4) == 0); /* stall */
    assert(p.interval == 0 && p.credit == 0);
    memset(&p,0,sizeof(p)); now = 1;
    for (i = 0; i < 400; ++i) { now += 7000000; assert(wineps5_fg_plan(&p,now,4) == 0); }
}
static void test_deadlines_and_hud(void)
{
    struct wineps5_hud_timing game = {0}, output = {0};
    struct wineps5_hud_metrics gm, om;
    uint64_t actual, last_actual = 0, ready, step = UINT64_C(8341675);
    struct wineps5_fg_schedule s;
    unsigned int i;
    /* A late generated call must not move the absolute real deadline or
     * feed back into readiness of the next independently prepared pair. */
    for (i = 1; i <= 600; ++i)
    {
        ready = (uint64_t)i * 2 * step;
        s = wineps5_fg_pair_schedule(ready,2 * step,1);
        assert(s.first == ready && s.real == ready + step);
        actual = s.first + UINT64_C(500000);
        if (last_actual) assert(actual - last_actual == step);
        wineps5_hud_note(&output,actual);
        actual = s.real + UINT64_C(500000);
        wineps5_hud_note(&output,actual);
        last_actual = actual;
    }
    /* All game callbacks are sampled, even if the worker coalesces a job.
     * Output samples include both generated and real QueuePresent successes. */
    for (i = 1; i <= 600; ++i)
        wineps5_hud_note(&game,(uint64_t)i * UINT64_C(16666667));
    gm = wineps5_hud_measure(&game); om = wineps5_hud_measure(&output);
    assert(fabs(gm.fps - 60.0) < 0.01 && fabs(gm.low_fps - 60.0) < 0.01);
    assert(fabs(om.fps - 119.88) < 0.01 && om.fps > 1.99 * gm.fps);
}
static void test_gpu_ready_pair(void)
{
    const uint64_t capture = UINT64_C(1000000000), ready = capture + UINT64_C(22000000);
    struct wineps5_fg_schedule s;
    unsigned int generated;
    /* The CPU capture + 16.7 ms deadline expired while its GPU copy/flow work
     * was in flight. That is not a missed interpolation slot: inputs only now
     * exist. Schedule generated first, then real, from readiness. */
    assert(ready > capture + UINT64_C(16666667));
    for (generated = 0; generated <= 3; ++generated)
    {
        s = wineps5_fg_pair_schedule(ready,UINT64_C(33333333),generated);
        assert(s.first == ready);
        assert(s.real >= s.first);
        assert(s.real - s.first == generated * s.step);
        assert(s.step >= UINT64_C(8341675));
    }
    s = wineps5_fg_pair_schedule(ready,UINT64_C(16666667),1);
    assert(s.first == ready && s.real == ready + UINT64_C(8341675));
    /* Zero generated frames add no intentional real-frame hold. */
    s = wineps5_fg_pair_schedule(ready,UINT64_C(16666667),0);
    assert(s.real == ready);
}
static void test_prefetch_budget(void)
{
    const uint64_t deadline = UINT64_C(1000000000);
    unsigned int flags;
    for (flags = 0; flags < 16; ++flags)
        assert(wineps5_fg_prefetch_eligible(flags & 1,flags & 2,flags & 4,flags & 8) == (flags == 1));
    assert(wineps5_fg_prefetch_fits(deadline - 2000000,deadline,3000000));
    assert(!wineps5_fg_prefetch_fits(deadline - 2000000,deadline,3000001));
    assert(wineps5_fg_prefetch_fits(deadline - 100000,deadline,1100000));
    assert(!wineps5_fg_prefetch_fits(deadline - 100000,deadline,1100001));
    assert(!wineps5_fg_prefetch_fits(deadline,deadline,0));
    assert(!wineps5_fg_prefetch_fits(deadline + 1,deadline,0));
}
static void test_game_block_metrics(void)
{
    struct wineps5_fg_block_stats s = {0};
    assert(wineps5_fg_block_mean_ms(&s) == 0);
    wineps5_fg_block_note(&s,0);
    wineps5_fg_block_note(&s,UINT64_C(2000000));
    wineps5_fg_block_note(&s,UINT64_C(10000000));
    assert(s.count == 3 && s.total_ns == UINT64_C(12000000));
    assert(s.max_ns == UINT64_C(10000000));
    assert(fabs(wineps5_fg_block_mean_ms(&s) - 4.0) < 1e-9);
    s = (struct wineps5_fg_block_stats){0};
    assert(s.count == 0 && s.max_ns == 0 && wineps5_fg_block_mean_ms(&s) == 0);
}
static void test_auto(void)
{
    struct wineps5_fg_settings s = wineps5_fg_defaults();
    struct wineps5_fg_auto a = {0};
    uint64_t start = UINT64_C(1000000000), now;
    unsigned int i;
    s.automatic = true;
    for (i = 0; i <= 120; ++i) wineps5_fg_auto_note(&a,start + (uint64_t)i * UINT64_C(16666667));
    assert(a.count == 2 && fabs(a.game_fps - 60) < 0.01);
    assert(!wineps5_fg_auto_decide(&a,&s,start + UINT64_C(2000000000)));
    for (i = 121; i <= 180; ++i) wineps5_fg_auto_note(&a,start + (uint64_t)i * UINT64_C(16666667));
    now = start + UINT64_C(3000000060);
    assert(a.count == 3 && wineps5_fg_auto_decide(&a,&s,now) && a.enabled);
    a.game_fps = 90;
    assert(!wineps5_fg_auto_decide(&a,&s,now + UINT64_C(4000000000)) && a.enabled);
    assert(wineps5_fg_auto_decide(&a,&s,now + UINT64_C(5000000000)) && !a.enabled);
    a.game_fps = 60;
    assert(!wineps5_fg_auto_decide(&a,&s,now + UINT64_C(9000000000)));
    assert(wineps5_fg_auto_decide(&a,&s,now + UINT64_C(10000000000)) && a.enabled);
    /* Strict thresholds, deadband, and the one-second evaluation gate. */
    a = (struct wineps5_fg_auto){.count = 3,.game_fps = 70};
    assert(!wineps5_fg_auto_decide(&a,&s,start));
    a.game_fps = 69;
    assert(!wineps5_fg_auto_decide(&a,&s,start + UINT64_C(500000000)));
    assert(wineps5_fg_auto_decide(&a,&s,start + UINT64_C(1000000000)) && a.enabled);
    a.game_fps = 80;
    assert(!wineps5_fg_auto_decide(&a,&s,start + UINT64_C(6000000000)) && a.enabled);
    a.game_fps = 81;
    assert(!wineps5_fg_auto_decide(&a,&s,start + UINT64_C(6500000000)));
    assert(wineps5_fg_auto_decide(&a,&s,start + UINT64_C(7000000000)) && !a.enabled);
}
static void test_present_latency(void)
{
    struct wineps5_present_latency h = {0};
    unsigned int i;
    assert(wineps5_present_latency_mean_ms(&h) == 0);
    assert(wineps5_present_latency_percentile_ms(&h,50) == 0);
    for (i = 0; i < 50; ++i) wineps5_present_latency_note(&h,UINT64_C(1000000));
    for (i = 0; i < 45; ++i) wineps5_present_latency_note(&h,UINT64_C(10000000));
    for (i = 0; i < 5; ++i) wineps5_present_latency_note(&h,UINT64_C(20000000));
    assert(h.count == 100 && h.max_ns == UINT64_C(20000000));
    assert(fabs(wineps5_present_latency_mean_ms(&h) - 6) < 1e-9);
    assert(wineps5_present_latency_percentile_ms(&h,50) == 1.25);
    assert(wineps5_present_latency_percentile_ms(&h,95) == 10.25);
    memset(&h,0,sizeof(h));
    wineps5_present_latency_note(&h,UINT64_C(750000000));
    assert(h.bins[WINEPS5_PRESENT_LATENCY_BUCKETS - 1] == 1);
    assert(wineps5_present_latency_percentile_ms(&h,50) == 750);
    assert(wineps5_present_latency_percentile_ms(&h,95) == 750);
    memset(&h,0,sizeof(h)); wineps5_present_latency_note(&h,0);
    assert(h.count == 1 && wineps5_present_latency_percentile_ms(&h,95) == 0);
}

static void test_display_latency(void)
{
    struct wineps5_display_latency_ring r = {0};
    struct wineps5_present_latency h = {0};
    uint64_t interval, first = UINT64_C(1000000000);
    unsigned int i;
    assert(!wineps5_display_latency_push(&r,0,first,false));
    assert(!wineps5_display_latency_push(&r,1,0,false));
    for (i = 0; i < WINEPS5_DISPLAY_LATENCY_CAPACITY; ++i)
        assert(wineps5_display_latency_push(&r,i + 1,first + i,(i + 1) % 2 != 0));
    assert(!wineps5_display_latency_push(&r,65,first,true));
    assert(r.count == WINEPS5_DISPLAY_LATENCY_CAPACITY && r.entries[r.head].id == 1);
    wineps5_display_latency_pop(&r);
    assert(wineps5_display_latency_push(&r,65,first,true));
    for (i = 2; i <= 65; ++i)
    {
        assert(r.entries[r.head].id == i);
        assert(r.entries[r.head].fg == (i % 2 != 0));
        wineps5_display_latency_pop(&r);
    }
    assert(!r.count);
    wineps5_display_latency_pop(&r);
    assert(!r.count);
    assert(!wineps5_display_latency_interval(0,first,&interval));
    assert(!wineps5_display_latency_interval(first,first - 1,&interval));
    assert(wineps5_display_latency_interval(first,first,&interval) && interval == 0);
    assert(wineps5_display_latency_interval(first,first + UINT64_C(25000000),&interval));
    wineps5_present_latency_note(&h,interval);
    assert(h.count == 1 && wineps5_present_latency_mean_ms(&h) == 25);
    assert(wineps5_present_latency_percentile_ms(&h,50) == 25);
    assert(wineps5_present_latency_percentile_ms(&h,95) == 25);
    assert(h.max_ns == UINT64_C(25000000));
    /* Both paths measure from the original API timestamp, not from their
     * different QueuePresent timestamps (submit-only: 0.2 vs 25 ms). */
    assert(wineps5_display_latency_interval(first,first + UINT64_C(33000000),&interval));
    wineps5_present_latency_note(&h,interval);
    assert(wineps5_present_latency_mean_ms(&h) == 29);
}

static void test_fg_stages(void)
{
    struct wineps5_fg_stage_stats s = {0};
    uint64_t begin = UINT64_C(1000000000);
    uint64_t ages[] = {200000,7000000,400000,9600000,9800000,18100000,19000000};
    unsigned int i;
    for (i = 0; i < WINEPS5_FG_STAGE_COUNT; ++i)
    {
        wineps5_fg_stage_note(&s,i,begin,begin + ages[i]);
        assert(s.stages[i].count == 1 && s.stages[i].total_ns == ages[i]);
        assert(fabs(wineps5_fg_block_mean_ms(&s.stages[i]) - ages[i] * 1e-6) < 1e-6);
        wineps5_fg_stage_note(&s,i,0,begin + ages[i]);
        wineps5_fg_stage_note(&s,i,begin,begin - 1);
        assert(s.stages[i].count == 1);
    }
    wineps5_fg_stage_note(&s,WINEPS5_FG_STAGE_COUNT,begin,begin);
    wineps5_fg_stage_note(&s,UINT32_MAX,begin,begin);
    wineps5_fg_stage_note(&s,WINEPS5_FG_STAGE_REAL_PRESENT,begin,begin + UINT64_C(20000000));
    assert(s.stages[WINEPS5_FG_STAGE_REAL_PRESENT].count == 2);
    assert(fabs(wineps5_fg_block_mean_ms(&s.stages[WINEPS5_FG_STAGE_REAL_PRESENT]) - 19.05) < 1e-6);
    memset(&s,0,sizeof(s));
    for (i = 0; i < WINEPS5_FG_STAGE_COUNT; ++i)
        assert(!s.stages[i].count && wineps5_fg_block_mean_ms(&s.stages[i]) == 0);
}

#define HITCH_MS(x) ((uint64_t)(x) * UINT64_C(1000000))

struct hitch_sim_thread
{
    struct wineps5_hitch_wine wine;
    struct wineps5_hitch_thread totals;
    struct wineps5_hitch_scope_state scope;
    struct wineps5_hitch_track track;
    struct wineps5_hitch_cpu_bound prev_bound;
};

static void hitch_wait(struct hitch_sim_thread *t, uint64_t begin_ms, uint64_t end_ms, bool end)
{
    if (!end) { t->wine.wait_start_ns = HITCH_MS(begin_ms); return; }
    t->wine.wait_ns += HITCH_MS(end_ms) - t->wine.wait_start_ns;
    t->wine.wait_count++;
    t->wine.wait_start_ns = 0;
}

static void hitch_scope(struct hitch_sim_thread *t, unsigned int bucket, uint64_t now_ms, bool close)
{
    struct wineps5_hitch_wine_totals w;
    wineps5_hitch_wine_effective(&t->wine, HITCH_MS(now_ms), &w);
    if (close) wineps5_hitch_scope_close(&t->totals, &t->scope, HITCH_MS(now_ms), &w);
    else wineps5_hitch_scope_open(&t->scope, bucket, HITCH_MS(now_ms), &w);
}

static bool hitch_step(struct hitch_sim_thread *t, uint64_t now_ms, struct wineps5_hitch_snapshot *d, uint64_t *span)
{
    struct wineps5_hitch_wine_totals w;
    struct wineps5_hitch_snapshot snap;
    struct wineps5_hitch_cpu_sample cpu;
    wineps5_hitch_wine_effective(&t->wine, HITCH_MS(now_ms), &w);
    wineps5_hitch_effective(&t->totals, &t->scope, &w, HITCH_MS(now_ms), &snap);
    wineps5_hitch_cpu_read(&t->wine, &cpu);
    return wineps5_hitch_track_step(&t->track, &snap, &cpu, HITCH_MS(now_ms), d, span, &t->prev_bound);
}

/* Owner-side CPU sample (blocking transition or profiler tick). */
static void hitch_cpu(struct hitch_sim_thread *t, uint64_t at_ms, uint64_t cpu_ms, uint32_t core)
{
    t->wine.cpu_ns = HITCH_MS(cpu_ms);
    t->wine.cpu_sample_ns = HITCH_MS(at_ms);
    t->wine.cpu_core = core;
    t->wine.cpu_samples++;
}

static void test_hitch_cpu(void)
{
    struct wineps5_hitch_cpu_bound b = {0}, b0 = {0}, b1 = {0};
    struct hitch_sim_thread run, starved, blocked;
    struct wineps5_hitch_cpu_sample sample = {0};
    struct wineps5_hitch_snapshot d;
    struct wineps5_hitch_cpu cpu;
    uint64_t span;

    /* Before-sample while running: c <= cpu(t) <= c + (t - s); a later sample tightens it. */
    sample.cpu_ns = HITCH_MS(5); sample.sample_ns = HITCH_MS(10); sample.core = 3;
    wineps5_hitch_cpu_refine(&b, &sample, HITCH_MS(30));
    assert(b.valid && b.lo == HITCH_MS(5) && b.hi == HITCH_MS(25) && b.core == 3);
    sample.cpu_ns = HITCH_MS(12); sample.sample_ns = HITCH_MS(40); sample.core = 4;
    wineps5_hitch_cpu_refine(&b, &sample, HITCH_MS(30));
    assert(b.lo == HITCH_MS(5) && b.hi == HITCH_MS(12) && b.core == 4);
    /* A sample taken at wait entry, still blocked: exact. No sample: unchanged. */
    memset(&b, 0, sizeof(b));
    sample.cpu_ns = HITCH_MS(5); sample.sample_ns = HITCH_MS(10); sample.blocked_since = true;
    wineps5_hitch_cpu_refine(&b, &sample, HITCH_MS(30));
    assert(b.lo == HITCH_MS(5) && b.hi == HITCH_MS(5));
    sample.sample_ns = 0;
    wineps5_hitch_cpu_refine(&b, &sample, HITCH_MS(40));
    assert(b.lo == HITCH_MS(5) && b.hi == HITCH_MS(5));
    /* Interval: [b1.lo - b0.hi, b1.hi - b0.lo] capped by the span; midpoint +- half-width. */
    b0.valid = b1.valid = true;
    b0.lo = b0.hi = HITCH_MS(5); b1.lo = HITCH_MS(40); b1.hi = HITCH_MS(60);
    wineps5_hitch_cpu_interval(&b0, &b1, HITCH_MS(80), &cpu);
    assert(cpu.valid && cpu.cpu_ns == HITCH_MS(45) && cpu.err_ns == HITCH_MS(10));
    wineps5_hitch_cpu_interval(&b0, &b1, HITCH_MS(45), &cpu);
    assert(cpu.cpu_ns == HITCH_MS(40) && cpu.err_ns == HITCH_MS(5));
    b0.valid = false;
    wineps5_hitch_cpu_interval(&b0, &b1, HITCH_MS(80), &cpu);
    assert(!cpu.valid && !cpu.cpu_ns);

    /* Track: a thread running flat out with 20-Hz tick samples at 10 and 60 ms;
     * presents at 16, 76 (hitch) and 92. Bounds bracket the true 60 ms. */
    memset(&run, 0, sizeof(run));
    hitch_cpu(&run, 10, 10, 2);
    assert(!hitch_step(&run, 16, &d, &span) && run.track.bound.lo == HITCH_MS(10) && run.track.bound.hi == HITCH_MS(16));
    hitch_cpu(&run, 60, 60, 2);
    assert(hitch_step(&run, 76, &d, &span) && span == HITCH_MS(60) && d.wine.cpu_samples == 1);
    assert(run.prev_bound.lo == HITCH_MS(16) && run.prev_bound.hi == HITCH_MS(16)); /* refined cpu(16) */
    b0 = run.prev_bound; b1 = run.track.bound;
    assert(b1.lo == HITCH_MS(60) && b1.hi == HITCH_MS(76));
    wineps5_hitch_cpu_interval(&b0, &b1, span, &cpu);
    assert(cpu.cpu_ns == HITCH_MS(52) && cpu.err_ns == HITCH_MS(8));
    /* The deferred print refines cpu(76) with the next sample (tick at 80). */
    hitch_cpu(&run, 80, 80, 5);
    hitch_step(&run, 92, &d, &span);
    b1 = run.prev_bound;
    assert(b1.lo == HITCH_MS(76) && b1.hi == HITCH_MS(76) && b1.core == 5);
    wineps5_hitch_cpu_interval(&b0, &b1, HITCH_MS(60), &cpu);
    assert(cpu.cpu_ns == HITCH_MS(60) && !cpu.err_ns);

    /* Runnable but starved: no tick serviced during the hitch, then 10 ms of CPU by 100 ms. */
    memset(&starved, 0, sizeof(starved));
    hitch_cpu(&starved, 10, 10, 1);
    hitch_step(&starved, 16, &d, &span);
    assert(hitch_step(&starved, 76, &d, &span) && !d.wine.cpu_samples);
    b0 = starved.prev_bound;
    hitch_cpu(&starved, 100, 20, 1);
    hitch_step(&starved, 108, &d, &span);
    b1 = starved.prev_bound;
    assert(b1.lo == HITCH_MS(10) && b1.hi == HITCH_MS(20));
    wineps5_hitch_cpu_interval(&b0, &b1, HITCH_MS(60), &cpu);
    /* cpu(16) in [10, 16], cpu(76) in [10, 20]: CPU in [0, 10] -> 5 +- 5 against busy 60. */
    assert(cpu.cpu_ns == HITCH_MS(5) && cpu.err_ns == HITCH_MS(5));

    /* Blocked across the whole hitch (sampled at wait entry): exactly 0 CPU. */
    memset(&blocked, 0, sizeof(blocked));
    hitch_wait(&blocked, 12, 0, false);
    hitch_cpu(&blocked, 12, 30, 6);
    hitch_step(&blocked, 16, &d, &span);
    assert(hitch_step(&blocked, 76, &d, &span) && d.wine.wait_ns == HITCH_MS(60));
    wineps5_hitch_cpu_interval(&blocked.prev_bound, &blocked.track.bound, span, &cpu);
    assert(cpu.valid && !cpu.cpu_ns && !cpu.err_ns);
}

static void test_hitch(void)
{
    static const uint64_t presents_ms[] = {16, 32, 48, 64, 124, 140};
    struct hitch_sim_thread threads[6], main_thread;
    struct wineps5_hitch_snapshot d[6], main_d, snap;
    struct wineps5_hitch_wine_totals w;
    struct wineps5_hitch_limiter l = {0}, closed;
    struct wineps5_hitch_clock clock = {0};
    uint64_t span[6], main_span = 0, busy[6], frame, now = UINT64_C(5000000000);
    struct wineps5_hitch_gpu g = {0};
    struct wineps5_hitch_split s;
    unsigned int i, k, hitches = 0;
    bool eligible[6], main_had = false;

    /* Must match ps5-wine struct wine_ps5_hitch_counters at its TEB offset. */
    assert(WINEPS5_HITCH_TEB64_OFFSET == 0x568 && WINEPS5_HITCH_MAGIC == 0x48545350);
    assert(offsetof(struct wineps5_hitch_wine, armed) == 4 && offsetof(struct wineps5_hitch_wine, tid) == 8 &&
            offsetof(struct wineps5_hitch_wine, sequence) == 12 && offsetof(struct wineps5_hitch_wine, wait_ns) == 16 &&
            offsetof(struct wineps5_hitch_wine, read_ns) == 32 && offsetof(struct wineps5_hitch_wine, unwind_ns) == 48 &&
            offsetof(struct wineps5_hitch_wine, wait_start_ns) == 64 &&
            offsetof(struct wineps5_hitch_wine, main_block) == 72 && offsetof(struct wineps5_hitch_wine, cpu_ns) == 80 &&
            offsetof(struct wineps5_hitch_wine, cpu_sample_ns) == 88 && offsetof(struct wineps5_hitch_wine, cpu_core) == 96 &&
            offsetof(struct wineps5_hitch_wine, cpu_samples) == 100 && sizeof(struct wineps5_hitch_wine) == 104 &&
            WINEPS5_HITCH_NO_CORE == 0xffffffffu);

    /* A wait in progress counts up to the snapshot time. */
    memset(&threads[0], 0, sizeof(threads[0]));
    threads[0].wine.wait_ns = 7; threads[0].wine.wait_start_ns = 100;
    wineps5_hitch_wine_effective(&threads[0].wine, 130, &w);
    assert(w.wait_ns == 37);
    wineps5_hitch_wine_effective(&threads[0].wine, 90, &w);
    assert(w.wait_ns == 7);

    /* 10 ms resource call with 3 ms blocked and 1 ms read inside: 6 ms exclusive. */
    memset(&threads[0], 0, sizeof(threads[0]));
    hitch_scope(&threads[0], WINEPS5_HITCH_RESOURCE, 10, false);
    threads[0].wine.wait_ns = HITCH_MS(3); threads[0].wine.read_ns = HITCH_MS(1);
    hitch_scope(&threads[0], 0, 20, true);
    assert(threads[0].totals.bucket_ns[WINEPS5_HITCH_RESOURCE] == HITCH_MS(6) &&
            threads[0].totals.bucket_calls[WINEPS5_HITCH_RESOURCE] == 1 && threads[0].totals.vkd3d_wait_ns == HITCH_MS(3));
    /* Present waited 8 ms of a 9 ms call. */
    hitch_scope(&threads[0], WINEPS5_HITCH_PRESENT, 30, false);
    threads[0].wine.wait_ns += HITCH_MS(8);
    hitch_scope(&threads[0], 0, 39, true);
    assert(threads[0].totals.bucket_ns[WINEPS5_HITCH_PRESENT] == HITCH_MS(1) &&
            threads[0].totals.present_wait_ns == HITCH_MS(8));
    /* Nested time larger than the call floors at 0; closing without an open scope is a no-op. */
    hitch_scope(&threads[0], WINEPS5_HITCH_PSO, 40, false);
    threads[0].wine.unwind_ns += HITCH_MS(5);
    hitch_scope(&threads[0], 0, 41, true);
    assert(!threads[0].totals.bucket_ns[WINEPS5_HITCH_PSO] && threads[0].totals.bucket_calls[WINEPS5_HITCH_PSO] == 1);
    hitch_scope(&threads[0], 0, 42, true);
    assert(threads[0].totals.bucket_calls[WINEPS5_HITCH_PSO] == 1);

    /* Six threads present in rotation; any Present ends the global interval.
     * Events: B (1) waits 20-50; C (2) is in CreateCommittedResource 40-70 with a
     * 50-55 wait inside, then waits 72-120; threads 0, 1 and 5 wait 66-120; D (3)
     * never waits; main (no vkd3d slot) waits 70-120. The 64->124 interval is
     * the only hitch, presented by thread 4. */
    memset(threads, 0, sizeof(threads));
    memset(&main_thread, 0, sizeof(main_thread));
    for (k = 0; k < sizeof(presents_ms) / sizeof(presents_ms[0]); k++)
    {
        uint64_t t = presents_ms[k];

        if (t == 32) hitch_wait(&threads[1], 20, 0, false);
        if (t == 48) hitch_scope(&threads[2], WINEPS5_HITCH_RESOURCE, 40, false);
        if (t == 64)
        {
            hitch_wait(&threads[1], 0, 50, true);
            hitch_wait(&threads[2], 50, 0, false);
            hitch_wait(&threads[2], 0, 55, true);
        }
        if (t == 124)
        {
            hitch_wait(&threads[0], 66, 0, false); hitch_wait(&threads[1], 66, 0, false);
            hitch_wait(&threads[5], 66, 0, false);
            hitch_scope(&threads[2], 0, 70, true);
            hitch_wait(&main_thread, 70, 0, false);
            hitch_wait(&threads[2], 72, 0, false);
            for (i = 0; i < 6; i++) if (i != 3 && i != 4) hitch_wait(&threads[i], 0, 120, true);
            hitch_wait(&main_thread, 0, 120, true);
        }

        frame = wineps5_hitch_clock_step(&clock, HITCH_MS(t));
        assert(clock.seq == k + 1 && frame == (k ? HITCH_MS(t - presents_ms[k - 1]) : 0));
        hitches += wineps5_hitch_is_hitch(frame);
        for (i = 0; i < 6; i++)
        {
            eligible[i] = hitch_step(&threads[i], t, &d[i], &span[i]);
            assert(eligible[i] == (k > 0) && (!k || span[i] == frame));
            if (eligible[i]) { wineps5_hitch_split(span[i], &d[i], &s); busy[i] = s.busy_ns; }
        }
        memset(&snap, 0, sizeof(snap));
        wineps5_hitch_wine_effective(&main_thread.wine, HITCH_MS(t), &snap.wine);
        {
            struct wineps5_hitch_cpu_sample none = {0};
            main_had = wineps5_hitch_track_step(&main_thread.track, &snap, &none, HITCH_MS(t), &main_d, &main_span,
                    &main_thread.prev_bound);
        }

        /* B's 20-50 wait is split across three intervals while in progress. */
        if (t == 32) assert(d[1].wine.wait_ns == HITCH_MS(12) && !d[1].wine.wait_count);
        if (t == 48) assert(d[1].wine.wait_ns == HITCH_MS(16));
        if (t == 64) assert(d[1].wine.wait_ns == HITCH_MS(2) && d[1].wine.wait_count == 1);
        /* C's open resource call is attributed as it runs, minus its nested wait. */
        if (t == 48) assert(d[2].vkd3d.bucket_ns[WINEPS5_HITCH_RESOURCE] == HITCH_MS(8) &&
                !d[2].vkd3d.bucket_calls[WINEPS5_HITCH_RESOURCE]);
        if (t == 64) assert(d[2].vkd3d.bucket_ns[WINEPS5_HITCH_RESOURCE] == HITCH_MS(11) &&
                d[2].vkd3d.vkd3d_wait_ns == HITCH_MS(5) && d[2].wine.wait_ns == HITCH_MS(5));
        if (t == 140) assert(!d[2].vkd3d.bucket_ns[WINEPS5_HITCH_RESOURCE] && !d[1].wine.wait_ns);

        if (wineps5_hitch_is_hitch(frame))
        {
            int present = k % 6, busiest;

            assert(t == 124 && present == 4 && main_had && main_span == frame);
            assert(d[2].vkd3d.bucket_ns[WINEPS5_HITCH_RESOURCE] == HITCH_MS(6) &&
                    d[2].vkd3d.bucket_calls[WINEPS5_HITCH_RESOURCE] == 1 && d[2].wine.wait_ns == HITCH_MS(48));
            wineps5_hitch_split(span[2], &d[2], &s);
            assert(s.vkd3d_ns == HITCH_MS(6) && s.busy_ns == HITCH_MS(12) && s.other_ns == HITCH_MS(6) && !s.overlap_ns);
            /* Busiest = largest span - wait among non-present, non-main threads: D. */
            busiest = wineps5_hitch_pick_busiest(busy, eligible, 6, present, -1);
            assert(busiest == 3 && busy[3] == HITCH_MS(60) && busy[0] == HITCH_MS(6) && busy[2] == HITCH_MS(12));
            wineps5_hitch_split(span[3], &d[3], &s);
            assert(s.other_ns == HITCH_MS(60));
            /* Excluding D (as main) moves busiest to C; nothing eligible gives -1. */
            assert(wineps5_hitch_pick_busiest(busy, eligible, 6, present, 3) == 2);
            wineps5_hitch_split(main_span, &main_d, &s);
            assert(main_d.wine.wait_ns == HITCH_MS(50) && s.busy_ns == HITCH_MS(10) && s.other_ns == HITCH_MS(10));
        }
    }
    assert(hitches == 1);
    memset(eligible, 0, sizeof(eligible));
    assert(wineps5_hitch_pick_busiest(busy, eligible, 6, -1, -1) == -1);
    /* A thread first seen mid-interval only covers its own span. */
    memset(&threads[0].track, 0, sizeof(threads[0].track));
    assert(!hitch_step(&threads[0], 150, &d[0], &span[0]) && hitch_step(&threads[0], 170, &d[0], &span[0]) &&
            span[0] == HITCH_MS(20));

    memset(&d[0], 0, sizeof(d[0]));
    d[0].vkd3d.bucket_ns[WINEPS5_HITCH_PSO] = HITCH_MS(15); d[0].wine.wait_ns = HITCH_MS(10);
    wineps5_hitch_split(HITCH_MS(20), &d[0], &s);
    assert(!s.other_ns && s.overlap_ns == HITCH_MS(5) && s.busy_ns == HITCH_MS(10));
    assert(!wineps5_hitch_is_hitch(WINEPS5_HITCH_THRESHOLD_NS) && wineps5_hitch_is_hitch(WINEPS5_HITCH_THRESHOLD_NS + 1));

    wineps5_hitch_gpu_complete(&g, 0, 30, 2);
    wineps5_hitch_gpu_complete(&g, 0, 10, 1);
    wineps5_hitch_gpu_complete(&g, 2, 52, 1);
    wineps5_hitch_gpu_complete(&g, 3, 99, 1);
    assert(g.max_ns[0] == 30 && g.completed[0] == 3 && g.max_ns[2] == 52 && g.completed[2] == 1 && !g.max_ns[1]);
    wineps5_hitch_gpu_submit(&g, 5); wineps5_hitch_gpu_submit(&g, 9);
    assert(g.vk_submit_ns == 14 && g.vk_submit_max_ns == 9 && g.vk_submits == 2);

    /* Rate limit: 8 events per window, the rest counted; the window rolls after 10 s. */
    assert(!wineps5_hitch_window_roll(&l, now, &closed) && l.window_start_ns == now);
    assert(!wineps5_hitch_admit(&l, 16000000));
    for (i = 0; i < 12; ++i)
        assert(wineps5_hitch_admit(&l, 60000000 + i) == (i < WINEPS5_HITCH_MAX_LINES));
    assert(l.frames == 13 && l.hitches == 12 && l.logged == 8 && l.suppressed == 4 && l.max_frame_ns == 60000011);
    assert(!wineps5_hitch_window_roll(&l, now + WINEPS5_HITCH_WINDOW_NS - 1, &closed));
    assert(wineps5_hitch_window_roll(&l, now + WINEPS5_HITCH_WINDOW_NS, &closed));
    assert(closed.hitches == 12 && closed.suppressed == 4 && closed.window_start_ns == now);
    assert(l.window_start_ns == now + WINEPS5_HITCH_WINDOW_NS && !l.frames && !l.hitches && !l.logged);
    assert(wineps5_hitch_admit(&l, 70000000) && l.logged == 1);
    /* A backwards clock restarts the window instead of underflowing. */
    assert(!wineps5_hitch_window_roll(&l, now, &closed) && l.window_start_ns == now);
}

int main(void)
{
    test_reuse_and_pso();
    test_config(); test_adoption(); test_resources(); test_pacer(); test_deadlines_and_hud(); test_gpu_ready_pair(); test_prefetch_budget(); test_game_block_metrics(); test_auto(); test_present_latency(); test_display_latency(); test_fg_stages();
    test_hitch();
    test_hitch_cpu();
    puts("wineps5 FG helpers: PASS"); return 0;
}
