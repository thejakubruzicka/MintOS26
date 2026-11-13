/* Kernel PCM mixer — see kernel/include/audio_mixer.h. */
#include <stdint.h>
#include <stddef.h>
#include "../include/audio_mixer.h"

static void dirty(MINT_MIXER *m, uint64_t lo, uint64_t hi) {
    if (hi <= lo) return;
    if (m->dirty_hi <= m->dirty_lo) { m->dirty_lo = lo; m->dirty_hi = hi; return; }
    if (lo < m->dirty_lo) m->dirty_lo = lo;
    if (hi > m->dirty_hi) m->dirty_hi = hi;
}

int mint_mixer_init(MINT_MIXER *m, int16_t *ring, uint32_t ring_frames,
                    uint32_t lead_frames, uint32_t safety_frames) {
    if (!m || !ring || ring_frames < 256u || (ring_frames & (ring_frames - 1u)) ||
        !lead_frames || safety_frames >= lead_frames || lead_frames + safety_frames > ring_frames / 2u)
        return 0;
    for (size_t i = 0; i < sizeof(*m); ++i) ((uint8_t *)m)[i] = 0;
    m->ring = ring;
    m->ring_frames = ring_frames;
    m->lead_frames = lead_frames;
    m->safety_frames = safety_frames;
    m->master_q15 = MINT_MIXER_UNITY;
    for (uint32_t i = 0; i < ring_frames * 2u; ++i) ring[i] = 0;
    for (uint32_t v = 0; v < MINT_MIXER_VOICES; ++v) m->voices[v].pool_slot = -1;
    dirty(m, 0, ring_frames);
    return 1;
}

uint32_t mint_mixer_volume_gain(uint32_t volume) {
    if (volume > 100u) volume = 100u;
    return (volume * volume * MINT_MIXER_UNITY + 5000u) / 10000u;
}

uint32_t mint_mixer_permille_gain(uint32_t permille) {
    if (permille > 1000u) permille = 1000u;
    return (permille * MINT_MIXER_UNITY + 500u) / 1000u;
}

uint32_t mint_mixer_advance(MINT_MIXER *m, uint32_t pos) {
    if (!m || !m->ring) return 0;
    const uint32_t mask = m->ring_frames - 1u;
    pos &= mask;
    uint32_t delta = (pos - m->hw_frame) & mask;
    for (uint32_t i = 0; i < delta; ++i) {
        uint32_t idx = ((m->hw_frame + i) & mask) * 2u;
        m->ring[idx] = 0;
        m->ring[idx + 1u] = 0;
    }
    dirty(m, m->played, m->played + delta);
    m->hw_frame = pos;
    m->played += delta;
    return delta;
}

static int16_t saturate(int32_t v, uint64_t *clipped) {
    if (v > 32767) { ++*clipped; return 32767; }
    if (v < -32768) { ++*clipped; return -32768; }
    return (int16_t)v;
}

void mint_mixer_fill(MINT_MIXER *m) {
    if (!m || !m->ring) return;
    const uint32_t mask = m->ring_frames - 1u;
    const uint64_t horizon = m->played + m->lead_frames;
    const uint32_t master = m->muted ? 0u : m->master_q15;
    for (uint32_t vi = 0; vi < MINT_MIXER_VOICES; ++vi) {
        MINT_MIXER_VOICE *v = &m->voices[vi];
        if (!v->active) continue;
        /* A fully mixed voice stays alive until the hardware has played it:
           a volume change may still have to re-render its tail, and user PCM
           pool slots must not be reused before then. */
        if (v->pos >= v->frames) {
            if (v->cursor <= m->played) { v->active = 0; v->data = 0; }
            continue;
        }
        if (v->cursor < m->played) {               /* the hardware overtook this voice */
            uint64_t lost = m->played + m->safety_frames - v->cursor;
            ++m->underruns;
            v->pos = lost >= (uint64_t)(v->frames - v->pos) ? v->frames : v->pos + (uint32_t)lost;
            v->cursor += lost;
        }
        uint32_t gain = (uint32_t)(((uint64_t)v->gain_q15 * master) >> 15);
        uint64_t first = v->cursor;
        while (v->cursor < horizon && v->pos < v->frames) {
            uint32_t idx = (uint32_t)(v->cursor & mask) * 2u;
            int32_t l, r;
            if (v->channels == 2u) { l = v->data[v->pos * 2u]; r = v->data[v->pos * 2u + 1u]; }
            else { l = r = v->data[v->pos]; }
            if (gain) {
                l = (int32_t)(((int64_t)l * gain) >> 15);
                r = (int32_t)(((int64_t)r * gain) >> 15);
                m->ring[idx] = saturate((int32_t)m->ring[idx] + l, &m->clipped);
                m->ring[idx + 1u] = saturate((int32_t)m->ring[idx + 1u] + r, &m->clipped);
            }
            ++v->cursor;
            ++v->pos;
        }
        if (v->cursor > first) { m->mixed_frames += v->cursor - first; dirty(m, first, v->cursor); }
        if (v->pos >= v->frames && v->cursor <= m->played) { v->active = 0; v->data = 0; }
    }
}

uint32_t mint_mixer_start(MINT_MIXER *m, const int16_t *data, uint32_t frames, uint32_t channels,
                          uint32_t gain_q15, int32_t pool_slot) {
    if (!m || !m->ring || !data || !frames || (channels != 1u && channels != 2u) || gain_q15 > MINT_MIXER_UNITY)
        return 0;
    for (uint32_t vi = 0; vi < MINT_MIXER_VOICES; ++vi) {
        MINT_MIXER_VOICE *v = &m->voices[vi];
        if (v->active) continue;
        if (++m->next_id == 0u) m->next_id = 1u;
        v->data = data; v->frames = frames; v->channels = channels; v->pos = 0;
        v->start = v->cursor = m->played + m->safety_frames;
        v->gain_q15 = gain_q15; v->id = m->next_id; v->pool_slot = pool_slot; v->active = 1u;
        ++m->started;
        return v->id;
    }
    return 0;
}

void mint_mixer_rewind(MINT_MIXER *m) {
    if (!m || !m->ring) return;
    const uint32_t mask = m->ring_frames - 1u;
    const uint64_t from = m->played + m->safety_frames, to = m->played + m->lead_frames;
    for (uint64_t f = from; f < to; ++f) {
        uint32_t idx = (uint32_t)(f & mask) * 2u;
        m->ring[idx] = 0; m->ring[idx + 1u] = 0;
    }
    dirty(m, from, to);
    for (uint32_t vi = 0; vi < MINT_MIXER_VOICES; ++vi) {
        MINT_MIXER_VOICE *v = &m->voices[vi];
        if (!v->active || v->cursor <= from) continue;
        uint64_t target = v->start > from ? v->start : from;
        uint64_t back = v->cursor - target;
        if (back > v->pos) back = v->pos;
        v->pos -= (uint32_t)back;
        v->cursor -= back;
    }
    mint_mixer_fill(m);
}

void mint_mixer_set_master(MINT_MIXER *m, uint32_t gain_q15, uint32_t muted) {
    if (!m) return;
    if (gain_q15 > MINT_MIXER_UNITY) gain_q15 = MINT_MIXER_UNITY;
    if (m->master_q15 == gain_q15 && m->muted == (muted ? 1u : 0u)) return;
    m->master_q15 = gain_q15;
    m->muted = muted ? 1u : 0u;
    mint_mixer_rewind(m);
}

void mint_mixer_stop_all(MINT_MIXER *m) {
    if (!m) return;
    for (uint32_t vi = 0; vi < MINT_MIXER_VOICES; ++vi) { m->voices[vi].active = 0; m->voices[vi].data = 0; }
    mint_mixer_rewind(m);
}

uint32_t mint_mixer_active(const MINT_MIXER *m) {
    uint32_t n = 0;
    if (m) for (uint32_t vi = 0; vi < MINT_MIXER_VOICES; ++vi) n += m->voices[vi].active ? 1u : 0u;
    return n;
}

int mint_mixer_slot_busy(const MINT_MIXER *m, int32_t slot) {
    if (!m || slot < 0) return 0;
    for (uint32_t vi = 0; vi < MINT_MIXER_VOICES; ++vi)
        if (m->voices[vi].active && m->voices[vi].pool_slot == slot) return 1;
    return 0;
}

int mint_mixer_take_dirty(MINT_MIXER *m, uint32_t *first, uint32_t *count) {
    if (!m || !first || !count || m->dirty_hi <= m->dirty_lo) return 0;
    uint64_t n = m->dirty_hi - m->dirty_lo;
    if (n > m->ring_frames) n = m->ring_frames;
    *first = (uint32_t)(m->dirty_lo & (m->ring_frames - 1u));
    *count = (uint32_t)n;
    m->dirty_lo = m->dirty_hi = 0;
    return 1;
}
