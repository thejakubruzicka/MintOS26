#ifndef MINTOS26_AUDIO_MIXER_H
#define MINTOS26_AUDIO_MIXER_H
#include <stdint.h>

/* Kernel PCM mixer (26.5.014.0). Voices are mixed additively into a cyclic
 * stereo int16 ring that the HDA stream plays. Positions are absolute frame
 * counts: `played` is what the hardware has consumed; every voice keeps the
 * absolute frame of its next sample (`cursor`). Each service pass silences
 * the frames the hardware has just played and mixes voices up to
 * played + lead, so an underrun plays silence instead of stale audio and a
 * new voice starts `safety` frames after the play position (low latency
 * even with a long lead). Integer-only; host-tested in tools/test_audio_mixer.c. */
#define MINT_MIXER_VOICES 8u
#define MINT_MIXER_UNITY 32768u            /* Q15 gain 1.0 */

typedef struct mint_mixer_voice {
    const int16_t *data;
    uint32_t frames, channels, pos;
    uint64_t cursor;                       /* absolute output frame for data[pos] */
    uint64_t start;                        /* absolute output frame for data[0] */
    uint32_t gain_q15;
    uint32_t id;
    int32_t pool_slot;                     /* owner bookkeeping for user PCM, -1 if none */
    uint32_t active;
} MINT_MIXER_VOICE;

typedef struct mint_mixer {
    int16_t *ring;                         /* interleaved L/R */
    uint32_t ring_frames;                  /* power of two */
    uint32_t lead_frames, safety_frames;
    uint64_t played;
    uint32_t hw_frame;                     /* last observed hardware frame within the ring */
    uint32_t master_q15;
    uint32_t muted;
    uint32_t next_id;
    uint64_t underruns, clipped, started, mixed_frames;
    uint64_t dirty_lo, dirty_hi;           /* absolute frames written since the last take */
    MINT_MIXER_VOICE voices[MINT_MIXER_VOICES];
} MINT_MIXER;

int mint_mixer_init(MINT_MIXER *m, int16_t *ring, uint32_t ring_frames,
                    uint32_t lead_frames, uint32_t safety_frames);
/* Perceptual volume curve: 0..100 -> Q15 gain (quadratic, 100 = unity). */
uint32_t mint_mixer_volume_gain(uint32_t volume);
uint32_t mint_mixer_permille_gain(uint32_t permille);
/* The hardware is now at ring frame `pos`; returns frames advanced. */
uint32_t mint_mixer_advance(MINT_MIXER *m, uint32_t pos);
void mint_mixer_fill(MINT_MIXER *m);
/* Returns a voice id (> 0) or 0 when every voice is busy or arguments are bad. */
uint32_t mint_mixer_start(MINT_MIXER *m, const int16_t *data, uint32_t frames, uint32_t channels,
                          uint32_t gain_q15, int32_t pool_slot);
/* Re-render everything after the safety margin (after a volume change). */
void mint_mixer_rewind(MINT_MIXER *m);
void mint_mixer_set_master(MINT_MIXER *m, uint32_t gain_q15, uint32_t muted);
void mint_mixer_stop_all(MINT_MIXER *m);
uint32_t mint_mixer_active(const MINT_MIXER *m);
int mint_mixer_slot_busy(const MINT_MIXER *m, int32_t slot);
/* Ring frames [first, first+count) (mod ring) written since the last call. */
int mint_mixer_take_dirty(MINT_MIXER *m, uint32_t *first, uint32_t *count);
#endif
