#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"

static void require_true(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "audio_drc_test: %s\n", message);
    exit(1);
}

int main(void)
{
    rab_config cfg;
    rab_bridge bridge;
    int16_t input[80 * 2];
    int16_t output[80 * 2];
    rab_stats stats;

    rab_config_defaults(&cfg);
    cfg.channels = 2;
    cfg.source_rate = 1000.0;
    cfg.host_rate = 1000.0;
    cfg.taps = 8;
    cfg.phases = 32;
    cfg.target_ms = 10.0;
    cfg.preroll_ms = 0.0;
    cfg.ring_ms = 100.0;
    cfg.stretch_enable = 1;
    cfg.stretch_min_ms = 2.0;
    cfg.stretch_max_ms = 4.0;
    cfg.stretch_xfade_ms = 1.0;
    cfg.stretch_limit_ms = 5.0;

    require_true(rab_init(&bridge, &cfg) == 0, "bridge initialization failed");
    require_true(fabs(bridge.prime_ms - cfg.target_ms) < 0.001,
                 "zero preroll must prime at the steady-state target");

    rab_free(&bridge);
    cfg.preroll_ms = 4.0;
    require_true(rab_init(&bridge, &cfg) == 0,
                 "bridge reinitialization with explicit preroll failed");
    require_true(fabs(bridge.prime_ms - cfg.preroll_ms) < 0.001,
                 "explicit preroll must be independent of steady-state target");

    for (int i = 0; i < 80; ++i) {
        int16_t sample = (int16_t)((i & 1) ? 12000 : -12000);
        input[i * 2] = sample;
        input[i * 2 + 1] = sample;
    }

    rab_push(&bridge, input, 40);
    rab_pull(&bridge, output, 60);
    rab_get_stats(&bridge, &stats);

    require_true(stats.stretch_events == 1,
                 "one starvation episode should start one concealment loop");
    require_true(stats.stretch_frames == 5,
                 "concealment must stop at the configured five-frame limit");
    require_true(stats.underrun_events > 0,
                 "a stall beyond the concealment cap must enter the fade path");
    require_true(output[59 * 2] == 0 && output[59 * 2 + 1] == 0,
                 "bounded concealment must finish at silence");

    rab_push(&bridge, input + 40 * 2, 40);
    rab_pull(&bridge, output, 8);
    require_true(bridge.stretch_episode_frames == 0,
                 "live recovery must reset the per-stall concealment budget");

    rab_free(&bridge);
    puts("audio_drc_test: PASS");
    return 0;
}
