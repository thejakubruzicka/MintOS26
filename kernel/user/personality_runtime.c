/* Userspace wrappers for the 26.5.014.0 personality syscalls (0x4D58-0x4D5F). */
#include "include/mintos/personality.h"

static inline int64_t sys4(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = a0;
    register uint64_t rsi __asm__("rsi") = a1;
    register uint64_t rdx __asm__("rdx") = a2;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile("int $0x80"
                     : "+a"(rax)
                     : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return (int64_t)rax;
}

int64_t mint_audio_info(MINT_AUDIO_INFO *out) {
    return sys4(MINT_SYS_AUDIO_INFO, (uint64_t)(uintptr_t)out, 0, 0, 0);
}
int64_t mint_audio_play(uint32_t sound, uint32_t gain_permille) {
    return sys4(MINT_SYS_AUDIO_PLAY_SOUND, sound, gain_permille, 0, 0);
}
int64_t mint_audio_queue(const int16_t *pcm, uint32_t frames, uint32_t channels, uint32_t gain_permille) {
    return sys4(MINT_SYS_AUDIO_QUEUE, (uint64_t)(uintptr_t)pcm, frames, channels, gain_permille);
}
int64_t mint_audio_volume(uint32_t volume, uint32_t mute) {
    return sys4(MINT_SYS_AUDIO_VOLUME, volume, mute, 0, 0);
}
int64_t mint_audio_control(uint32_t op, uint32_t token) {
    return sys4(MINT_SYS_AUDIO_CONTROL, op, token, 0, 0);
}
int64_t mint_system_status_get(MINT_SYSTEM_STATUS *out) {
    return sys4(MINT_SYS_SYSTEM_STATUS, (uint64_t)(uintptr_t)out, 0, 0, 0);
}
int64_t mint_boot_splash(uint32_t op) {
    return sys4(MINT_SYS_BOOT_SPLASH, op, 0, 0, 0);
}
