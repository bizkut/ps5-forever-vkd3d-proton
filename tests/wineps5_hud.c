/* Standalone host regression test for the PS5 present HUD's pure helpers. */
#include <assert.h>
#include <stdio.h>
#include "../libs/vkd3d/wineps5_hud.h"
int main(void)
{
    struct wineps5_hud_settings s;
    struct wineps5_hud_timing t = {0};
    struct wineps5_hud_metrics m;
    char on[] = "hud=on\nscale=4 pos=br\r\n";
    char defaults[] = "hud=off";
    char bad[] = "hud=on scale=5", duplicate[] = "hud=on hud=off";
    unsigned int i;
    assert(wineps5_hud_parse(on, &s) && s.enabled && s.scale == 4 && s.pos == 3);
    assert(wineps5_hud_parse(defaults, &s) && !s.enabled && s.scale == 2 && s.pos == 0);
    assert(!wineps5_hud_parse(bad, &s));
    assert(!wineps5_hud_parse(duplicate, &s));
    assert(fabsf(wineps5_hud_pq(203) - 0.58068888f) < 0.00001f);
    for (i = 0; i < 5000; ++i) wineps5_hud_note(&t, UINT64_C(10000000) * i);
    m = wineps5_hud_measure(&t);
    assert(fabs(m.fps - 100) < 0.0001 && fabs(m.mean_ms - 10) < 0.0001 && fabs(m.low_fps - 100) < 0.0001);
    /* Five slow frames in the most recent five seconds are precisely the 1% tail. */
    memset(&t, 0, sizeof(t));
    wineps5_hud_note(&t, 0);
    for (i = 1; i <= 500; ++i)
        wineps5_hud_note(&t, t.timestamps[(t.cursor + WINEPS5_HUD_SAMPLES - 1) % WINEPS5_HUD_SAMPLES] + (i > 495 ? UINT64_C(50000000) : UINT64_C(10000000)));
    m = wineps5_hud_measure(&t);
    assert(fabs(m.low_fps - 20) < 0.0001);
    assert(wineps5_hud_glyph('0') != 0 && wineps5_hud_glyph('A') != 0);
    assert(wineps5_hud_glyph(' ') == 0);
    assert((wineps5_hud_glyph('A') >> 35) == 0);
    puts("wineps5 HUD helpers: PASS");
    return 0;
}
