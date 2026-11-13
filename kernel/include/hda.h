#ifndef MINTOS26_HDA_H
#define MINTOS26_HDA_H
#include <stdint.h>
#include <stddef.h>

/* Intel High Definition Audio (HDA) — 26.5.014.0.
 *
 * hda_core.c holds everything that can be tested on the host against a
 * register-level fake: controller reset, CORB/RIRB and immediate command
 * transport, codec discovery, the widget-graph walk to an output path,
 * path programming (connection selects, amps, pin control, EAPD), the BDL
 * builder and stream descriptor setup. All register access goes through
 * MINT_HDA_HW so the same code drives real MMIO in the kernel.
 * hda.c is the polled kernel driver (PCI, DMA memory, mixer task). */

/* ---- controller registers */
#define HDA_GCAP      0x00u
#define HDA_VMIN      0x02u
#define HDA_VMAJ      0x03u
#define HDA_GCTL      0x08u
#define HDA_STATESTS  0x0Eu
#define HDA_INTCTL    0x20u
#define HDA_INTSTS    0x24u
#define HDA_CORBLBASE 0x40u
#define HDA_CORBUBASE 0x44u
#define HDA_CORBWP    0x48u
#define HDA_CORBRP    0x4Au
#define HDA_CORBCTL   0x4Cu
#define HDA_CORBSTS   0x4Du
#define HDA_CORBSIZE  0x4Eu
#define HDA_RIRBLBASE 0x50u
#define HDA_RIRBUBASE 0x54u
#define HDA_RIRBWP    0x58u
#define HDA_RINTCNT   0x5Au
#define HDA_RIRBCTL   0x5Cu
#define HDA_RIRBSTS   0x5Du
#define HDA_RIRBSIZE  0x5Eu
#define HDA_ICOI      0x60u
#define HDA_ICII      0x64u
#define HDA_ICIS      0x68u
#define HDA_DPLBASE   0x70u
#define HDA_DPUBASE   0x74u
#define HDA_SD_BASE   0x80u
#define HDA_SD_SIZE   0x20u
/* stream descriptor registers (offset from the descriptor base) */
#define HDA_SD_CTL    0x00u   /* 24-bit control + 8-bit status at +3 */
#define HDA_SD_STS    0x03u
#define HDA_SD_LPIB   0x04u
#define HDA_SD_CBL    0x08u
#define HDA_SD_LVI    0x0Cu
#define HDA_SD_FIFOS  0x10u
#define HDA_SD_FMT    0x12u
#define HDA_SD_BDPL   0x18u
#define HDA_SD_BDPU   0x1Cu

#define HDA_GCTL_CRST       0x00000001u
#define HDA_CORBRP_RST      0x8000u
#define HDA_RIRBWP_RST      0x8000u
#define HDA_CORBCTL_RUN     0x02u
#define HDA_RIRBCTL_DMAEN   0x02u
#define HDA_RIRBCTL_RINTCTL 0x01u
#define HDA_RIRBSTS_RINTFL  0x01u
#define HDA_RIRBSTS_OIS     0x04u
#define HDA_ICIS_ICB        0x0001u
#define HDA_ICIS_IRV        0x0002u
#define HDA_SD_CTL_SRST     0x000001u
#define HDA_SD_CTL_RUN      0x000002u
#define HDA_SD_STS_CLEAR    0x1Cu       /* BCIS | FIFOE | DESE */

/* ---- codec verbs (12-bit verbs carry an 8-bit payload, 4-bit verbs 16) */
#define HDA_VERB_GET_PARAMETER      0xF00u
#define HDA_VERB_GET_CONN_SELECT    0xF01u
#define HDA_VERB_GET_CONN_LIST      0xF02u
#define HDA_VERB_GET_PIN_CONTROL    0xF07u
#define HDA_VERB_GET_PIN_SENSE      0xF09u
#define HDA_VERB_GET_EAPD           0xF0Cu
#define HDA_VERB_GET_CONFIG_DEFAULT 0xF1Cu
#define HDA_VERB_GET_SUBSYSTEM_ID   0xF20u
#define HDA_VERB_SET_CONN_SELECT    0x701u
#define HDA_VERB_SET_POWER_STATE    0x705u
#define HDA_VERB_SET_STREAM_CHANNEL 0x706u
#define HDA_VERB_SET_PIN_CONTROL    0x707u
#define HDA_VERB_SET_EAPD           0x70Cu
#define HDA_VERB_FUNCTION_RESET     0x7FFu
#define HDA_VERB4_SET_FORMAT        0x2u
#define HDA_VERB4_SET_AMP           0x3u
#define HDA_VERB4_GET_FORMAT        0xAu
#define HDA_VERB4_GET_AMP           0xBu

/* GET_PARAMETER ids */
#define HDA_PAR_VENDOR_ID      0x00u
#define HDA_PAR_REVISION       0x02u
#define HDA_PAR_NODE_COUNT     0x04u
#define HDA_PAR_FG_TYPE        0x05u
#define HDA_PAR_AUDIO_CAPS     0x09u
#define HDA_PAR_PCM            0x0Au
#define HDA_PAR_STREAM_FORMATS 0x0Bu
#define HDA_PAR_PIN_CAPS       0x0Cu
#define HDA_PAR_IN_AMP_CAPS    0x0Du
#define HDA_PAR_CONN_LEN       0x0Eu
#define HDA_PAR_POWER_STATES   0x0Fu
#define HDA_PAR_OUT_AMP_CAPS   0x12u

/* audio widget capability fields */
#define HDA_WCAP_TYPE(c)     (((c) >> 20) & 0xFu)
#define HDA_WCAP_STEREO      (1u << 0)
#define HDA_WCAP_IN_AMP      (1u << 1)
#define HDA_WCAP_OUT_AMP     (1u << 2)
#define HDA_WCAP_AMP_OVRD    (1u << 3)
#define HDA_WCAP_CONN_LIST   (1u << 8)
#define HDA_WCAP_DIGITAL     (1u << 9)
#define HDA_WCAP_POWER       (1u << 10)
enum { HDA_WID_OUTPUT = 0, HDA_WID_INPUT = 1, HDA_WID_MIXER = 2, HDA_WID_SELECTOR = 3,
       HDA_WID_PIN = 4, HDA_WID_POWER = 5, HDA_WID_VOLUME = 6, HDA_WID_BEEP = 7, HDA_WID_VENDOR = 15 };
#define HDA_PINCAP_HP_DRIVE  (1u << 3)
#define HDA_PINCAP_OUTPUT    (1u << 4)
#define HDA_PINCAP_EAPD      (1u << 16)
#define HDA_PINCTL_OUT_EN    0x40u
#define HDA_PINCTL_HP_EN     0x80u
#define HDA_EAPD_ENABLE      0x02u
/* configuration default */
#define HDA_CFG_PORT(c)      (((c) >> 30) & 3u)   /* 0 jack, 1 none, 2 fixed, 3 both */
#define HDA_CFG_DEVICE(c)    (((c) >> 20) & 0xFu)
enum { HDA_DEV_LINE_OUT = 0, HDA_DEV_SPEAKER = 1, HDA_DEV_HP_OUT = 2 };
/* amp capabilities */
#define HDA_AMP_OFFSET(c)    ((c) & 0x7Fu)
#define HDA_AMP_STEPS(c)     (((c) >> 8) & 0x7Fu)
#define HDA_AMP_MUTE_CAP     (1u << 31)
/* SET_AMP payload */
#define HDA_AMP_SET_OUTPUT   0x8000u
#define HDA_AMP_SET_INPUT    0x4000u
#define HDA_AMP_SET_LEFT     0x2000u
#define HDA_AMP_SET_RIGHT    0x1000u
#define HDA_AMP_MUTE         0x0080u

#define MINT_HDA_MAX_NODES   80u
#define MINT_HDA_MAX_CONN    16u
#define MINT_HDA_MAX_PATH    6u
#define MINT_HDA_MAX_OUTPUTS 4u
#define MINT_HDA_CORB_ENTRIES 256u
#define MINT_HDA_RIRB_ENTRIES 256u

typedef struct mint_hda_hw {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t offset, uint32_t size);
    void (*write)(void *ctx, uint32_t offset, uint32_t size, uint32_t value);
    uint64_t (*ticks)(void *ctx);                    /* ~100 Hz */
    void (*sleep)(void *ctx, uint32_t ticks);
    void (*flush)(void *ctx, const void *p, uint32_t bytes);   /* write back CPU cache lines (may be 0) */
} MINT_HDA_HW;

typedef struct mint_hda_link {
    uint16_t gcap;
    uint8_t iss, oss, bss;
    uint8_t corb_entries, rirb_entries_log;       /* bookkeeping */
    uint32_t corb_size, rirb_size;                /* entries in use */
    volatile uint32_t *corb;                      /* CPU view */
    volatile uint32_t *rirb;                      /* 2 x u32 per entry */
    uint64_t corb_pa, rirb_pa;
    uint32_t corb_wp, rirb_rp;
    int corb_ready;                               /* 0: use immediate command registers */
    uint32_t codec_mask;                          /* STATESTS after reset */
    uint32_t timeouts, commands, unsolicited;
} MINT_HDA_LINK;

typedef struct mint_hda_widget {
    uint8_t nid, type, conn_count, flags;
    uint32_t caps, pin_caps, config, out_amp, in_amp;
    uint8_t conn[MINT_HDA_MAX_CONN];
} MINT_HDA_WIDGET;

typedef struct mint_hda_codec {
    uint32_t cad;
    uint32_t vendor_id, revision, subsystem;
    uint8_t afg, start_nid, node_count, reserved;
    uint32_t afg_out_amp, afg_in_amp;
    MINT_HDA_WIDGET w[MINT_HDA_MAX_NODES];
} MINT_HDA_CODEC;

typedef struct mint_hda_path {
    uint8_t length;                     /* nids[0] = pin ... nids[length-1] = DAC */
    uint8_t kind;                       /* HDA_DEV_* of the pin */
    uint8_t nids[MINT_HDA_MAX_PATH];
    uint8_t conn_index[MINT_HDA_MAX_PATH];   /* nids[i] selects nids[i+1] via this index */
} MINT_HDA_PATH;

typedef struct mint_hda_plan {
    uint32_t count;
    MINT_HDA_PATH paths[MINT_HDA_MAX_OUTPUTS];
} MINT_HDA_PLAN;

typedef struct __attribute__((packed)) mint_hda_bdl_entry {
    uint64_t address;
    uint32_t length;
    uint32_t flags;                     /* bit 0: interrupt on completion */
} MINT_HDA_BDL_ENTRY;

/* ---- pure helpers */
uint32_t mint_hda_verb(uint32_t cad, uint32_t nid, uint32_t verb, uint32_t payload);
/* Stream format word, e.g. 48 kHz 16-bit stereo = 0x0011. 0 if unsupported. */
uint16_t mint_hda_stream_format(uint32_t rate, uint32_t bits, uint32_t channels);
uint32_t mint_hda_output_stream_offset(const MINT_HDA_LINK *link, uint32_t index);
uint32_t mint_hda_build_bdl(MINT_HDA_BDL_ENTRY *bdl, uint32_t max_entries, uint64_t buffer_pa,
                            uint32_t bytes, uint32_t entries);
const MINT_HDA_WIDGET *mint_hda_widget(const MINT_HDA_CODEC *codec, uint32_t nid);
int mint_hda_plan_outputs(const MINT_HDA_CODEC *codec, MINT_HDA_PLAN *plan);
const char *mint_hda_codec_name(uint32_t vendor_id);

/* ---- controller and codec operations (bounded waits, return 1 on success) */
int mint_hda_reset_link(const MINT_HDA_HW *hw, MINT_HDA_LINK *link);
int mint_hda_init_corb_rirb(const MINT_HDA_HW *hw, MINT_HDA_LINK *link,
                            void *corb, uint64_t corb_pa, void *rirb, uint64_t rirb_pa);
void mint_hda_stop_corb_rirb(const MINT_HDA_HW *hw, MINT_HDA_LINK *link);
int mint_hda_command(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, uint32_t verb, uint32_t *response);
int mint_hda_codec_scan(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, uint32_t cad, MINT_HDA_CODEC *codec);
int mint_hda_apply_plan(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, const MINT_HDA_CODEC *codec,
                        const MINT_HDA_PLAN *plan, uint32_t stream_tag, uint16_t format);
int mint_hda_set_mute(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, const MINT_HDA_CODEC *codec,
                      const MINT_HDA_PLAN *plan, int mute);
int mint_hda_stream_setup(const MINT_HDA_HW *hw, uint32_t sd, uint64_t bdl_pa, uint32_t entries,
                          uint32_t cyclic_bytes, uint16_t format, uint32_t stream_tag);
int mint_hda_stream_run(const MINT_HDA_HW *hw, uint32_t sd, int run);
uint32_t mint_hda_stream_position(const MINT_HDA_HW *hw, uint32_t sd);

/* ---- kernel driver (hda.c) */
#ifndef MINT_HDA_HOST_TEST
struct mint_audio_info;
void mint_hda_probe(void);                  /* boot: read-only PCI match */
void mint_audio_task(void *argument);       /* starts known-good controllers, runs the mixer */
int mint_hda_start(void);                   /* explicit start (experimental hardware) */
int mint_hda_stop(void);                    /* clean stop: stream off, bus master off, free DMA */
int mint_audio_get_info(struct mint_audio_info *out);
int mint_audio_driver_state(void);
int64_t mint_audio_play_sound(uint32_t sound, uint32_t gain_permille);
int64_t mint_audio_queue_pcm(const int16_t *pcm, uint32_t frames, uint32_t channels, uint32_t gain_permille);
int64_t mint_audio_set_volume(uint32_t volume, uint32_t mute);
int64_t mint_audio_silence(void);
/* User PCM: copy into a kernel-owned pool slot, then play it. */
int16_t *mint_audio_pool_acquire(uint32_t samples, int32_t *slot_out);
int64_t mint_audio_pool_play(int32_t slot, uint32_t frames, uint32_t channels, uint32_t gain_permille);
void mint_audio_timer_tick(void);            /* called from the PIT hub: keeps the ring fed */
struct mint_system_status;
void mint_personality_timer_init(void);      /* system_status.c: installs the PIT hub */
int mint_system_status(struct mint_system_status *out);
#endif
#endif
