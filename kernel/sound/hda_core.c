/* Intel High Definition Audio — controller/codec logic shared by the kernel
 * driver (hda.c) and the host test (tools/test_hda.c). Every wait is
 * bounded; nothing here allocates memory or touches PCI. */
#include <stdint.h>
#include <stddef.h>
#include "../include/hda.h"

static uint32_t rd(const MINT_HDA_HW *hw, uint32_t off, uint32_t size) { return hw->read(hw->ctx, off, size); }
static void wr(const MINT_HDA_HW *hw, uint32_t off, uint32_t size, uint32_t v) { hw->write(hw->ctx, off, size, v); }
static uint64_t now(const MINT_HDA_HW *hw) { return hw->ticks ? hw->ticks(hw->ctx) : 0u; }
static void nap(const MINT_HDA_HW *hw, uint32_t t) { if (hw->sleep) hw->sleep(hw->ctx, t); }
static void flush(const MINT_HDA_HW *hw, const volatile void *p, uint32_t n) {
    if (hw->flush) hw->flush(hw->ctx, (const void *)p, n);
}
static void zero(volatile void *p, size_t n) { volatile uint8_t *b = (volatile uint8_t *)p; while (n--) *b++ = 0; }

/* Poll until (reg & mask) == want: a short busy phase (codecs answer in
 * microseconds), then one-tick sleeps up to limit_ticks. The guard keeps the
 * loop finite even if the tick source stalls. */
static int wait_bits(const MINT_HDA_HW *hw, uint32_t off, uint32_t size, uint32_t mask,
                     uint32_t want, uint32_t limit_ticks) {
    for (uint32_t spin = 0; spin < 512u; ++spin) {
        if ((rd(hw, off, size) & mask) == want) return 1;
        __asm__ volatile("" ::: "memory");
    }
    uint64_t start = now(hw);
    for (uint32_t guard = 0; guard < limit_ticks * 4u + 8u; ++guard) {
        if ((rd(hw, off, size) & mask) == want) return 1;
        if (now(hw) - start >= limit_ticks) break;
        nap(hw, 1u);
    }
    return (rd(hw, off, size) & mask) == want;
}

/* ------------------------------------------------------------ pure helpers */
uint32_t mint_hda_verb(uint32_t cad, uint32_t nid, uint32_t verb, uint32_t payload) {
    uint32_t v = ((cad & 0xFu) << 28) | ((nid & 0x7Fu) << 20);
    if (verb <= 0xFu) return v | (verb << 16) | (payload & 0xFFFFu);
    return v | ((verb & 0xFFFu) << 8) | (payload & 0xFFu);
}

uint16_t mint_hda_stream_format(uint32_t rate, uint32_t bits, uint32_t channels) {
    uint32_t base = 0, mult = 0, div = 0, b;
    switch (rate) {
    case 48000u: break;
    case 44100u: base = 1; break;
    case 96000u: mult = 1; break;
    case 88200u: base = 1; mult = 1; break;
    case 192000u: mult = 3; break;
    case 32000u: mult = 1; div = 2; break;
    case 24000u: div = 1; break;
    case 22050u: base = 1; div = 1; break;
    case 16000u: div = 2; break;
    case 11025u: base = 1; div = 3; break;
    case 8000u: div = 5; break;
    default: return 0;
    }
    switch (bits) {
    case 8u: b = 0; break; case 16u: b = 1; break; case 20u: b = 2; break;
    case 24u: b = 3; break; case 32u: b = 4; break;
    default: return 0;
    }
    if (channels < 1u || channels > 16u) return 0;
    return (uint16_t)((base << 14) | (mult << 11) | (div << 8) | (b << 4) | (channels - 1u));
}

uint32_t mint_hda_output_stream_offset(const MINT_HDA_LINK *link, uint32_t index) {
    if (!link || index >= link->oss) return 0;
    return HDA_SD_BASE + ((uint32_t)link->iss + index) * HDA_SD_SIZE;
}

/* Split a cyclic buffer into equal BDL entries. Every entry and the buffer
 * itself must be 128-byte aligned, and HDA needs at least two entries. */
uint32_t mint_hda_build_bdl(MINT_HDA_BDL_ENTRY *bdl, uint32_t max_entries, uint64_t buffer_pa,
                            uint32_t bytes, uint32_t entries) {
    if (!bdl || entries < 2u || entries > 256u || entries > max_entries || !bytes ||
        (buffer_pa & 127u) || bytes % entries) return 0;
    uint32_t chunk = bytes / entries;
    if (chunk & 127u) return 0;
    for (uint32_t i = 0; i < entries; ++i) {
        bdl[i].address = buffer_pa + (uint64_t)i * chunk;
        bdl[i].length = chunk;
        bdl[i].flags = 0;          /* polled: no interrupt on completion */
    }
    return entries;
}

const MINT_HDA_WIDGET *mint_hda_widget(const MINT_HDA_CODEC *codec, uint32_t nid) {
    if (!codec || nid < codec->start_nid || nid >= (uint32_t)codec->start_nid + codec->node_count) return 0;
    return &codec->w[nid - codec->start_nid];
}

const char *mint_hda_codec_name(uint32_t vendor_id) {
    switch (vendor_id) {
    case 0x1AF40012u: return "QEMU HDA output";
    case 0x1AF40022u: return "QEMU HDA duplex";
    case 0x1AF40032u: return "QEMU HDA micro";
    case 0x10EC0255u: return "Realtek ALC255 / ALC3234";
    case 0x10EC0256u: return "Realtek ALC256";
    case 0x10EC0236u: return "Realtek ALC236";
    case 0x10EC0662u: return "Realtek ALC662";
    case 0x10EC0887u: return "Realtek ALC887";
    case 0x10EC0892u: return "Realtek ALC892";
    case 0x80862809u: return "Intel Skylake HDMI";
    case 0x8086280Bu: return "Intel Kaby Lake HDMI";
    default: break;
    }
    switch (vendor_id >> 16) {
    case 0x10ECu: return "Realtek codec";
    case 0x8086u: return "Intel codec";
    case 0x1AF4u: return "QEMU codec";
    case 0x14F1u: return "Conexant codec";
    case 0x1106u: return "VIA codec";
    case 0x111Du: case 0x92F1u: return "IDT codec";
    case 0x10DEu: return "NVIDIA HDMI codec";
    case 0x1002u: return "AMD HDMI codec";
    case 0x80EEu: return "VirtualBox codec";
    default: return "HDA codec";
    }
}

/* ------------------------------------------------------------ link reset */
int mint_hda_reset_link(const MINT_HDA_HW *hw, MINT_HDA_LINK *link) {
    if (!hw || !link) return 0;
    uint32_t gcap = rd(hw, HDA_GCAP, 2u);
    if (gcap == 0xFFFFu) return 0;
    link->gcap = (uint16_t)gcap;
    link->iss = (uint8_t)((gcap >> 8) & 0xFu);
    link->oss = (uint8_t)((gcap >> 12) & 0xFu);
    link->bss = (uint8_t)((gcap >> 3) & 0x1Fu);
    link->corb_ready = 0;
    /* Quiesce every DMA engine before the reset. */
    wr(hw, HDA_INTCTL, 4u, 0u);
    wr(hw, HDA_CORBCTL, 1u, 0u);
    wr(hw, HDA_RIRBCTL, 1u, 0u);
    uint32_t streams = (uint32_t)link->iss + link->oss + link->bss;
    if (streams > 30u) streams = 30u;
    for (uint32_t s = 0; s < streams; ++s) {
        uint32_t off = HDA_SD_BASE + s * HDA_SD_SIZE;
        wr(hw, off, 4u, rd(hw, off, 4u) & 0x00FFFFFFu & ~HDA_SD_CTL_RUN);
    }
    (void)wait_bits(hw, HDA_CORBCTL, 1u, HDA_CORBCTL_RUN, 0u, 5u);
    (void)wait_bits(hw, HDA_RIRBCTL, 1u, HDA_RIRBCTL_DMAEN, 0u, 5u);
    wr(hw, HDA_DPLBASE, 4u, 0u);
    wr(hw, HDA_DPUBASE, 4u, 0u);
    wr(hw, HDA_STATESTS, 2u, 0x7FFFu);
    wr(hw, HDA_GCTL, 4u, rd(hw, HDA_GCTL, 4u) & ~HDA_GCTL_CRST);
    if (!wait_bits(hw, HDA_GCTL, 4u, HDA_GCTL_CRST, 0u, 20u)) return 0;
    nap(hw, 1u);
    wr(hw, HDA_GCTL, 4u, rd(hw, HDA_GCTL, 4u) | HDA_GCTL_CRST);
    if (!wait_bits(hw, HDA_GCTL, 4u, HDA_GCTL_CRST, HDA_GCTL_CRST, 20u)) return 0;
    /* Codecs request enumeration within 521 us of leaving reset. */
    nap(hw, 2u);
    uint64_t start = now(hw);
    for (uint32_t guard = 0; guard < 20u; ++guard) {
        link->codec_mask = rd(hw, HDA_STATESTS, 2u) & 0x7FFFu;
        if (link->codec_mask || now(hw) - start >= 10u) break;
        nap(hw, 1u);
    }
    return 1;
}

int mint_hda_init_corb_rirb(const MINT_HDA_HW *hw, MINT_HDA_LINK *link,
                            void *corb, uint64_t corb_pa, void *rirb, uint64_t rirb_pa) {
    if (!hw || !link) return 0;
    link->corb_ready = 0;
    if (!corb || !rirb || (corb_pa & 127u) || (rirb_pa & 127u)) return 0;
    wr(hw, HDA_CORBCTL, 1u, 0u);
    wr(hw, HDA_RIRBCTL, 1u, 0u);
    if (!wait_bits(hw, HDA_CORBCTL, 1u, HDA_CORBCTL_RUN, 0u, 5u) ||
        !wait_bits(hw, HDA_RIRBCTL, 1u, HDA_RIRBCTL_DMAEN, 0u, 5u)) return 0;
    uint32_t cs = rd(hw, HDA_CORBSIZE, 1u), rs = rd(hw, HDA_RIRBSIZE, 1u);
    uint32_t csel = (cs & 0x40u) ? 2u : (cs & 0x20u) ? 1u : 0u;
    uint32_t rsel = (rs & 0x40u) ? 2u : (rs & 0x20u) ? 1u : 0u;
    link->corb_size = csel == 2u ? 256u : csel == 1u ? 16u : 2u;
    link->rirb_size = rsel == 2u ? 256u : rsel == 1u ? 16u : 2u;
    wr(hw, HDA_CORBSIZE, 1u, (cs & ~3u) | csel);
    wr(hw, HDA_RIRBSIZE, 1u, (rs & ~3u) | rsel);
    link->corb = (volatile uint32_t *)corb;
    link->rirb = (volatile uint32_t *)rirb;
    link->corb_pa = corb_pa;
    link->rirb_pa = rirb_pa;
    zero(corb, link->corb_size * 4u);
    zero(rirb, link->rirb_size * 8u);
    flush(hw, corb, link->corb_size * 4u);
    flush(hw, rirb, link->rirb_size * 8u);
    wr(hw, HDA_CORBLBASE, 4u, (uint32_t)corb_pa);
    wr(hw, HDA_CORBUBASE, 4u, (uint32_t)(corb_pa >> 32));
    /* Read-pointer reset handshake. Some controllers (and QEMU) clear the
       pointer immediately and never show the bit; that is accepted. */
    wr(hw, HDA_CORBRP, 2u, HDA_CORBRP_RST);
    (void)wait_bits(hw, HDA_CORBRP, 2u, HDA_CORBRP_RST, HDA_CORBRP_RST, 2u);
    wr(hw, HDA_CORBRP, 2u, 0u);
    if (!wait_bits(hw, HDA_CORBRP, 2u, HDA_CORBRP_RST, 0u, 5u)) return 0;
    if (rd(hw, HDA_CORBRP, 2u) & 0xFFu) return 0;
    wr(hw, HDA_CORBWP, 2u, 0u);
    link->corb_wp = 0;
    wr(hw, HDA_RIRBLBASE, 4u, (uint32_t)rirb_pa);
    wr(hw, HDA_RIRBUBASE, 4u, (uint32_t)(rirb_pa >> 32));
    wr(hw, HDA_RIRBWP, 2u, HDA_RIRBWP_RST);
    link->rirb_rp = 0;
    /* One response per "interrupt"; INTCTL stays 0, so the flag is only
       polled and cleared after every response (QEMU pauses CORB fetches
       until RINTFL is acknowledged when RINTCTL is set). */
    wr(hw, HDA_RINTCNT, 2u, 1u);
    wr(hw, HDA_RIRBSTS, 1u, HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_OIS);
    wr(hw, HDA_RIRBCTL, 1u, HDA_RIRBCTL_DMAEN | HDA_RIRBCTL_RINTCTL);
    wr(hw, HDA_CORBCTL, 1u, HDA_CORBCTL_RUN);
    if (!wait_bits(hw, HDA_CORBCTL, 1u, HDA_CORBCTL_RUN, HDA_CORBCTL_RUN, 5u) ||
        !wait_bits(hw, HDA_RIRBCTL, 1u, HDA_RIRBCTL_DMAEN, HDA_RIRBCTL_DMAEN, 5u)) {
        mint_hda_stop_corb_rirb(hw, link);
        return 0;
    }
    link->corb_ready = 1;
    return 1;
}

void mint_hda_stop_corb_rirb(const MINT_HDA_HW *hw, MINT_HDA_LINK *link) {
    if (!hw || !link) return;
    wr(hw, HDA_CORBCTL, 1u, 0u);
    wr(hw, HDA_RIRBCTL, 1u, 0u);
    (void)wait_bits(hw, HDA_CORBCTL, 1u, HDA_CORBCTL_RUN, 0u, 5u);
    (void)wait_bits(hw, HDA_RIRBCTL, 1u, HDA_RIRBCTL_DMAEN, 0u, 5u);
    link->corb_ready = 0;
}

static int immediate_command(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, uint32_t verb, uint32_t *response) {
    if (!wait_bits(hw, HDA_ICIS, 2u, HDA_ICIS_ICB, 0u, 5u)) { ++link->timeouts; return 0; }
    wr(hw, HDA_ICIS, 2u, HDA_ICIS_IRV);                 /* clear a stale "response valid" */
    wr(hw, HDA_ICOI, 4u, verb);
    wr(hw, HDA_ICIS, 2u, HDA_ICIS_ICB);
    if (!wait_bits(hw, HDA_ICIS, 2u, HDA_ICIS_ICB | HDA_ICIS_IRV, HDA_ICIS_IRV, 50u)) { ++link->timeouts; return 0; }
    *response = rd(hw, HDA_ICII, 4u);
    wr(hw, HDA_ICIS, 2u, HDA_ICIS_IRV);
    return 1;
}

int mint_hda_command(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, uint32_t verb, uint32_t *response) {
    uint32_t scratch;
    if (!hw || !link) return 0;
    if (!response) response = &scratch;
    ++link->commands;
    if (!link->corb_ready || !link->corb || !link->rirb || !link->corb_size || !link->rirb_size)
        return immediate_command(hw, link, verb, response);
    uint32_t wp = (link->corb_wp + 1u) % link->corb_size;
    link->corb[wp] = verb;
    flush(hw, &link->corb[wp], 4u);
    link->corb_wp = wp;
    wr(hw, HDA_CORBWP, 2u, wp);
    uint64_t start = 0;
    int started = 0;
    for (uint32_t round = 0; round < 512u + 400u; ++round) {
        uint32_t hw_wp = (rd(hw, HDA_RIRBWP, 2u) & 0xFFu) % link->rirb_size;
        while (link->rirb_rp != hw_wp) {
            uint32_t rp = (link->rirb_rp + 1u) % link->rirb_size;
            flush(hw, &link->rirb[rp * 2u], 8u);
            uint32_t value = link->rirb[rp * 2u], extra = link->rirb[rp * 2u + 1u];
            link->rirb_rp = rp;
            if (extra & 0x10u) { ++link->unsolicited; continue; }
            wr(hw, HDA_RIRBSTS, 1u, HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_OIS);
            *response = value;
            return 1;
        }
        if (rd(hw, HDA_RIRBSTS, 1u) & (HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_OIS))
            wr(hw, HDA_RIRBSTS, 1u, HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_OIS);
        if (round < 512u) { __asm__ volatile("" ::: "memory"); continue; }
        if (!started) { start = now(hw); started = 1; }
        if (now(hw) - start >= 50u) break;            /* 500 ms */
        nap(hw, 1u);
    }
    ++link->timeouts;
    /* A controller whose CORB never answers falls back to immediate commands. */
    if (link->timeouts >= 3u && link->commands <= 8u) {
        mint_hda_stop_corb_rirb(hw, link);
        return immediate_command(hw, link, verb, response);
    }
    return 0;
}

/* ------------------------------------------------------------ codec discovery */
static int param(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, uint32_t cad, uint32_t nid,
                 uint32_t id, uint32_t *out) {
    return mint_hda_command(hw, link, mint_hda_verb(cad, nid, HDA_VERB_GET_PARAMETER, id), out);
}

static void read_connections(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, uint32_t cad, MINT_HDA_WIDGET *w) {
    uint32_t len_par = 0;
    w->conn_count = 0;
    if (!param(hw, link, cad, w->nid, HDA_PAR_CONN_LEN, &len_par)) return;
    uint32_t long_form = (len_par >> 7) & 1u, len = len_par & 0x7Fu;
    uint32_t per = long_form ? 2u : 4u, bits = long_form ? 16u : 8u;
    uint32_t id_mask = long_form ? 0x7FFFu : 0x7Fu, range = long_form ? 0x8000u : 0x80u;
    uint32_t prev = 0, n = 0;
    for (uint32_t i = 0; i < len && n < MINT_HDA_MAX_CONN; i += per) {
        uint32_t resp = 0;
        if (!mint_hda_command(hw, link, mint_hda_verb(cad, w->nid, HDA_VERB_GET_CONN_LIST, i), &resp)) break;
        for (uint32_t j = 0; j < per && i + j < len && n < MINT_HDA_MAX_CONN; ++j) {
            uint32_t e = (resp >> (j * bits)) & ((1u << bits) - 1u), id = e & id_mask;
            if ((e & range) && prev && id > prev && id - prev < 64u) {
                for (uint32_t r = prev + 1u; r <= id && n < MINT_HDA_MAX_CONN; ++r) w->conn[n++] = (uint8_t)r;
            } else if (id && id < 128u) {
                w->conn[n++] = (uint8_t)id;
            }
            prev = id;
        }
    }
    w->conn_count = (uint8_t)n;
}

int mint_hda_codec_scan(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, uint32_t cad, MINT_HDA_CODEC *codec) {
    if (!hw || !link || !codec || cad > 14u) return 0;
    zero(codec, sizeof(*codec));
    codec->cad = cad;
    uint32_t v = 0, nodes = 0;
    if (!param(hw, link, cad, 0u, HDA_PAR_VENDOR_ID, &v) || !v || v == 0xFFFFFFFFu) return 0;
    codec->vendor_id = v;
    (void)param(hw, link, cad, 0u, HDA_PAR_REVISION, &codec->revision);
    if (!param(hw, link, cad, 0u, HDA_PAR_NODE_COUNT, &nodes)) return 0;
    uint32_t fg_start = (nodes >> 16) & 0xFFu, fg_count = nodes & 0xFFu;
    if (fg_count > 8u) fg_count = 8u;
    for (uint32_t i = 0; i < fg_count; ++i) {
        uint32_t type = 0;
        if (param(hw, link, cad, fg_start + i, HDA_PAR_FG_TYPE, &type) && (type & 0xFFu) == 1u) {
            codec->afg = (uint8_t)(fg_start + i);
            break;
        }
    }
    if (!codec->afg) return 0;
    (void)mint_hda_command(hw, link, mint_hda_verb(cad, codec->afg, HDA_VERB_SET_POWER_STATE, 0u), 0);
    (void)mint_hda_command(hw, link, mint_hda_verb(cad, codec->afg, HDA_VERB_GET_SUBSYSTEM_ID, 0u), &codec->subsystem);
    (void)param(hw, link, cad, codec->afg, HDA_PAR_OUT_AMP_CAPS, &codec->afg_out_amp);
    (void)param(hw, link, cad, codec->afg, HDA_PAR_IN_AMP_CAPS, &codec->afg_in_amp);
    if (!param(hw, link, cad, codec->afg, HDA_PAR_NODE_COUNT, &nodes)) return 0;
    uint32_t start = (nodes >> 16) & 0xFFu, count = nodes & 0xFFu;
    if (!start || start >= 128u) return 0;
    if (count > MINT_HDA_MAX_NODES) count = MINT_HDA_MAX_NODES;
    if (start + count > 128u) count = 128u - start;
    codec->start_nid = (uint8_t)start;
    codec->node_count = (uint8_t)count;
    for (uint32_t i = 0; i < count; ++i) {
        MINT_HDA_WIDGET *w = &codec->w[i];
        w->nid = (uint8_t)(start + i);
        if (!param(hw, link, cad, w->nid, HDA_PAR_AUDIO_CAPS, &w->caps)) continue;
        w->type = (uint8_t)HDA_WCAP_TYPE(w->caps);
        if (w->type == HDA_WID_PIN) {
            (void)param(hw, link, cad, w->nid, HDA_PAR_PIN_CAPS, &w->pin_caps);
            (void)mint_hda_command(hw, link, mint_hda_verb(cad, w->nid, HDA_VERB_GET_CONFIG_DEFAULT, 0u), &w->config);
        }
        if (w->caps & HDA_WCAP_OUT_AMP) {
            if (w->caps & HDA_WCAP_AMP_OVRD) (void)param(hw, link, cad, w->nid, HDA_PAR_OUT_AMP_CAPS, &w->out_amp);
            else w->out_amp = codec->afg_out_amp;
        }
        if (w->caps & HDA_WCAP_IN_AMP) {
            if (w->caps & HDA_WCAP_AMP_OVRD) (void)param(hw, link, cad, w->nid, HDA_PAR_IN_AMP_CAPS, &w->in_amp);
            else w->in_amp = codec->afg_in_amp;
        }
        if (w->caps & HDA_WCAP_CONN_LIST) read_connections(hw, link, cad, w);
    }
    return 1;
}

/* ------------------------------------------------------------ widget-graph walk */
static int routable(const MINT_HDA_WIDGET *w) {
    return w && !(w->caps & HDA_WCAP_DIGITAL) &&
           (w->type == HDA_WID_OUTPUT || w->type == HDA_WID_MIXER || w->type == HDA_WID_SELECTOR);
}

/* Breadth-first search from an output pin through mixers/selectors to an
   analog DAC (the preferred one when given): the shortest usable path. */
static int find_path(const MINT_HDA_CODEC *codec, uint32_t pin, uint32_t want_dac, MINT_HDA_PATH *out) {
    uint8_t parent[128], via[128], depth[128], seen[128], queue[128];
    uint32_t head = 0, tail = 0, found = 0;
    for (uint32_t i = 0; i < 128u; ++i) { parent[i] = via[i] = depth[i] = seen[i] = 0; }
    queue[tail++] = (uint8_t)pin; seen[pin] = 1;
    while (head < tail) {
        uint32_t n = queue[head++];
        const MINT_HDA_WIDGET *w = mint_hda_widget(codec, n);
        if (!w) continue;
        if (n != pin && w->type == HDA_WID_OUTPUT) {
            if (!want_dac || n == want_dac) { found = n; break; }
            continue;
        }
        if (n != pin && w->type != HDA_WID_MIXER && w->type != HDA_WID_SELECTOR) continue;
        if ((uint32_t)depth[n] + 1u >= MINT_HDA_MAX_PATH) continue;
        for (uint32_t i = 0; i < w->conn_count; ++i) {
            uint32_t c = w->conn[i];
            if (c >= 128u || seen[c] || !routable(mint_hda_widget(codec, c)) || tail >= 128u) continue;
            seen[c] = 1; parent[c] = (uint8_t)n; via[c] = (uint8_t)i; depth[c] = (uint8_t)(depth[n] + 1u);
            queue[tail++] = (uint8_t)c;
        }
    }
    if (!found) return 0;
    uint32_t length = (uint32_t)depth[found] + 1u;
    out->length = (uint8_t)length;
    for (uint32_t k = found;; k = parent[k]) {
        out->nids[depth[k]] = (uint8_t)k;
        if (k == pin) break;
        out->conn_index[depth[k] - 1u] = via[k];
    }
    out->conn_index[length - 1u] = 0;
    return 1;
}

static uint32_t pin_rank(const MINT_HDA_WIDGET *w) {
    switch (HDA_CFG_DEVICE(w->config)) {
    case HDA_DEV_LINE_OUT: return 0u;
    case HDA_DEV_SPEAKER: return 1u;
    case HDA_DEV_HP_OUT: return 2u;
    default: return 9u;
    }
}

static int output_pin(const MINT_HDA_WIDGET *w, int strict) {
    if (!w || w->type != HDA_WID_PIN || (w->caps & HDA_WCAP_DIGITAL) || !(w->pin_caps & HDA_PINCAP_OUTPUT)) return 0;
    if (!strict) return 1;
    return HDA_CFG_PORT(w->config) != 1u && pin_rank(w) < 9u;
}

int mint_hda_plan_outputs(const MINT_HDA_CODEC *codec, MINT_HDA_PLAN *plan) {
    if (!codec || !plan) return 0;
    for (size_t i = 0; i < sizeof(*plan); ++i) ((uint8_t *)plan)[i] = 0;
    /* Strict pass: pins whose configuration default says line out, speaker
       or headphone and that are connected, ranked in that order. If the
       firmware left no usable defaults, any analog output-capable pin. */
    for (int strict = 1; strict >= 0 && !plan->count; --strict) {
        uint32_t ranks = strict ? 3u : 1u;
        for (uint32_t rank = 0; rank < ranks; ++rank) {
            for (uint32_t i = 0; i < codec->node_count && plan->count < MINT_HDA_MAX_OUTPUTS; ++i) {
                const MINT_HDA_WIDGET *w = &codec->w[i];
                if (!output_pin(w, strict) || (strict && pin_rank(w) != rank)) continue;
                MINT_HDA_PATH path;
                for (size_t b = 0; b < sizeof(path); ++b) ((uint8_t *)&path)[b] = 0;
                uint32_t dac0 = plan->count ? plan->paths[0].nids[plan->paths[0].length - 1u] : 0u;
                if (!(dac0 && find_path(codec, w->nid, dac0, &path)) && !find_path(codec, w->nid, 0u, &path)) continue;
                path.kind = (uint8_t)(pin_rank(w) < 3u ? pin_rank(w) : (uint32_t)HDA_DEV_LINE_OUT);
                plan->paths[plan->count++] = path;
            }
        }
    }
    return (int)plan->count;
}

/* ------------------------------------------------------------ path programming */
static uint32_t amp_gain(uint32_t caps) {
    uint32_t offset = HDA_AMP_OFFSET(caps), steps = HDA_AMP_STEPS(caps);
    return offset > steps ? steps : offset;           /* 0 dB where the amp offers it */
}

static int cmd(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, uint32_t cad, uint32_t nid, uint32_t verb, uint32_t payload) {
    uint32_t r;
    return mint_hda_command(hw, link, mint_hda_verb(cad, nid, verb, payload), &r);
}

int mint_hda_apply_plan(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, const MINT_HDA_CODEC *codec,
                        const MINT_HDA_PLAN *plan, uint32_t stream_tag, uint16_t format) {
    if (!hw || !link || !codec || !plan || !plan->count || !stream_tag || stream_tag > 15u || !format) return 0;
    uint32_t cad = codec->cad, ok = 1;
    for (uint32_t p = 0; p < plan->count; ++p) {
        const MINT_HDA_PATH *path = &plan->paths[p];
        for (uint32_t i = 0; i < path->length; ++i) {
            const MINT_HDA_WIDGET *w = mint_hda_widget(codec, path->nids[i]);
            if (!w) { ok = 0; continue; }
            uint32_t nid = w->nid;
            if (w->caps & HDA_WCAP_POWER) ok &= (uint32_t)cmd(hw, link, cad, nid, HDA_VERB_SET_POWER_STATE, 0u);
            if (i + 1u < path->length && w->conn_count > 1u && w->type != HDA_WID_MIXER)
                ok &= (uint32_t)cmd(hw, link, cad, nid, HDA_VERB_SET_CONN_SELECT, path->conn_index[i]);
            if (i + 1u < path->length && (w->caps & HDA_WCAP_IN_AMP) && w->type != HDA_WID_PIN)
                ok &= (uint32_t)cmd(hw, link, cad, nid, HDA_VERB4_SET_AMP,
                                    HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
                                    ((uint32_t)(path->conn_index[i] & 0xFu) << 8) | amp_gain(w->in_amp));
            if (w->caps & HDA_WCAP_OUT_AMP)
                ok &= (uint32_t)cmd(hw, link, cad, nid, HDA_VERB4_SET_AMP,
                                    HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | amp_gain(w->out_amp));
            if (w->type == HDA_WID_PIN) {
                uint32_t pinctl = HDA_PINCTL_OUT_EN;
                if (path->kind == HDA_DEV_HP_OUT && (w->pin_caps & HDA_PINCAP_HP_DRIVE)) pinctl |= HDA_PINCTL_HP_EN;
                ok &= (uint32_t)cmd(hw, link, cad, nid, HDA_VERB_SET_PIN_CONTROL, pinctl);
                if (w->pin_caps & HDA_PINCAP_EAPD) ok &= (uint32_t)cmd(hw, link, cad, nid, HDA_VERB_SET_EAPD, HDA_EAPD_ENABLE);
            }
            if (w->type == HDA_WID_OUTPUT) {
                ok &= (uint32_t)cmd(hw, link, cad, nid, HDA_VERB4_SET_FORMAT, format);
                ok &= (uint32_t)cmd(hw, link, cad, nid, HDA_VERB_SET_STREAM_CHANNEL, (stream_tag << 4) | 0u);
            }
        }
    }
    return (int)ok;
}

int mint_hda_set_mute(const MINT_HDA_HW *hw, MINT_HDA_LINK *link, const MINT_HDA_CODEC *codec,
                      const MINT_HDA_PLAN *plan, int mute) {
    if (!hw || !link || !codec || !plan) return 0;
    uint32_t ok = 1;
    for (uint32_t p = 0; p < plan->count; ++p) {
        const MINT_HDA_PATH *path = &plan->paths[p];
        for (uint32_t i = 0; i < path->length; ++i) {
            const MINT_HDA_WIDGET *w = mint_hda_widget(codec, path->nids[i]);
            if (!w || !(w->caps & HDA_WCAP_OUT_AMP)) continue;
            if (w->type != HDA_WID_PIN && w->type != HDA_WID_OUTPUT) continue;
            ok &= (uint32_t)cmd(hw, link, codec->cad, w->nid, HDA_VERB4_SET_AMP,
                                HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
                                (mute ? HDA_AMP_MUTE : 0u) | amp_gain(w->out_amp));
        }
    }
    return (int)ok;
}

/* ------------------------------------------------------------ stream descriptor */
int mint_hda_stream_run(const MINT_HDA_HW *hw, uint32_t sd, int run) {
    if (!hw || !sd) return 0;
    uint32_t ctl = rd(hw, sd + HDA_SD_CTL, 4u) & 0x00FFFFFFu;
    ctl = run ? (ctl | HDA_SD_CTL_RUN) : (ctl & ~HDA_SD_CTL_RUN);
    wr(hw, sd + HDA_SD_CTL, 4u, ctl);
    return wait_bits(hw, sd + HDA_SD_CTL, 4u, HDA_SD_CTL_RUN, run ? HDA_SD_CTL_RUN : 0u, 10u);
}

int mint_hda_stream_setup(const MINT_HDA_HW *hw, uint32_t sd, uint64_t bdl_pa, uint32_t entries,
                          uint32_t cyclic_bytes, uint16_t format, uint32_t stream_tag) {
    if (!hw || !sd || (bdl_pa & 127u) || entries < 2u || entries > 256u || !cyclic_bytes ||
        !format || !stream_tag || stream_tag > 15u) return 0;
    if (!mint_hda_stream_run(hw, sd, 0)) return 0;
    uint32_t ctl = rd(hw, sd + HDA_SD_CTL, 4u) & 0x00FFFFFFu;
    wr(hw, sd + HDA_SD_CTL, 4u, ctl | HDA_SD_CTL_SRST);
    if (!wait_bits(hw, sd + HDA_SD_CTL, 4u, HDA_SD_CTL_SRST, HDA_SD_CTL_SRST, 10u)) return 0;
    ctl = rd(hw, sd + HDA_SD_CTL, 4u) & 0x00FFFFFFu;
    wr(hw, sd + HDA_SD_CTL, 4u, ctl & ~HDA_SD_CTL_SRST);
    if (!wait_bits(hw, sd + HDA_SD_CTL, 4u, HDA_SD_CTL_SRST, 0u, 10u)) return 0;
    wr(hw, sd + HDA_SD_BDPL, 4u, (uint32_t)bdl_pa);
    wr(hw, sd + HDA_SD_BDPU, 4u, (uint32_t)(bdl_pa >> 32));
    wr(hw, sd + HDA_SD_CBL, 4u, cyclic_bytes);
    wr(hw, sd + HDA_SD_LVI, 2u, entries - 1u);
    wr(hw, sd + HDA_SD_FMT, 2u, format);
    ctl = rd(hw, sd + HDA_SD_CTL, 4u) & 0x00FFFFFFu;
    ctl = (ctl & ~0x00F0001Cu) | (stream_tag << 20);      /* tag; IOCE/FEIE/DEIE off (polled) */
    wr(hw, sd + HDA_SD_CTL, 4u, ctl);
    wr(hw, sd + HDA_SD_STS, 1u, HDA_SD_STS_CLEAR);
    return rd(hw, sd + HDA_SD_CBL, 4u) == cyclic_bytes &&
           (rd(hw, sd + HDA_SD_LVI, 2u) & 0xFFu) == entries - 1u &&
           (rd(hw, sd + HDA_SD_FMT, 2u) & 0x7F7Fu) == format;
}

uint32_t mint_hda_stream_position(const MINT_HDA_HW *hw, uint32_t sd) {
    if (!hw || !sd) return 0;
    return rd(hw, sd + HDA_SD_LPIB, 4u);
}
