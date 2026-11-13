#ifndef MINTOS26_UAPI_AUDIO_H
#define MINTOS26_UAPI_AUDIO_H
#include <stdint.h>

/* 26.5.014.0 sound: Intel High Definition Audio output, a kernel mixer and
 * built-in MintOS sounds. Syscalls use the personality range 0x4D58-0x4D5F.
 * All PCM is signed 16-bit little-endian at 48 kHz; the output stream is
 * always 48 kHz 16-bit stereo. */
#define MINT_SYS_AUDIO_INFO        0x4D58u  /* arg0 = MINT_AUDIO_INFO * */
#define MINT_SYS_AUDIO_PLAY_SOUND  0x4D59u  /* arg0 = MINT_SOUND_*, arg1 = gain 0..1000 -> voice id */
#define MINT_SYS_AUDIO_QUEUE       0x4D5Au  /* arg0 = int16 *, arg1 = frames, arg2 = channels (1|2), arg3 = gain -> voice id */
#define MINT_SYS_AUDIO_VOLUME      0x4D5Bu  /* arg0 = volume 0..100 or MINT_AUDIO_VOLUME_KEEP, arg1 = 0 unmute, 1 mute, 2 keep */
#define MINT_SYS_AUDIO_CONTROL     0x4D5Cu  /* arg0 = MINT_AUDIO_OP_*, arg1 = MINT_AUDIO_START_TOKEN for START */
#define MINT_SYS_BOOT_SPLASH       0x4D5Du  /* arg0 = MINT_SPLASH_OP_* */
#define MINT_SYS_SYSTEM_STATUS     0x4D5Eu  /* arg0 = MINT_SYSTEM_STATUS * (Notification Center widget) */

#define MINT_AUDIO_INFO_VERSION 1u
#define MINT_AUDIO_SAMPLE_RATE 48000u
#define MINT_AUDIO_VOLUME_KEEP 0xFFFFu
#define MINT_AUDIO_MUTE_KEEP 2u
#define MINT_AUDIO_QUEUE_MAX_FRAMES 96000u     /* samples per request: 2 s mono or 1 s stereo */
#define MINT_AUDIO_START_TOKEN 0x48444121u     /* "HDA!": explicit start of an experimental codec */

enum mint_audio_state {
    MINT_AUDIO_ABSENT = 0,      /* no HDA controller (class 0x0403) */
    MINT_AUDIO_FOUND = 1,       /* detected; experimental hardware waits for an explicit start */
    MINT_AUDIO_STARTING = 2,
    MINT_AUDIO_RUNNING = 3,
    MINT_AUDIO_FAILED = 4,
    MINT_AUDIO_STOPPED = 5      /* stopped cleanly; DMA released */
};

enum mint_audio_op {
    MINT_AUDIO_OP_START = 1,    /* start the driver (needs MINT_AUDIO_START_TOKEN) */
    MINT_AUDIO_OP_STOP = 2,     /* stop stream, bus mastering off, then free DMA */
    MINT_AUDIO_OP_SILENCE = 3   /* stop every playing voice */
};

enum mint_splash_op {
    MINT_SPLASH_OP_FINISH = 1,  /* MintDE drew its first frame: stop the boot animation */
    MINT_SPLASH_OP_STATUS = 2   /* returns 1 while the boot animation is still running */
};

/* Built-in sounds (original, synthesised by tools/make_sounds.py). */
enum mint_sound_id {
    MINT_SOUND_STARTUP = 0,
    MINT_SOUND_TRASH = 1,
    MINT_SOUND_EMPTY_TRASH = 2,
    MINT_SOUND_NOTIFICATION = 3,
    MINT_SOUND_VOLUME = 4,
    MINT_SOUND_SCREENSHOT = 5,
    MINT_SOUND_ALERT_MINT = 6,       /* alert sounds: user's choice in Settings > Sound */
    MINT_SOUND_ALERT_DEWDROP = 7,
    MINT_SOUND_ALERT_FERN = 8,
    MINT_SOUND_ALERT_RIPPLE = 9,
    MINT_SOUND_ALERT_WOODBLOCK = 10,
    MINT_SOUND_COUNT = 11
};
#define MINT_SOUND_ALERT_FIRST MINT_SOUND_ALERT_MINT
#define MINT_SOUND_ALERT_COUNT 5u

enum mint_audio_output_kind {
    MINT_AUDIO_OUT_LINE = 0, MINT_AUDIO_OUT_SPEAKER = 1, MINT_AUDIO_OUT_HEADPHONE = 2
};

#define MINT_AUDIO_MAX_OUTPUTS 4u
typedef struct mint_audio_info {
    uint32_t version;
    uint32_t state;                   /* enum mint_audio_state */
    uint16_t pci_vendor, pci_device;
    uint8_t pci_bus, pci_slot, pci_function;
    uint8_t experimental;             /* 1: not a known-good virtual controller */
    uint32_t codec_vendor;            /* vendor << 16 | device */
    uint32_t codec_address;
    uint32_t codec_mask;              /* STATESTS: codecs that answered reset */
    uint32_t output_count;
    uint8_t output_pin[MINT_AUDIO_MAX_OUTPUTS];
    uint8_t output_kind[MINT_AUDIO_MAX_OUTPUTS];
    uint8_t output_dac[MINT_AUDIO_MAX_OUTPUTS];
    uint8_t output_hops[MINT_AUDIO_MAX_OUTPUTS];
    uint32_t sample_rate, channels, bits;
    uint32_t volume;                  /* 0..100 */
    uint32_t muted;
    uint32_t active_voices;
    uint32_t ring_frames, lead_frames;
    uint32_t command_path;            /* 0 = CORB/RIRB DMA, 1 = immediate command registers */
    uint32_t reserved;
    uint64_t frames_played;
    uint64_t underruns;
    uint64_t sounds_started;
    uint64_t clipped_samples;
    char status[64];
    char codec_name[32];
} MINT_AUDIO_INFO;

/* Whole-system status for the Notification Center widget. CPU load is
 * sampled on every PIT tick: a tick that interrupts the scheduler's idle
 * HLT counts as idle; the value covers the last second. */
#define MINT_SYSTEM_STATUS_VERSION 1u
typedef struct mint_system_status {
    uint32_t version;
    uint32_t cpu_load_permille;
    uint32_t tasks;
    uint32_t reserved;
    uint64_t memory_total_bytes;
    uint64_t memory_free_bytes;
    uint64_t uptime_ms;
} MINT_SYSTEM_STATUS;

#endif
