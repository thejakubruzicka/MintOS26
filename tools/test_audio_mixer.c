/* Host test for kernel/sound/audio_mixer.c: gain curve, immediate mixing,
 * additive saturation, silencing behind the play position, underrun
 * recovery, volume rewind, voice limits, dirty-range tracking and a
 * simulated DMA consumer with irregular service gaps (the output must be
 * bit-exact with no stale audio). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/audio_mixer.h"

static int failures, checks;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

#define RING 4096u
static int16_t ring[RING * 2u];

static int16_t sat(int64_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

int main(void) {
    MINT_MIXER m;
    CHECK(!mint_mixer_init(&m, ring, 3000u, 512u, 32u), "non power-of-two ring rejected");
    CHECK(!mint_mixer_init(&m, ring, RING, 4000u, 32u), "lead longer than half the ring rejected");
    CHECK(!mint_mixer_init(&m, ring, RING, 64u, 64u), "safety >= lead rejected");
    CHECK(mint_mixer_init(&m, ring, RING, 1024u, 48u), "mixer init");
    CHECK(mint_mixer_volume_gain(0) == 0 && mint_mixer_volume_gain(100) == MINT_MIXER_UNITY &&
          mint_mixer_volume_gain(50) == 8192u && mint_mixer_volume_gain(250) == MINT_MIXER_UNITY, "quadratic volume curve");
    int mono = 1;
    for (uint32_t v = 1; v <= 100u; ++v) if (mint_mixer_volume_gain(v) < mint_mixer_volume_gain(v - 1u)) mono = 0;
    CHECK(mono, "volume curve is monotonic");
    CHECK(mint_mixer_permille_gain(1000) == MINT_MIXER_UNITY && mint_mixer_permille_gain(500) == 16384u, "permille gain");

    /* immediate mix of a mono voice at unity */
    static int16_t tone[2000];
    for (int i = 0; i < 2000; ++i) tone[i] = (int16_t)((i * 37) % 20000 - 10000);
    uint32_t id = mint_mixer_start(&m, tone, 2000u, 1u, MINT_MIXER_UNITY, -1);
    CHECK(id > 0 && mint_mixer_active(&m) == 1u, "voice started");
    mint_mixer_fill(&m);
    int exact = 1;
    for (uint32_t f = 0; f < 1024u - 48u; ++f)
        if (ring[(48u + f) * 2u] != tone[f] || ring[(48u + f) * 2u + 1u] != tone[f]) exact = 0;
    CHECK(exact, "mono voice lands safety frames after the play position on both channels");
    CHECK(ring[0] == 0 && ring[47u * 2u] == 0, "nothing written inside the safety margin");
    uint32_t first = 0, count = 0;
    CHECK(mint_mixer_take_dirty(&m, &first, &count) && first == 0u && count >= 1024u, "dirty range covers mixed frames");

    /* additive saturation with a second voice */
    static int16_t loud[512];
    for (int i = 0; i < 512; ++i) loud[i] = 30000;
    uint64_t clipped0 = m.clipped;
    CHECK(mint_mixer_start(&m, loud, 512u, 1u, MINT_MIXER_UNITY, 2) > 0, "second voice");
    mint_mixer_fill(&m);
    CHECK(m.clipped > clipped0, "saturation counted");
    int satok = 1;
    for (uint32_t f = 0; f < 512u; ++f) if (ring[(48u + f) * 2u] != sat((int64_t)tone[f] + 30000)) satok = 0;
    CHECK(satok, "voices add and saturate instead of wrapping");
    CHECK(mint_mixer_slot_busy(&m, 2) && !mint_mixer_slot_busy(&m, 3), "pool slot bookkeeping");

    /* advance: played frames are silenced */
    CHECK(mint_mixer_advance(&m, 300u) == 300u && m.played == 300u, "advance by hardware position");
    int silent = 1;
    for (uint32_t f = 0; f < 300u; ++f) if (ring[f * 2u] || ring[f * 2u + 1u]) silent = 0;
    CHECK(silent, "played region silenced");

    /* volume rewind: everything after played+safety re-rendered at half gain */
    mint_mixer_set_master(&m, MINT_MIXER_UNITY / 2u, 0);
    int half = 1;
    for (uint64_t f = m.played + m.safety_frames; f < m.played + m.lead_frames && f - 48u < 2000u; ++f) {
        int64_t expect = ((int64_t)tone[f - 48u] * (MINT_MIXER_UNITY / 2u)) >> 15;
        if (f - 48u < 512u) expect += ((int64_t)30000 * (MINT_MIXER_UNITY / 2u)) >> 15;
        if (ring[(f % RING) * 2u] != sat(expect)) { half = 0; break; }
    }
    CHECK(half, "volume change re-renders queued audio");
    mint_mixer_set_master(&m, MINT_MIXER_UNITY, 1);
    int muted = 1;
    for (uint64_t f = m.played + m.safety_frames; f < m.played + m.lead_frames; ++f) if (ring[(f % RING) * 2u]) muted = 0;
    CHECK(muted, "mute silences queued audio");
    mint_mixer_set_master(&m, MINT_MIXER_UNITY, 0);

    /* voice limit */
    mint_mixer_stop_all(&m);
    CHECK(mint_mixer_active(&m) == 0, "stop all");
    uint32_t started = 0;
    for (int i = 0; i < 12; ++i) started += mint_mixer_start(&m, tone, 2000u, 1u, 1000u, -1) ? 1u : 0u;
    CHECK(started == MINT_MIXER_VOICES, "voice table is bounded");
    CHECK(!mint_mixer_start(&m, tone, 0u, 1u, 1000u, -1) && !mint_mixer_start(&m, tone, 10u, 3u, 1000u, -1) &&
          !mint_mixer_start(&m, 0, 10u, 1u, 1000u, -1) && !mint_mixer_start(&m, tone, 10u, 1u, MINT_MIXER_UNITY + 1u, -1),
          "invalid voices rejected");
    mint_mixer_stop_all(&m);

    /* simulated DMA consumer with irregular service: output must be exact */
    MINT_MIXER s;
    CHECK(mint_mixer_init(&s, ring, RING, 1500u, 48u), "simulation mixer");
    static int16_t chime[9000 * 2];
    for (int i = 0; i < 9000; ++i) { chime[i * 2] = (int16_t)(i % 3000 - 1500); chime[i * 2 + 1] = (int16_t)(1500 - i % 2900); }
    static int16_t out[40000 * 2];
    uint64_t hw = 0, consumed = 0;
    uint32_t start_at = 0;
    srand(7);
    int started_voice = 0;
    while (consumed < 30000u) {
        uint32_t step = 40u + (uint32_t)(rand() % 900);          /* service gaps up to ~19 ms at 48 kHz */
        for (uint32_t k = 0; k < step && consumed < 30000u; ++k, ++consumed) {
            uint32_t idx = (uint32_t)(hw % RING) * 2u;
            out[consumed * 2u] = ring[idx];
            out[consumed * 2u + 1u] = ring[idx + 1u];
            ++hw;
        }
        (void)mint_mixer_advance(&s, (uint32_t)(hw % RING));
        if (!started_voice && consumed > 3000u) {
            start_at = (uint32_t)(s.played + s.safety_frames);
            CHECK(mint_mixer_start(&s, chime, 9000u, 2u, MINT_MIXER_UNITY, -1) > 0, "stereo voice in simulation");
            started_voice = 1;
        }
        mint_mixer_fill(&s);
    }
    int ok = 1;
    for (uint32_t f = 0; f < 30000u; ++f) {
        int16_t l = 0, r = 0;
        if (f >= start_at && f - start_at < 9000u) { l = chime[(f - start_at) * 2u]; r = chime[(f - start_at) * 2u + 1u]; }
        if (out[f * 2u] != l || out[f * 2u + 1u] != r) { ok = 0; printf("  mismatch at frame %u\n", f); break; }
    }
    CHECK(ok && s.underruns == 0, "consumer output is bit-exact with jittered service");

    /* starvation longer than the lead: silence, never stale audio, then recovery */
    MINT_MIXER u;
    CHECK(mint_mixer_init(&u, ring, RING, 1024u, 48u), "underrun mixer");
    static int16_t longv[20000];
    for (int i = 0; i < 20000; ++i) longv[i] = 1000;
    CHECK(mint_mixer_start(&u, longv, 20000u, 1u, MINT_MIXER_UNITY, -1) > 0, "long voice");
    mint_mixer_fill(&u);
    uint64_t pos = 0, stale = 0;
    for (uint32_t k = 0; k < 3000u; ++k, ++pos) if (k >= 1024u && ring[(uint32_t)(pos % RING) * 2u]) ++stale;
    CHECK(stale == 0, "starved consumer hears silence past the lead, not old data");
    (void)mint_mixer_advance(&u, (uint32_t)(pos % RING));
    mint_mixer_fill(&u);
    CHECK(u.underruns == 1u && u.voices[0].active && u.voices[0].cursor == u.played + u.lead_frames, "underrun counted and the voice resumes");

    printf("%s: %d/%d mixer checks passed\n", failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
