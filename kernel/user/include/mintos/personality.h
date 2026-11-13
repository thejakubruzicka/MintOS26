#ifndef MINTOS26_USER_PERSONALITY_H
#define MINTOS26_USER_PERSONALITY_H
/* 26.5.014.0 userspace wrappers for the personality syscalls (sound and the
 * boot animation hand-off). Implemented in kernel/user/personality_runtime.c
 * so the shared runtime stays untouched. */
#include <stddef.h>
#include <stdint.h>
#include <uapi/mintos/audio.h>

int64_t mint_audio_info(MINT_AUDIO_INFO *out);
/* Plays a built-in MINT_SOUND_*; gain 0..1000. Returns a voice id or -errno. */
int64_t mint_audio_play(uint32_t sound, uint32_t gain_permille);
/* Queues signed 16-bit 48 kHz PCM (copied by the kernel); channels 1 or 2. */
int64_t mint_audio_queue(const int16_t *pcm, uint32_t frames, uint32_t channels, uint32_t gain_permille);
/* volume 0..100 or MINT_AUDIO_VOLUME_KEEP; mute 0, 1 or MINT_AUDIO_MUTE_KEEP. */
int64_t mint_audio_volume(uint32_t volume, uint32_t mute);
int64_t mint_audio_control(uint32_t op, uint32_t token);
int64_t mint_boot_splash(uint32_t op);
int64_t mint_system_status_get(MINT_SYSTEM_STATUS *out);
#endif
