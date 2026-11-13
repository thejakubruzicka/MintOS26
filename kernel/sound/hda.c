/* Intel High Definition Audio — polled kernel driver (26.5.014.0).
 *
 * Boot does a read-only PCI match (class 0x0403). Controllers under a
 * hypervisor (QEMU ich9-intel-hda / VirtualBox / VMware) are started by the
 * audio task; real hardware such as the OptiPlex 7040's Realtek ALC3234 is
 * experimental and starts only on an explicit request (Settings > Sound or
 * Terminal `sound start`), like the I219 NIC. One output stream (48 kHz,
 * 16-bit stereo) plays a 256 KiB DMA ring that the kernel mixer keeps fed
 * from the PIT interrupt, so sounds keep playing while MintDE is busy.
 * Stop order: stream off, CORB/RIRB off, link reset, bus mastering off and
 * confirmed, and only then is DMA memory released. */
#include <stdint.h>
#include <stddef.h>
#include "../include/hda.h"
#include "../include/audio_mixer.h"
#include "../include/uapi/mintos/audio.h"
#include "../include/uapi/mintos/syscall.h"
#include "../include/pci.h"
#include "../include/physmem.h"
#include "../include/vmalloc.h"
#include "../include/serial.h"
#include "../include/log.h"
#include "../include/task.h"
#include "../include/arch.h"

#define AUDIO_RING_FRAMES 65536u                    /* 1.37 s, 256 KiB */
#define AUDIO_RING_BYTES (AUDIO_RING_FRAMES * 4u)
#define AUDIO_RING_PAGES (AUDIO_RING_BYTES / 4096u)
#define AUDIO_LEAD_FRAMES 24000u                    /* mix 500 ms ahead */
#define AUDIO_SAFETY_FRAMES 480u                    /* start new sounds 10 ms after the play position */
#define AUDIO_BDL_ENTRIES 8u
#define AUDIO_STREAM_TAG 1u
#define AUDIO_POOL_SLOTS 4u
#define AUDIO_POOL_SLOT_BYTES (96000u * 2u)         /* MINT_AUDIO_QUEUE_MAX_SAMPLES int16 */
#define AUDIO_POOL_PAGES ((AUDIO_POOL_SLOTS * AUDIO_POOL_SLOT_BYTES + 4095u) / 4096u)
#define DMA_LIMIT 0x100000000ull


static MINT_AUDIO_INFO info;
static MINT_HDA_LINK link;
static MINT_HDA_CODEC codec;
static MINT_HDA_PLAN plan;
static MINT_MIXER mixer;
static const MINT_PCI_DEVICE *device;
static volatile uint8_t *mmio;
static uint32_t stream_sd;
static uint64_t control_pa, ring_pa, pool_pa;      /* 0 when not owned */
static volatile uint32_t audio_running, in_service;
static uint32_t volume = 70u, muted;
static int32_t pending_sound = -1;
static uint32_t pending_gain;

/* ------------------------------------------------------------ built-in sounds (sounds.S) */
#define SOUND_DECL(n) extern const int16_t mint_sound_##n##_start[] __attribute__((weak)); \
                      extern const int16_t mint_sound_##n##_end[] __attribute__((weak));
SOUND_DECL(startup) SOUND_DECL(trash) SOUND_DECL(empty_trash) SOUND_DECL(notification)
SOUND_DECL(volume) SOUND_DECL(screenshot) SOUND_DECL(alert_mint) SOUND_DECL(alert_dewdrop)
SOUND_DECL(alert_fern) SOUND_DECL(alert_ripple) SOUND_DECL(alert_woodblock)
typedef struct { const int16_t *start, *end; uint32_t channels; } SOUND;
static SOUND sound_table(uint32_t id) {
#define S(n, ch) (SOUND){mint_sound_##n##_start, mint_sound_##n##_end, ch}
    switch (id) {
    case MINT_SOUND_STARTUP: return S(startup, 2u);
    case MINT_SOUND_TRASH: return S(trash, 1u);
    case MINT_SOUND_EMPTY_TRASH: return S(empty_trash, 1u);
    case MINT_SOUND_NOTIFICATION: return S(notification, 2u);
    case MINT_SOUND_VOLUME: return S(volume, 1u);
    case MINT_SOUND_SCREENSHOT: return S(screenshot, 1u);
    case MINT_SOUND_ALERT_MINT: return S(alert_mint, 1u);
    case MINT_SOUND_ALERT_DEWDROP: return S(alert_dewdrop, 1u);
    case MINT_SOUND_ALERT_FERN: return S(alert_fern, 1u);
    case MINT_SOUND_ALERT_RIPPLE: return S(alert_ripple, 1u);
    case MINT_SOUND_ALERT_WOODBLOCK: return S(alert_woodblock, 1u);
    default: return (SOUND){0, 0, 0};
    }
#undef S
}

/* ------------------------------------------------------------ helpers */
static void copy_str(char *d, size_t cap, const char *s) {
    size_t i = 0;
    while (s && s[i] && i + 1u < cap) { d[i] = s[i]; ++i; }
    d[i] = 0;
}
static void status(const char *s) { copy_str(info.status, sizeof(info.status), s); }
static void zero(void *p, size_t n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }
static uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static void irq_restore(uint64_t flags) { if (flags & 0x200u) __asm__ volatile("sti" ::: "memory"); }
static int hypervisor_present(void) {
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (c >> 31) & 1u;
}

/* MMIO register access for hda_core.c. */
static uint32_t hw_read(void *ctx, uint32_t off, uint32_t size) {
    (void)ctx;
    if (!mmio || off >= 0x4000u) return 0xFFFFFFFFu;
    if (size == 1u) return *(volatile uint8_t *)(mmio + off);
    if (size == 2u) return *(volatile uint16_t *)(mmio + off);
    return *(volatile uint32_t *)(mmio + off);
}
static void hw_write(void *ctx, uint32_t off, uint32_t size, uint32_t v) {
    (void)ctx;
    if (!mmio || off >= 0x4000u) return;
    if (size == 1u) *(volatile uint8_t *)(mmio + off) = (uint8_t)v;
    else if (size == 2u) *(volatile uint16_t *)(mmio + off) = (uint16_t)v;
    else *(volatile uint32_t *)(mmio + off) = v;
}
static uint64_t hw_ticks(void *ctx) { (void)ctx; return mint_timer_ticks(); }
static void hw_sleep(void *ctx, uint32_t t) { (void)ctx; mint_task_sleep_ticks(t); }
/* Write CPU cache lines back to RAM so a non-snooping controller sees them
   (and drop stale lines before reading what the controller wrote). */
static void hw_flush(void *ctx, const void *p, uint32_t bytes) {
    (void)ctx;
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63u, end = (uintptr_t)p + bytes;
    for (; a < end; a += 64u) __asm__ volatile("clflush (%0)" :: "r"((const void *)a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}
static const MINT_HDA_HW hw = {0, hw_read, hw_write, hw_ticks, hw_sleep, hw_flush};

static uint32_t pci_command(void) {
    return mint_pci_read_config32(device->bus, device->device, device->function, 0x04u) & 0xFFFFu;
}
static void pci_set_command(uint32_t value) {
    (void)mint_pci_write_config32(device->bus, device->device, device->function, 0x04u, value & 0xFFFFu);
}

/* ------------------------------------------------------------ probe */
void mint_hda_probe(void) {
    zero(&info, sizeof(info));
    info.version = MINT_AUDIO_INFO_VERSION;
    info.sample_rate = MINT_AUDIO_SAMPLE_RATE; info.channels = 2u; info.bits = 16u;
    info.volume = volume;
    info.ring_frames = AUDIO_RING_FRAMES; info.lead_frames = AUDIO_LEAD_FRAMES;
    const MINT_PCI_INVENTORY *inv = mint_pci_get_inventory();
    for (uint32_t i = 0; inv && i < inv->devices_stored && i < MINT_PCI_MAX_DEVICES; ++i) {
        const MINT_PCI_DEVICE *d = &inv->devices[i];
        if (d->class_code == 0x04u && d->subclass == 0x03u) { device = d; break; }
    }
    if (!device) {
        info.state = MINT_AUDIO_ABSENT;
        status("No High Definition Audio controller found");
        mint_serial_write("[audio] no HDA controller (PCI class 0x0403); sound unavailable\n");
        return;
    }
    info.state = MINT_AUDIO_FOUND;
    info.pci_vendor = device->vendor_id; info.pci_device = device->device_id;
    info.pci_bus = device->bus; info.pci_slot = device->device; info.pci_function = device->function;
    info.experimental = hypervisor_present() ? 0u : 1u;
    status(info.experimental ? "Detected; experimental, start it in Settings > Sound" : "Detected; starting");
    mint_serial_write("[audio] HDA controller "); mint_serial_hex64(device->vendor_id);
    mint_serial_write(":"); mint_serial_hex64(device->device_id);
    mint_serial_write(info.experimental ? " on real hardware: experimental, idle until started (bus mastering off)\n"
                                        : " under a hypervisor: the audio task will start it\n");
}

/* ------------------------------------------------------------ stop / release */
static void release_dma(void) {
    if (control_pa) { (void)mint_physmem_release_contiguous(control_pa, 1u); control_pa = 0; }
    if (ring_pa) { (void)mint_physmem_release_contiguous(ring_pa, AUDIO_RING_PAGES); ring_pa = 0; }
    if (pool_pa) { (void)mint_physmem_release_contiguous(pool_pa, AUDIO_POOL_PAGES); pool_pa = 0; }
}

/* Returns 1 when the controller is quiet and DMA memory was released. */
static int quiesce(void) {
    uint64_t flags = irq_save();
    audio_running = 0;
    irq_restore(flags);
    if (mmio) {
        if (stream_sd) (void)mint_hda_stream_run(&hw, stream_sd, 0);
        mint_hda_stop_corb_rirb(&hw, &link);
        hw_write(0, HDA_DPLBASE, 4u, 0u);
        hw_write(0, HDA_GCTL, 4u, hw_read(0, HDA_GCTL, 4u) & ~HDA_GCTL_CRST);   /* hold the link in reset */
    }
    if (device) {
        pci_set_command(pci_command() & ~0x0004u);
        if (pci_command() & 0x0004u) {
            mint_serial_write("[audio] bus mastering did not turn off; DMA memory is kept reserved\n");
            mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_AUDIO, "HDA bus mastering stuck on; DMA memory not freed");
            return 0;
        }
    }
    release_dma();
    return 1;
}

static int fail(const char *why) {
    status(why);
    info.state = MINT_AUDIO_FAILED;
    mint_serial_write("[audio] start failed: "); mint_serial_write(why); mint_serial_write("\n");
    mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_AUDIO, why);
    (void)quiesce();
    return 0;
}

int mint_hda_stop(void) {
    if (info.state != MINT_AUDIO_RUNNING && info.state != MINT_AUDIO_FAILED) return info.state == MINT_AUDIO_STOPPED;
    int ok = quiesce();
    info.state = ok ? MINT_AUDIO_STOPPED : MINT_AUDIO_FAILED;
    status(ok ? "Stopped; DMA released" : "Stop incomplete; see logs");
    mint_serial_write(ok ? "[audio] stream stopped, bus mastering off, DMA released\n"
                         : "[audio] stop incomplete\n");
    mint_log_emit(ok ? MINT_LOG_INFO : MINT_LOG_ERROR, MINT_LOG_COMPONENT_AUDIO,
                  ok ? "HDA stopped cleanly; DMA memory released" : "HDA stop incomplete");
    return ok;
}

/* ------------------------------------------------------------ mixer service */
static void flush_dirty(void) {
    uint32_t first, count;
    if (!mint_mixer_take_dirty(&mixer, &first, &count)) return;
    uint32_t head = AUDIO_RING_FRAMES - first;
    if (head > count) head = count;
    hw_flush(0, (const uint8_t *)(uintptr_t)ring_pa + (uint64_t)first * 4u, head * 4u);
    if (count > head) hw_flush(0, (const void *)(uintptr_t)ring_pa, (count - head) * 4u);
}

/* Interrupts must be off (PIT IRQ or an irq_save section). */
static void service(void) {
    if (!audio_running || in_service) return;
    in_service = 1u;
    uint32_t lpib = mint_hda_stream_position(&hw, stream_sd);
    if (lpib < AUDIO_RING_BYTES) (void)mint_mixer_advance(&mixer, lpib / 4u);
    mint_mixer_fill(&mixer);
    flush_dirty();
    info.frames_played = mixer.played;
    info.underruns = mixer.underruns;
    info.clipped_samples = mixer.clipped;
    info.sounds_started = mixer.started;
    info.active_voices = mint_mixer_active(&mixer);
    in_service = 0;
}
void mint_audio_timer_tick(void) { service(); }

/* ------------------------------------------------------------ start */
static int pick_codec(void) {
    for (uint32_t cad = 0; cad < 15u; ++cad) {
        if (!(link.codec_mask & (1u << cad))) continue;
        if (!mint_hda_codec_scan(&hw, &link, cad, &codec)) continue;
        mint_serial_write("[audio] codec "); mint_serial_dec64(cad); mint_serial_write(" vendor ");
        mint_serial_hex64(codec.vendor_id); mint_serial_write(" ("); mint_serial_write(mint_hda_codec_name(codec.vendor_id));
        mint_serial_write(") widgets="); mint_serial_dec64(codec.node_count); mint_serial_write("\n");
        if (mint_hda_plan_outputs(&codec, &plan) > 0) return 1;
        mint_serial_write("[audio] codec has no analog output path; trying the next one\n");
    }
    return 0;
}

int mint_hda_start(void) {
    if (!device) return 0;
    if (info.state == MINT_AUDIO_RUNNING) return 1;
    if (info.state == MINT_AUDIO_STARTING) return 0;
    info.state = MINT_AUDIO_STARTING;
    status("Starting");
    mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_AUDIO, "starting Intel HDA output driver");
    const MINT_PCI_BAR *bar = &device->bars[0];
    if ((bar->kind != MINT_PCI_BAR_MEMORY32 && bar->kind != MINT_PCI_BAR_MEMORY64) || !bar->address)
        return fail("BAR0 is not a usable MMIO range");
    if (!mmio) mmio = (volatile uint8_t *)mint_mmio_map(bar->address, 0x4000u);
    if (!mmio) return fail("could not map HDA registers");
    uint32_t command = pci_command();
    pci_set_command((command | 0x0002u) & ~0x0004u);             /* memory space, no DMA yet */
    if (device->vendor_id == 0x8086u) {                            /* TCSEL = TC0 (as Linux does) */
        uint32_t v = mint_pci_read_config32(device->bus, device->device, device->function, 0x44u);
        if (v & 7u) (void)mint_pci_write_config32(device->bus, device->device, device->function, 0x44u, v & ~7u);
    }
    if (hw_read(0, HDA_GCAP, 2u) == 0xFFFFu) return fail("HDA registers not responding");
    /* DMA memory: CORB (1 KiB) + RIRB (2 KiB) + BDL in one page; the ring; the PCM pool. */
    control_pa = mint_physmem_claim_contiguous_below(1u, DMA_LIMIT);
    ring_pa = mint_physmem_claim_contiguous_below(AUDIO_RING_PAGES, DMA_LIMIT);
    pool_pa = mint_physmem_claim_contiguous(AUDIO_POOL_PAGES);
    if (!control_pa || !ring_pa || !pool_pa) return fail("no DMA memory below 4 GiB");
    zero((void *)(uintptr_t)control_pa, 4096u);
    zero((void *)(uintptr_t)ring_pa, AUDIO_RING_BYTES);
    hw_flush(0, (void *)(uintptr_t)control_pa, 4096u);
    hw_flush(0, (void *)(uintptr_t)ring_pa, AUDIO_RING_BYTES);
    if (!mint_hda_reset_link(&hw, &link)) return fail("controller reset timed out");
    info.codec_mask = link.codec_mask;
    if (!link.codec_mask) return fail("no codec answered the link reset");
    if (!link.oss) return fail("controller has no output stream");
    /* DMA structures are owned and zeroed: only now allow bus mastering. */
    pci_set_command(command | 0x0006u);
    uint8_t *ctl = (uint8_t *)(uintptr_t)control_pa;
    if (!mint_hda_init_corb_rirb(&hw, &link, ctl, control_pa, ctl + 1024u, control_pa + 1024u)) {
        mint_serial_write("[audio] CORB/RIRB unavailable; using immediate command registers\n");
        link.corb_ready = 0;
    }
    if (!pick_codec()) return fail("no codec with a usable analog output");
    info.command_path = link.corb_ready ? 0u : 1u;
    info.codec_vendor = codec.vendor_id;
    info.codec_address = codec.cad;
    copy_str(info.codec_name, sizeof(info.codec_name), mint_hda_codec_name(codec.vendor_id));
    info.output_count = plan.count;
    for (uint32_t i = 0; i < plan.count && i < MINT_AUDIO_MAX_OUTPUTS; ++i) {
        const MINT_HDA_PATH *p = &plan.paths[i];
        info.output_pin[i] = p->nids[0];
        info.output_dac[i] = p->nids[p->length - 1u];
        info.output_kind[i] = p->kind;
        info.output_hops[i] = p->length;
        mint_serial_write("[audio] output path: pin "); mint_serial_dec64(p->nids[0]);
        for (uint32_t k = 1; k < p->length; ++k) { mint_serial_write(" -> "); mint_serial_dec64(p->nids[k]); }
        mint_serial_write(p->kind == HDA_DEV_SPEAKER ? " (speaker)\n" : p->kind == HDA_DEV_HP_OUT ? " (headphones)\n" : " (line out)\n");
    }
    uint16_t format = mint_hda_stream_format(48000u, 16u, 2u);
    if (!mint_hda_apply_plan(&hw, &link, &codec, &plan, AUDIO_STREAM_TAG, format))
        mint_serial_write("[audio] some codec verbs were not acknowledged; continuing\n");
    MINT_HDA_BDL_ENTRY *bdl = (MINT_HDA_BDL_ENTRY *)(ctl + 3072u);
    if (mint_hda_build_bdl(bdl, AUDIO_BDL_ENTRIES, ring_pa, AUDIO_RING_BYTES, AUDIO_BDL_ENTRIES) != AUDIO_BDL_ENTRIES)
        return fail("could not build the buffer descriptor list");
    hw_flush(0, bdl, AUDIO_BDL_ENTRIES * sizeof(*bdl));
    stream_sd = mint_hda_output_stream_offset(&link, 0u);
    if (!mint_hda_stream_setup(&hw, stream_sd, control_pa + 3072u, AUDIO_BDL_ENTRIES, AUDIO_RING_BYTES,
                               format, AUDIO_STREAM_TAG))
        return fail("output stream descriptor did not accept the setup");
    if (!mint_mixer_init(&mixer, (int16_t *)(uintptr_t)ring_pa, AUDIO_RING_FRAMES, AUDIO_LEAD_FRAMES, AUDIO_SAFETY_FRAMES))
        return fail("mixer initialization failed");
    mint_mixer_set_master(&mixer, mint_mixer_volume_gain(volume), muted);
    flush_dirty();
    if (muted) (void)mint_hda_set_mute(&hw, &link, &codec, &plan, 1);
    if (!mint_hda_stream_run(&hw, stream_sd, 1)) return fail("output stream did not start");
    uint64_t flags = irq_save();
    audio_running = 1u;
    info.state = MINT_AUDIO_RUNNING;
    irq_restore(flags);
    status(info.experimental ? "Running (experimental): 48 kHz 16-bit stereo" : "Running: 48 kHz 16-bit stereo");
    mint_serial_write("[audio] HDA running: "); mint_serial_write(info.codec_name);
    mint_serial_write(", outputs="); mint_serial_dec64(plan.count);
    mint_serial_write(link.corb_ready ? ", CORB/RIRB" : ", immediate commands");
    mint_serial_write(", 48 kHz 16-bit stereo, ring 256 KiB\n");
    mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_AUDIO, "HDA output stream running at 48 kHz 16-bit stereo");
    if (pending_sound >= 0) { (void)mint_audio_play_sound((uint32_t)pending_sound, pending_gain); pending_sound = -1; }
    return 1;
}

void mint_audio_task(void *argument) {
    (void)argument;
    if (!device || info.experimental) return;
    (void)mint_hda_start();
}

/* ------------------------------------------------------------ API used by syscalls */
int mint_audio_driver_state(void) { return (int)info.state; }

int mint_audio_get_info(MINT_AUDIO_INFO *out) {
    if (!out) return 0;
    uint64_t flags = irq_save();
    info.volume = volume; info.muted = muted;
    if (audio_running) info.active_voices = mint_mixer_active(&mixer);
    *out = info;
    irq_restore(flags);
    return 1;
}

int64_t mint_audio_play_sound(uint32_t sound, uint32_t gain_permille) {
    SOUND s = sound_table(sound);
    if (!s.start || !s.end || s.end <= s.start) return -MINT_EINVAL;
    if (gain_permille > 1000u) return -MINT_EINVAL;
    if (!audio_running) {
        if (info.state == MINT_AUDIO_STARTING || (info.state == MINT_AUDIO_FOUND && !info.experimental)) {
            pending_sound = (int32_t)sound; pending_gain = gain_permille;   /* play once the stream runs */
            return 0;
        }
        return -MINT_ENODEV;
    }
    uint32_t frames = (uint32_t)((uint64_t)(s.end - s.start) / s.channels);
    uint64_t flags = irq_save();
    service();
    uint32_t id = mint_mixer_start(&mixer, s.start, frames, s.channels, mint_mixer_permille_gain(gain_permille), -1);
    service();
    irq_restore(flags);
    return id ? (int64_t)id : -MINT_EBUSY;
}

/* User PCM is copied into one of four kernel-owned pool slots first. */
int16_t *mint_audio_pool_acquire(uint32_t samples, int32_t *slot_out) {
    if (!audio_running || !pool_pa || !slot_out || !samples || samples * 2u > AUDIO_POOL_SLOT_BYTES) return 0;
    uint64_t flags = irq_save();
    int32_t found = -1;
    for (int32_t s = 0; s < (int32_t)AUDIO_POOL_SLOTS; ++s) if (!mint_mixer_slot_busy(&mixer, s)) { found = s; break; }
    irq_restore(flags);
    if (found < 0) return 0;
    *slot_out = found;
    return (int16_t *)(uintptr_t)(pool_pa + (uint64_t)found * AUDIO_POOL_SLOT_BYTES);
}

int64_t mint_audio_pool_play(int32_t slot, uint32_t frames, uint32_t channels, uint32_t gain_permille) {
    if (!audio_running || slot < 0 || slot >= (int32_t)AUDIO_POOL_SLOTS || gain_permille > 1000u) return -MINT_EINVAL;
    const int16_t *pcm = (const int16_t *)(uintptr_t)(pool_pa + (uint64_t)slot * AUDIO_POOL_SLOT_BYTES);
    uint64_t flags = irq_save();
    service();
    uint32_t id = mint_mixer_start(&mixer, pcm, frames, channels, mint_mixer_permille_gain(gain_permille), slot);
    service();
    irq_restore(flags);
    return id ? (int64_t)id : -MINT_EBUSY;
}

int64_t mint_audio_set_volume(uint32_t new_volume, uint32_t mute) {
    if ((new_volume > 100u && new_volume != MINT_AUDIO_VOLUME_KEEP) || mute > MINT_AUDIO_MUTE_KEEP) return -MINT_EINVAL;
    uint32_t old_muted = muted;
    if (new_volume != MINT_AUDIO_VOLUME_KEEP) volume = new_volume;
    if (mute != MINT_AUDIO_MUTE_KEEP) muted = mute;
    info.volume = volume; info.muted = muted;
    if (!audio_running) return 0;
    uint64_t flags = irq_save();
    service();
    mint_mixer_set_master(&mixer, mint_mixer_volume_gain(volume), muted);
    flush_dirty();
    irq_restore(flags);
    if (old_muted != muted) (void)mint_hda_set_mute(&hw, &link, &codec, &plan, (int)muted);
    return 0;
}

int64_t mint_audio_silence(void) {
    if (!audio_running) return -MINT_ENODEV;
    uint64_t flags = irq_save();
    mint_mixer_stop_all(&mixer);
    flush_dirty();
    irq_restore(flags);
    return 0;
}
