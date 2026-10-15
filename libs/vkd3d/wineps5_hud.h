/* PS5 HUD pure helpers. Original 5x7 bitmap artwork below is CC0/public domain. */
#ifndef WINEPS5_HUD_H
#define WINEPS5_HUD_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#define WINEPS5_HUD_SAMPLES 4096
#define WINEPS5_HUD_QUADS 512
struct wineps5_hud_settings { bool enabled; unsigned int pos, scale; };
struct wineps5_hud_timing { uint64_t timestamps[WINEPS5_HUD_SAMPLES]; unsigned int cursor, count; };
struct wineps5_hud_metrics { double fps, mean_ms, low_fps; };
static inline bool wineps5_hud_parse(char *text, struct wineps5_hud_settings *s)
{
    char *p = text, *key, *value;
    unsigned int seen = 0, bit;
    *s = (struct wineps5_hud_settings){false, 0, 2};
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
        if (!strcmp(key, "hud"))
        {
            bit = 1;
            if (!strcmp(value, "on")) s->enabled = true;
            else if (strcmp(value, "off")) return false;
        }
        else if (!strcmp(key, "pos"))
        {
            bit = 2;
            if (!strcmp(value, "tl")) s->pos = 0;
            else if (!strcmp(value, "tr")) s->pos = 1;
            else if (!strcmp(value, "bl")) s->pos = 2;
            else if (!strcmp(value, "br")) s->pos = 3;
            else return false;
        }
        else if (!strcmp(key, "scale"))
        {
            bit = 4;
            if (value[0] < '1' || value[0] > '4' || value[1]) return false;
            s->scale = value[0] - '0';
        }
        else return false;
        if (seen & bit) return false;
        seen |= bit;
    }
    return (seen & 1) != 0;
}
static inline float wineps5_hud_pq(float nits)
{
    float p = powf(nits / 10000.0f, 0.1593017578125f);
    return powf((0.8359375f + 18.8515625f * p) / (1.0f + 18.6875f * p), 78.84375f);
}
static inline void wineps5_hud_note(struct wineps5_hud_timing *t, uint64_t now)
{
    t->timestamps[t->cursor] = now;
    t->cursor = (t->cursor + 1) % WINEPS5_HUD_SAMPLES;
    if (t->count < WINEPS5_HUD_SAMPLES) ++t->count;
}
static inline uint64_t wineps5_hud_interval(const struct wineps5_hud_timing *t, unsigned int age)
{
    unsigned int i = (t->cursor + WINEPS5_HUD_SAMPLES - 1 - age) % WINEPS5_HUD_SAMPLES;
    return t->timestamps[i] - t->timestamps[(i + WINEPS5_HUD_SAMPLES - 1) % WINEPS5_HUD_SAMPLES];
}
static inline struct wineps5_hud_metrics wineps5_hud_measure(const struct wineps5_hud_timing *t)
{
    struct wineps5_hud_metrics m = {0};
    uint64_t slow[WINEPS5_HUD_SAMPLES / 100 + 1] = {0}, dt, total = 0, recent = 0, sum = 0;
    unsigned int n = 0, nr = 0, i, j, k;
    for (i = 0; i + 1 < t->count; ++i)
    {
        dt = wineps5_hud_interval(t, i);
        if (!dt) continue;
        if (total >= UINT64_C(5000000000)) break;
        total += dt; ++n;
        if (recent < UINT64_C(1000000000)) { recent += dt; ++nr; }
        for (j = 0; j < sizeof(slow) / sizeof(slow[0]); ++j)
            if (dt > slow[j])
            {
                for (k = sizeof(slow) / sizeof(slow[0]) - 1; k > j; --k) slow[k] = slow[k - 1];
                slow[j] = dt; break;
            }
    }
    if (recent) { m.fps = nr * 1e9 / recent; m.mean_ms = recent * 1e-6 / nr; }
    k = (n + 99) / 100;
    for (i = 0; i < k; ++i) sum += slow[i];
    if (sum) m.low_fps = k * 1e9 / sum;
    return m;
}
/* Row-major, five low bits per row; 35 bits per glyph. ASCII uppercase HUD vocabulary. */
static inline uint64_t wineps5_hud_glyph(char c)
{
    static const unsigned char rows[][7] = {
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2},{31,16,16,30,1,1,30},{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14},{14,17,17,15,1,1,14},
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
        {14,17,16,23,17,17,14},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
        {0,0,0,0,0,6,6},{0,6,6,0,6,6,0},{17,2,4,8,17,0,0},{0,0,0,31,0,0,0}
    };
    uint64_t mask = 0;
    unsigned int index, r;
    if (c >= '0' && c <= '9') index = c - '0';
    else if (c >= 'A' && c <= 'Z') index = c - 'A' + 10;
    else if (c == '.') index = 36;
    else if (c == ':') index = 37;
    else if (c == '%') index = 38;
    else if (c == '-') index = 39;
    else return 0;
    for (r = 0; r < 7; ++r) mask |= (uint64_t)rows[index][r] << (5 * r);
    return mask;
}
#endif
