/* Host test for kernel/sound/hda_core.c against a register-level fake of an
 * Intel HDA controller with two codec models:
 *   - QEMU's hda-output (root, AFG 1, DAC 2, line-out pin 3), and
 *   - a Realtek ALC255/ALC3234-like graph (two DACs behind mixers, speaker,
 *     headphone and line pins, a digital S/PDIF pin, an unconnected pin,
 *     a multi-request connection list with a range entry and a cycle).
 * The fake can behave like QEMU (CORBRP reset bit never visible, CORB stops
 * after RINTCNT responses until RINTFL is acknowledged) or like hardware
 * (CORBRP reset handshake visible), can have a dead CORB (immediate-command
 * fallback) and can refuse to leave reset (bounded failure). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/hda.h"

static int failures, checks;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* ------------------------------------------------------------ codec models */
typedef struct {
    uint32_t caps, pin_caps, config, out_amp, in_amp, conn_len;
    uint32_t conn_words[8];                 /* raw GET_CONN_LIST responses (short form, 4 per word) */
    uint32_t pinctl, eapd, select, stream, format, power;
    uint32_t amp_out, amp_in[16];           /* last SET_AMP payloads */
    int present;
} NODE;

typedef struct {
    uint32_t vendor;
    uint32_t root_nodes, afg_nodes;
    NODE n[128];
    uint32_t verbs;
} CODEC;

static CODEC codecs[2];
static int codec_present[2];

static uint32_t codec_exec(uint32_t verb) {
    uint32_t cad = verb >> 28, nid = (verb >> 20) & 0x7Fu;
    if (cad > 1u || !codec_present[cad]) return 0xFFFFFFFFu;
    CODEC *c = &codecs[cad];
    ++c->verbs;
    NODE *n = &c->n[nid];
    uint32_t v12 = (verb >> 8) & 0xFFFu, p8 = verb & 0xFFu, v4 = (verb >> 16) & 0xFu, p16 = verb & 0xFFFFu;
    if (v12 == HDA_VERB_GET_PARAMETER) {
        if (nid == 0) {
            if (p8 == HDA_PAR_VENDOR_ID) return c->vendor;
            if (p8 == HDA_PAR_REVISION) return 0x00100100u;
            if (p8 == HDA_PAR_NODE_COUNT) return c->root_nodes;
            return 0;
        }
        if (nid == 1) {
            if (p8 == HDA_PAR_FG_TYPE) return 1u;
            if (p8 == HDA_PAR_NODE_COUNT) return c->afg_nodes;
            if (p8 == HDA_PAR_OUT_AMP_CAPS) return 0x80053F3Fu;     /* AFG default: mute, 64 steps, 0 dB at 0x3F */
            if (p8 == HDA_PAR_IN_AMP_CAPS) return 0x80051717u;
            return 0;
        }
        if (!n->present) return 0;
        switch (p8) {
        case HDA_PAR_AUDIO_CAPS: return n->caps;
        case HDA_PAR_PIN_CAPS: return n->pin_caps;
        case HDA_PAR_OUT_AMP_CAPS: return n->out_amp;
        case HDA_PAR_IN_AMP_CAPS: return n->in_amp;
        case HDA_PAR_CONN_LEN: return n->conn_len;
        default: return 0;
        }
    }
    if (v12 == HDA_VERB_GET_CONN_LIST) return n->conn_words[(p8 / 4u) & 7u];
    if (v12 == HDA_VERB_GET_CONFIG_DEFAULT) return n->config;
    if (v12 == HDA_VERB_GET_SUBSYSTEM_ID) return 0x1AF41100u;
    if (v12 == HDA_VERB_SET_PIN_CONTROL) { n->pinctl = p8; return 0; }
    if (v12 == HDA_VERB_SET_EAPD) { n->eapd = p8; return 0; }
    if (v12 == HDA_VERB_SET_CONN_SELECT) { n->select = p8; return 0; }
    if (v12 == HDA_VERB_SET_STREAM_CHANNEL) { n->stream = p8; return 0; }
    if (v12 == HDA_VERB_SET_POWER_STATE) { n->power = p8 + 0x100u; return 0; }
    if (v4 == HDA_VERB4_SET_FORMAT) { n->format = p16; return 0; }
    if (v4 == HDA_VERB4_SET_AMP) {
        if (p16 & HDA_AMP_SET_OUTPUT) n->amp_out = p16;
        if (p16 & HDA_AMP_SET_INPUT) n->amp_in[(p16 >> 8) & 0xFu] = p16;
        return 0;
    }
    return 0;
}

static void node(CODEC *c, uint32_t nid, uint32_t type, uint32_t extra_caps) {
    NODE *n = &c->n[nid];
    memset(n, 0, sizeof(*n));
    n->present = 1;
    n->caps = (type << 20) | extra_caps;
}
static void conns(CODEC *c, uint32_t nid, const uint8_t *list, uint32_t count) {
    NODE *n = &c->n[nid];
    n->caps |= HDA_WCAP_CONN_LIST;
    n->conn_len = count;
    for (uint32_t i = 0; i < count; ++i) n->conn_words[i / 4u] |= (uint32_t)list[i] << ((i % 4u) * 8u);
}

static void build_qemu_codec(CODEC *c) {
    memset(c, 0, sizeof(*c));
    c->vendor = 0x1AF40012u;
    c->root_nodes = (1u << 16) | 1u;
    c->afg_nodes = (2u << 16) | 2u;
    node(c, 2, HDA_WID_OUTPUT, HDA_WCAP_OUT_AMP | HDA_WCAP_AMP_OVRD | HDA_WCAP_STEREO);
    c->n[2].out_amp = 0x80034A4Au;
    node(c, 3, HDA_WID_PIN, HDA_WCAP_STEREO);
    c->n[3].pin_caps = HDA_PINCAP_OUTPUT;
    c->n[3].config = (0u << 30) | (HDA_DEV_LINE_OUT << 20) | 0x10u;
    static const uint8_t l3[] = {2};
    conns(c, 3, l3, 1);
}

static void build_realtek_codec(CODEC *c) {
    memset(c, 0, sizeof(*c));
    c->vendor = 0x10EC0255u;
    c->root_nodes = (1u << 16) | 1u;
    c->afg_nodes = (2u << 16) | 0x20u;                       /* nids 0x02..0x21 */
    node(c, 0x02, HDA_WID_OUTPUT, HDA_WCAP_OUT_AMP | HDA_WCAP_STEREO | HDA_WCAP_POWER);
    node(c, 0x03, HDA_WID_OUTPUT, HDA_WCAP_OUT_AMP | HDA_WCAP_STEREO | HDA_WCAP_POWER);
    node(c, 0x06, HDA_WID_OUTPUT, HDA_WCAP_DIGITAL);         /* S/PDIF converter */
    node(c, 0x08, HDA_WID_INPUT, HDA_WCAP_IN_AMP);
    node(c, 0x0B, HDA_WID_MIXER, HDA_WCAP_IN_AMP);           /* loopback mixer: inputs from pins */
    static const uint8_t l0b[] = {0x18, 0x19, 0x1A, 0x1B, 0x1D};
    conns(c, 0x0B, l0b, 5);                                   /* needs two GET_CONN_LIST requests */
    node(c, 0x0C, HDA_WID_MIXER, HDA_WCAP_IN_AMP);
    static const uint8_t l0c[] = {0x02, 0x0B};
    conns(c, 0x0C, l0c, 2);
    node(c, 0x0D, HDA_WID_MIXER, HDA_WCAP_IN_AMP);
    static const uint8_t l0d[] = {0x03, 0x0B, 0x0D};           /* includes a self-loop (cycle) */
    conns(c, 0x0D, l0d, 3);
    node(c, 0x14, HDA_WID_PIN, HDA_WCAP_OUT_AMP | HDA_WCAP_STEREO);   /* internal speaker */
    c->n[0x14].pin_caps = HDA_PINCAP_OUTPUT | HDA_PINCAP_EAPD;
    c->n[0x14].config = (2u << 30) | (HDA_DEV_SPEAKER << 20);
    c->n[0x14].out_amp = 0x80000000u;
    static const uint8_t l14[] = {0x0C, 0x0D};
    conns(c, 0x14, l14, 2);
    node(c, 0x17, HDA_WID_PIN, 0);                            /* mono out, port "no connection" */
    c->n[0x17].pin_caps = HDA_PINCAP_OUTPUT;
    c->n[0x17].config = (1u << 30) | (HDA_DEV_SPEAKER << 20);
    static const uint8_t l17[] = {0x0C};
    conns(c, 0x17, l17, 1);
    node(c, 0x18, HDA_WID_PIN, 0);                            /* mic jack (input only) */
    c->n[0x18].config = (0u << 30) | (0xAu << 20);
    node(c, 0x19, HDA_WID_PIN, 0);
    node(c, 0x1A, HDA_WID_PIN, 0);
    node(c, 0x1B, HDA_WID_PIN, 0);                            /* rear line out */
    c->n[0x1B].pin_caps = HDA_PINCAP_OUTPUT;
    c->n[0x1B].config = (0u << 30) | (HDA_DEV_LINE_OUT << 20);
    /* short-form range: 0x0C, then range up to 0x0D (bit 7) */
    c->n[0x1B].caps |= HDA_WCAP_CONN_LIST;
    c->n[0x1B].conn_len = 2;
    c->n[0x1B].conn_words[0] = 0x0Cu | ((0x80u | 0x0Du) << 8);
    node(c, 0x1D, HDA_WID_BEEP, 0);
    node(c, 0x1E, HDA_WID_PIN, HDA_WCAP_DIGITAL);             /* S/PDIF out */
    c->n[0x1E].pin_caps = HDA_PINCAP_OUTPUT;
    c->n[0x1E].config = (0u << 30) | (4u << 20);
    static const uint8_t l1e[] = {0x06};
    conns(c, 0x1E, l1e, 1);
    node(c, 0x21, HDA_WID_PIN, HDA_WCAP_OUT_AMP | HDA_WCAP_STEREO);   /* headphone jack */
    c->n[0x21].pin_caps = HDA_PINCAP_OUTPUT | HDA_PINCAP_HP_DRIVE | HDA_PINCAP_EAPD;
    c->n[0x21].config = (0u << 30) | (HDA_DEV_HP_OUT << 20);
    c->n[0x21].out_amp = 0x80000000u;
    static const uint8_t l21[] = {0x0C, 0x0D};
    conns(c, 0x21, l21, 2);
}

/* ------------------------------------------------------------ controller model */
typedef struct {
    int qemu_mode, dead_corb, stuck_reset;
    uint32_t gctl, statests, intctl;
    uint64_t corb_base, rirb_base;
    uint32_t corbwp, corbrp, corbctl, corbsize, rirbwp, rirbctl, rintcnt, rirbsts, rirbsize;
    uint32_t rirb_count;
    uint32_t icoi, icii, icis;
    uint32_t dpl, dpu;
    struct { uint32_t ctl, lpib, cbl, lvi, fmt, bdpl, bdpu, srst_reads; } sd[8];
    uint32_t reset_reads;
    uint64_t ticks;
    uint32_t sleeps;
} FAKE;
static FAKE fk;

static void corb_run(void) {
    if (fk.dead_corb) return;
    while ((fk.corbctl & HDA_CORBCTL_RUN) && (fk.corbrp & 0xFFu) != fk.corbwp) {
        if (fk.qemu_mode && fk.rintcnt && fk.rirb_count >= fk.rintcnt) return;   /* QEMU pauses */
        uint32_t rp = (fk.corbrp + 1u) & 0xFFu;
        uint32_t verb = ((volatile uint32_t *)(uintptr_t)fk.corb_base)[rp];
        fk.corbrp = rp;
        uint32_t resp = codec_exec(verb);
        if (!(fk.rirbctl & HDA_RIRBCTL_DMAEN)) continue;
        uint32_t wp = (fk.rirbwp + 1u) & 0xFFu;
        volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)fk.rirb_base;
        r[wp * 2u] = resp;
        r[wp * 2u + 1u] = verb >> 28;
        fk.rirbwp = wp;
        if (++fk.rirb_count >= fk.rintcnt && (fk.rirbctl & HDA_RIRBCTL_RINTCTL)) fk.rirbsts |= HDA_RIRBSTS_RINTFL;
    }
}

static uint32_t sd_index(uint32_t off) { return (off - HDA_SD_BASE) / HDA_SD_SIZE; }

static uint32_t fake_read(void *ctx, uint32_t off, uint32_t size) {
    (void)ctx;
    uint32_t v = 0;
    if (off == HDA_GCAP) v = (4u << 12) | (4u << 8) | 1u;
    else if (off == HDA_GCTL) {
        if (fk.stuck_reset) v = 0;
        else v = fk.gctl;
    } else if (off == HDA_STATESTS) v = fk.statests;
    else if (off == HDA_INTCTL) v = fk.intctl;
    else if (off == HDA_CORBWP) v = fk.corbwp;
    else if (off == HDA_CORBRP) v = fk.corbrp;
    else if (off == HDA_CORBCTL) v = fk.corbctl;
    else if (off == HDA_CORBSIZE) v = 0x40u | fk.corbsize;
    else if (off == HDA_RIRBWP) v = fk.rirbwp;
    else if (off == HDA_RINTCNT) v = fk.rintcnt;
    else if (off == HDA_RIRBCTL) v = fk.rirbctl;
    else if (off == HDA_RIRBSTS) v = fk.rirbsts;
    else if (off == HDA_RIRBSIZE) v = 0x40u | fk.rirbsize;
    else if (off == HDA_ICII) v = fk.icii;
    else if (off == HDA_ICIS) v = fk.icis;
    else if (off >= HDA_SD_BASE && off < HDA_SD_BASE + 8u * HDA_SD_SIZE) {
        uint32_t i = sd_index(off), r = (off - HDA_SD_BASE) % HDA_SD_SIZE;
        if (r == HDA_SD_CTL) v = fk.sd[i].ctl;
        else if (r == HDA_SD_STS) v = fk.sd[i].ctl >> 24;
        else if (r == HDA_SD_LPIB) v = fk.sd[i].lpib;
        else if (r == HDA_SD_CBL) v = fk.sd[i].cbl;
        else if (r == HDA_SD_LVI) v = fk.sd[i].lvi;
        else if (r == HDA_SD_FMT) v = fk.sd[i].fmt;
        else if (r == HDA_SD_BDPL) v = fk.sd[i].bdpl;
        else if (r == HDA_SD_BDPU) v = fk.sd[i].bdpu;
    }
    if (size == 1u) v &= 0xFFu; else if (size == 2u) v &= 0xFFFFu;
    return v;
}

static void fake_write(void *ctx, uint32_t off, uint32_t size, uint32_t v) {
    (void)ctx; (void)size;
    if (off == HDA_GCTL) {
        uint32_t old = fk.gctl;
        fk.gctl = v & 1u;
        if ((old & 1u) && !(v & 1u)) {           /* enter reset */
            fk.corbctl = fk.rirbctl = 0; fk.corbwp = fk.corbrp = fk.rirbwp = 0; fk.statests = 0;
        }
        if (!(old & 1u) && (v & 1u)) fk.statests = (codec_present[0] ? 1u : 0u) | (codec_present[1] ? 2u : 0u);
    } else if (off == HDA_STATESTS) fk.statests &= ~v;
    else if (off == HDA_INTCTL) fk.intctl = v;
    else if (off == HDA_CORBLBASE) fk.corb_base = (fk.corb_base & ~0xFFFFFFFFull) | v;
    else if (off == HDA_CORBUBASE) fk.corb_base = (fk.corb_base & 0xFFFFFFFFull) | ((uint64_t)v << 32);
    else if (off == HDA_RIRBLBASE) fk.rirb_base = (fk.rirb_base & ~0xFFFFFFFFull) | v;
    else if (off == HDA_RIRBUBASE) fk.rirb_base = (fk.rirb_base & 0xFFFFFFFFull) | ((uint64_t)v << 32);
    else if (off == HDA_CORBWP) { fk.corbwp = v & 0xFFu; corb_run(); }
    else if (off == HDA_CORBRP) {
        if (fk.qemu_mode) { if (v & HDA_CORBRP_RST) fk.corbrp = 0; }
        else fk.corbrp = (v & HDA_CORBRP_RST) ? HDA_CORBRP_RST : 0u;
    } else if (off == HDA_CORBCTL) { fk.corbctl = v & 3u; corb_run(); }
    else if (off == HDA_CORBSIZE) fk.corbsize = v & 3u;
    else if (off == HDA_RIRBWP) { if (v & HDA_RIRBWP_RST) fk.rirbwp = 0; }
    else if (off == HDA_RINTCNT) fk.rintcnt = v & 0xFFu;
    else if (off == HDA_RIRBCTL) fk.rirbctl = v & 7u;
    else if (off == HDA_RIRBSTS) {
        uint32_t old = fk.rirbsts;
        fk.rirbsts &= ~(v & 5u);
        if ((old & HDA_RIRBSTS_RINTFL) && !(fk.rirbsts & HDA_RIRBSTS_RINTFL)) { fk.rirb_count = 0; corb_run(); }
    } else if (off == HDA_RIRBSIZE) fk.rirbsize = v & 3u;
    else if (off == HDA_ICOI) fk.icoi = v;
    else if (off == HDA_ICIS) {
        if (v & HDA_ICIS_IRV) fk.icis &= ~HDA_ICIS_IRV;
        if (v & HDA_ICIS_ICB) { fk.icii = codec_exec(fk.icoi); fk.icis = HDA_ICIS_IRV; }
    } else if (off == HDA_DPLBASE) fk.dpl = v;
    else if (off == HDA_DPUBASE) fk.dpu = v;
    else if (off >= HDA_SD_BASE && off < HDA_SD_BASE + 8u * HDA_SD_SIZE) {
        uint32_t i = sd_index(off), r = (off - HDA_SD_BASE) % HDA_SD_SIZE;
        if (r == HDA_SD_CTL) {
            uint32_t sts = (fk.sd[i].ctl >> 24) & ~((v >> 24) & HDA_SD_STS_CLEAR);
            fk.sd[i].ctl = (v & 0x00FFFFFFu) | (sts << 24);
            if (v & HDA_SD_CTL_SRST) { fk.sd[i].lpib = 0; fk.sd[i].ctl &= ~HDA_SD_CTL_RUN; }
        } else if (r == HDA_SD_STS) fk.sd[i].ctl &= ~((v & HDA_SD_STS_CLEAR) << 24);
        else if (r == HDA_SD_CBL) fk.sd[i].cbl = v;
        else if (r == HDA_SD_LVI) fk.sd[i].lvi = v & 0xFFu;
        else if (r == HDA_SD_FMT) fk.sd[i].fmt = v & 0x7F7Fu;
        else if (r == HDA_SD_BDPL) fk.sd[i].bdpl = v & ~127u;
        else if (r == HDA_SD_BDPU) fk.sd[i].bdpu = v;
    }
}
static uint64_t fake_ticks(void *ctx) { (void)ctx; return fk.ticks; }
static void fake_sleep(void *ctx, uint32_t t) { (void)ctx; fk.ticks += t; ++fk.sleeps; }
static const MINT_HDA_HW HW = {0, fake_read, fake_write, fake_ticks, fake_sleep, 0};

static void reset_fake(int qemu_mode) {
    memset(&fk, 0, sizeof(fk));
    fk.qemu_mode = qemu_mode;
    fk.gctl = 1;
}

/* 128-byte aligned "DMA" memory; physical address == host pointer. */
static void *dma(size_t bytes) {
    void *p = 0;
    if (posix_memalign(&p, 4096, bytes)) return 0;
    memset(p, 0xA5, bytes);
    return p;
}

static void test_pure(void) {
    CHECK(mint_hda_verb(0, 3, HDA_VERB_SET_PIN_CONTROL, 0x40) == 0x00370740u, "12-bit verb encoding");
    CHECK(mint_hda_verb(0, 2, HDA_VERB4_SET_FORMAT, 0x0011) == 0x00220011u, "4-bit verb encoding");
    CHECK(mint_hda_verb(2, 0x21, HDA_VERB4_SET_AMP, 0xB04A) == 0x2213B04Au, "codec address and amp payload");
    CHECK(mint_hda_stream_format(48000, 16, 2) == 0x0011u, "48 kHz 16-bit stereo format word");
    CHECK(mint_hda_stream_format(44100, 16, 2) == 0x4011u, "44.1 kHz base rate bit");
    CHECK(mint_hda_stream_format(96000, 24, 2) == 0x0831u, "96 kHz multiplier and 24-bit");
    CHECK(mint_hda_stream_format(8000, 16, 1) == 0x0510u, "8 kHz divisor, mono");
    CHECK(mint_hda_stream_format(12345, 16, 2) == 0 && mint_hda_stream_format(48000, 12, 2) == 0 &&
          mint_hda_stream_format(48000, 16, 0) == 0, "unsupported formats rejected");
    MINT_HDA_BDL_ENTRY bdl[16];
    CHECK(mint_hda_build_bdl(bdl, 16, 0x100000, 262144, 8) == 8, "BDL with 8 entries");
    int contiguous = 1;
    for (int i = 0; i < 8; ++i)
        if (bdl[i].address != 0x100000u + (uint64_t)i * 32768u || bdl[i].length != 32768u || bdl[i].flags) contiguous = 0;
    CHECK(contiguous, "BDL entries cover the ring contiguously without IOC");
    CHECK(sizeof(MINT_HDA_BDL_ENTRY) == 16, "BDL entry is 16 bytes");
    CHECK(mint_hda_build_bdl(bdl, 16, 0x100040, 262144, 8) == 0, "misaligned buffer rejected");
    CHECK(mint_hda_build_bdl(bdl, 16, 0x100000, 262144, 1) == 0, "single-entry BDL rejected");
    CHECK(mint_hda_build_bdl(bdl, 4, 0x100000, 262144, 8) == 0, "BDL capacity respected");
    CHECK(mint_hda_build_bdl(bdl, 16, 0x100000, 1000, 2) == 0, "chunk must be 128-byte multiple");
    MINT_HDA_LINK l = {0};
    l.iss = 4; l.oss = 4;
    CHECK(mint_hda_output_stream_offset(&l, 0) == 0x100u, "first output stream follows the input streams");
    CHECK(mint_hda_output_stream_offset(&l, 4) == 0, "out-of-range output stream");
}

static int bring_up(int qemu_mode, int dead_corb, uint32_t **corb_out) {
    reset_fake(qemu_mode);
    fk.dead_corb = dead_corb;
    static MINT_HDA_LINK link;
    memset(&link, 0, sizeof(link));
    if (!mint_hda_reset_link(&HW, &link)) return 0;
    uint8_t *page = dma(4096);
    *corb_out = (uint32_t *)page;
    int ok = mint_hda_init_corb_rirb(&HW, &link, page, (uint64_t)(uintptr_t)page, page + 1024, (uint64_t)(uintptr_t)(page + 1024));
    (void)ok;
    return 1;
}

static void test_link(void) {
    codec_present[0] = 1; codec_present[1] = 0;
    build_qemu_codec(&codecs[0]);
    for (int mode = 0; mode < 2; ++mode) {
        reset_fake(mode);
        MINT_HDA_LINK link = {0};
        CHECK(mint_hda_reset_link(&HW, &link), mode ? "QEMU-mode link reset" : "hardware-mode link reset");
        CHECK(link.codec_mask == 1u && link.iss == 4 && link.oss == 4, "codec mask and stream counts after reset");
        uint8_t *page = dma(4096);
        CHECK(mint_hda_init_corb_rirb(&HW, &link, page, (uint64_t)(uintptr_t)page, page + 1024, (uint64_t)(uintptr_t)(page + 1024)),
              mode ? "CORB/RIRB start (QEMU read-pointer reset)" : "CORB/RIRB start (visible reset handshake)");
        CHECK(link.corb_size == 256u && link.rirb_size == 256u && fk.rintcnt == 1u &&
              (fk.rirbctl & (HDA_RIRBCTL_DMAEN | HDA_RIRBCTL_RINTCTL)) == 3u && (fk.corbctl & HDA_CORBCTL_RUN), "ring sizes and DMA engines");
        uint32_t r = 0, all = 1;
        for (int i = 0; i < 300; ++i) {             /* wraps both 256-entry rings */
            if (!mint_hda_command(&HW, &link, mint_hda_verb(0, 0, HDA_VERB_GET_PARAMETER, HDA_PAR_VENDOR_ID), &r) || r != 0x1AF40012u) all = 0;
        }
        CHECK(all && link.timeouts == 0, "300 commands round-trip through wrapping CORB/RIRB (RINTFL acknowledged)");
        CHECK(fk.intctl == 0, "controller interrupts stay disabled (polled)");
        mint_hda_stop_corb_rirb(&HW, &link);
        CHECK(!(fk.corbctl & HDA_CORBCTL_RUN) && !(fk.rirbctl & HDA_RIRBCTL_DMAEN) && !link.corb_ready, "CORB/RIRB stop");
        free(page);
    }
    /* dead CORB: bounded timeouts, then the immediate-command interface */
    uint32_t *page = 0;
    CHECK(bring_up(1, 1, &page), "dead-CORB controller resets");
    MINT_HDA_LINK link = {0};
    reset_fake(1); fk.dead_corb = 1;
    CHECK(mint_hda_reset_link(&HW, &link), "dead-CORB reset");
    uint8_t *pg = dma(4096);
    (void)mint_hda_init_corb_rirb(&HW, &link, pg, (uint64_t)(uintptr_t)pg, pg + 1024, (uint64_t)(uintptr_t)(pg + 1024));
    uint64_t t0 = fk.ticks;
    uint32_t r = 0, got = 0;
    for (int i = 0; i < 4; ++i) got += (uint32_t)mint_hda_command(&HW, &link, mint_hda_verb(0, 0, HDA_VERB_GET_PARAMETER, 0), &r);
    CHECK(!link.corb_ready && got >= 1 && r == 0x1AF40012u, "falls back to immediate commands after CORB timeouts");
    CHECK(fk.ticks - t0 <= 4u * 60u, "CORB timeouts are bounded (~500 ms each)");
    CHECK(mint_hda_command(&HW, &link, mint_hda_verb(0, 0, HDA_VERB_GET_PARAMETER, 0), &r) && r == 0x1AF40012u, "immediate command path");
    free(pg); free(page);
    /* controller that never leaves reset */
    reset_fake(0); fk.stuck_reset = 1;
    MINT_HDA_LINK stuck = {0};
    t0 = fk.ticks;
    CHECK(!mint_hda_reset_link(&HW, &stuck) && fk.ticks - t0 <= 40u, "stuck reset fails within a bounded wait");
}

static void scan(MINT_HDA_CODEC *codec, uint32_t cad, int qemu_mode) {
    reset_fake(qemu_mode);
    static MINT_HDA_LINK link;
    memset(&link, 0, sizeof(link));
    (void)mint_hda_reset_link(&HW, &link);
    static uint8_t *pg;
    if (!pg) pg = dma(4096);
    (void)mint_hda_init_corb_rirb(&HW, &link, pg, (uint64_t)(uintptr_t)pg, pg + 1024, (uint64_t)(uintptr_t)(pg + 1024));
    CHECK(mint_hda_codec_scan(&HW, &link, cad, codec), "codec scan");
}

static void test_codecs(void) {
    MINT_HDA_CODEC codec;
    MINT_HDA_PLAN plan;
    codec_present[0] = 1; codec_present[1] = 1;
    build_qemu_codec(&codecs[0]);
    build_realtek_codec(&codecs[1]);
    /* QEMU hda-output */
    scan(&codec, 0, 1);
    CHECK(codec.vendor_id == 0x1AF40012u && codec.afg == 1 && codec.start_nid == 2 && codec.node_count == 2, "QEMU codec topology");
    CHECK(mint_hda_plan_outputs(&codec, &plan) == 1 && plan.paths[0].length == 2 &&
          plan.paths[0].nids[0] == 3 && plan.paths[0].nids[1] == 2 && plan.paths[0].kind == HDA_DEV_LINE_OUT, "QEMU path: line-out pin 3 -> DAC 2");
    static MINT_HDA_LINK link;
    memset(&link, 0, sizeof(link));
    reset_fake(1); (void)mint_hda_reset_link(&HW, &link);
    uint8_t *pg = dma(4096);
    (void)mint_hda_init_corb_rirb(&HW, &link, pg, (uint64_t)(uintptr_t)pg, pg + 1024, (uint64_t)(uintptr_t)(pg + 1024));
    CHECK(mint_hda_apply_plan(&HW, &link, &codec, &plan, 1, 0x0011), "QEMU path programmed");
    CHECK(codecs[0].n[2].format == 0x0011u && codecs[0].n[2].stream == 0x10u, "DAC format and stream tag 1 / channel 0");
    CHECK((codecs[0].n[2].amp_out & 0xF0FFu) == 0xB04Au, "DAC out amp unmuted at 0 dB (offset 0x4A) on both channels");
    CHECK(codecs[0].n[3].pinctl == HDA_PINCTL_OUT_EN, "line-out pin enabled without HP drive");
    CHECK(mint_hda_set_mute(&HW, &link, &codec, &plan, 1) && (codecs[0].n[2].amp_out & HDA_AMP_MUTE), "hardware mute");
    CHECK(mint_hda_set_mute(&HW, &link, &codec, &plan, 0) && !(codecs[0].n[2].amp_out & HDA_AMP_MUTE), "hardware unmute");
    /* Realtek-like graph on codec address 1 */
    scan(&codec, 1, 0);
    CHECK(codec.vendor_id == 0x10EC0255u && !strcmp(mint_hda_codec_name(codec.vendor_id), "Realtek ALC255 / ALC3234"), "Realtek codec identified");
    const MINT_HDA_WIDGET *w0b = mint_hda_widget(&codec, 0x0B);
    CHECK(w0b && w0b->conn_count == 5 && w0b->conn[4] == 0x1D, "connection list read with two requests");
    const MINT_HDA_WIDGET *w1b = mint_hda_widget(&codec, 0x1B);
    CHECK(w1b && w1b->conn_count == 2 && w1b->conn[0] == 0x0C && w1b->conn[1] == 0x0D, "short-form range entry expanded");
    CHECK(mint_hda_plan_outputs(&codec, &plan) == 3, "three analog outputs (line, speaker, headphones)");
    const MINT_HDA_PATH *line = &plan.paths[0], *spk = &plan.paths[1], *hp = &plan.paths[2];
    CHECK(line->kind == HDA_DEV_LINE_OUT && line->nids[0] == 0x1B && line->length == 3 && line->nids[1] == 0x0C && line->nids[2] == 0x02,
          "line out first: 0x1B -> mixer 0x0C -> DAC 0x02");
    CHECK(spk->kind == HDA_DEV_SPEAKER && spk->nids[0] == 0x14 && spk->nids[2] == 0x02 && spk->conn_index[0] == 0,
          "speaker shares the first DAC");
    CHECK(hp->kind == HDA_DEV_HP_OUT && hp->nids[0] == 0x21 && hp->nids[2] == 0x02, "headphones share the first DAC");
    int bad = 0;
    for (uint32_t i = 0; i < plan.count; ++i) if (plan.paths[i].nids[0] == 0x17 || plan.paths[i].nids[0] == 0x1E) bad = 1;
    CHECK(!bad, "unconnected and digital pins are skipped");
    reset_fake(0); memset(&link, 0, sizeof(link)); (void)mint_hda_reset_link(&HW, &link);
    (void)mint_hda_init_corb_rirb(&HW, &link, pg, (uint64_t)(uintptr_t)pg, pg + 1024, (uint64_t)(uintptr_t)(pg + 1024));
    CHECK(mint_hda_apply_plan(&HW, &link, &codec, &plan, 1, 0x0011), "Realtek paths programmed");
    CODEC *rt = &codecs[1];
    CHECK(rt->n[0x14].select == 0 && rt->n[0x21].select == 0 && rt->n[0x1B].select == 0, "pin connection selects");
    CHECK((rt->n[0x0C].amp_in[0] & 0xF080u) == (HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT), "mixer input 0 unmuted");
    CHECK(rt->n[0x14].pinctl == HDA_PINCTL_OUT_EN && rt->n[0x21].pinctl == (HDA_PINCTL_OUT_EN | HDA_PINCTL_HP_EN), "speaker and HP pin control");
    CHECK(rt->n[0x14].eapd == HDA_EAPD_ENABLE && rt->n[0x21].eapd == HDA_EAPD_ENABLE, "EAPD enabled where supported");
    CHECK(rt->n[0x02].format == 0x0011u && rt->n[0x02].stream == 0x10u && rt->n[0x02].power == 0x100u, "DAC powered, formatted, tagged");
    CHECK((rt->n[0x02].amp_out & 0x7Fu) == 0x3Fu, "AFG default amp caps used when no override (0 dB = 0x3F)");
    CHECK(rt->n[0x03].format == 0 && rt->n[0x06].format == 0, "unused and digital converters untouched");
    /* hostile codec answers */
    codecs[1].vendor = 0xFFFFFFFFu;
    MINT_HDA_CODEC bogus;
    reset_fake(0); memset(&link, 0, sizeof(link)); (void)mint_hda_reset_link(&HW, &link);
    (void)mint_hda_init_corb_rirb(&HW, &link, pg, (uint64_t)(uintptr_t)pg, pg + 1024, (uint64_t)(uintptr_t)(pg + 1024));
    CHECK(!mint_hda_codec_scan(&HW, &link, 1, &bogus), "absent codec (all-ones vendor) rejected");
    build_realtek_codec(&codecs[1]);
    codecs[1].afg_nodes = (2u << 16) | 0xFFu;
    CHECK(mint_hda_codec_scan(&HW, &link, 1, &bogus) && bogus.node_count <= MINT_HDA_MAX_NODES, "node count clamped");
    free(pg);
}

static void test_stream(void) {
    reset_fake(1);
    uint32_t sd = 0x100;
    CHECK(mint_hda_stream_setup(&HW, sd, 0x200000, 8, 262144, 0x0011, 1), "stream descriptor setup");
    CHECK(fk.sd[4].cbl == 262144u && fk.sd[4].lvi == 7u && fk.sd[4].fmt == 0x0011u && fk.sd[4].bdpl == 0x200000u &&
          ((fk.sd[4].ctl >> 20) & 0xFu) == 1u && !(fk.sd[4].ctl & 0x1Cu), "CBL, LVI, format, BDL address, stream tag, no IRQs");
    CHECK(mint_hda_stream_run(&HW, sd, 1) && (fk.sd[4].ctl & HDA_SD_CTL_RUN), "stream runs");
    fk.sd[4].lpib = 4096;
    CHECK(mint_hda_stream_position(&HW, sd) == 4096u, "position from LPIB");
    CHECK(mint_hda_stream_run(&HW, sd, 0) && !(fk.sd[4].ctl & HDA_SD_CTL_RUN), "stream stops");
    CHECK(!mint_hda_stream_setup(&HW, sd, 0x200040, 8, 262144, 0x0011, 1), "misaligned BDL rejected");
    CHECK(!mint_hda_stream_setup(&HW, sd, 0x200000, 8, 262144, 0x0011, 0), "stream tag 0 rejected");
}

int main(void) {
    test_pure();
    test_link();
    test_codecs();
    test_stream();
    printf("%s: %d/%d HDA checks passed\n", failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
