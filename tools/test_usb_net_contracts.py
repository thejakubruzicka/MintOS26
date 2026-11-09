#!/usr/bin/env python3
"""Source contracts for the 26.5.011.0 xHCI driver, IPv4 stack, their syscalls and MintDE users."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
xhci = (root / 'kernel/usb/xhci.c').read_text()
usb_h = (root / 'kernel/include/usb.h').read_text()
net = (root / 'kernel/net/net.c').read_text()
core = (root / 'kernel/net/netcore.c').read_text()
net_h = (root / 'kernel/include/net.h').read_text()
syscall = (root / 'kernel/abi/syscall.c').read_text()
vmalloc = (root / 'kernel/mm/vmalloc.c').read_text()
paging = (root / 'kernel/mm/paging.c').read_text()
kernel = (root / 'kernel/kernel.c').read_text()
ps2 = (root / 'kernel/input/ps2.c').read_text()
web = (root / 'kernel/user/mintde/web.c').read_text()
apps = (root / 'kernel/user/mintde/apps.c').read_text()
desktop = (root / 'kernel/user/desktop.c').read_text()

import re
# Double-encoded UTF-8 ("mojibake", e.g. a middle dot written as "\u00c2\u00b7") shows up as C1 control
# characters or as U+00C2/U+00C3 followed by a Latin-1 symbol. Legitimate UTF-8 comments are fine.
MOJIBAKE = re.compile('[\u0080-\u009f]|[\u00c2\u00c3][\u00a0-\u00bf]')
sources = [p for d in ('kernel', 'uefi', 'tools') for p in (root / d).rglob('*')
           if p.suffix in ('.c', '.h', '.S', '.py') and p.name not in ('font_data.c', 'test_usb_net_contracts.py')]
non_ascii = [str(p.relative_to(root)) for p in sources if MOJIBAKE.search(p.read_text(errors='replace'))]

checks = {
    'C/assembly sources contain no double-encoded UTF-8 (mojibake)': not non_ascii,
    'xHCI DMA structures live below 4 GiB and every controller wait is bounded':
        'mint_physmem_claim_contiguous_below(1, XHCI_DMA_LIMIT)' in xhci and 'WAIT_TICKS' in xhci and
        'mint_timer_ticks() - start > WAIT_TICKS' in xhci,
    'xHCI enables PCI bus mastering only after the rings, ERST and DCBAA are programmed':
        xhci.find('/* ERSTBA */') < xhci.find('command_reg | 0x0006u') and 'only now allow bus mastering' in xhci,
    'xHCI takes ownership from firmware (USBLEGSUP) before resetting the controller':
        'static int bios_handoff(' in xhci and xhci.find('(void)bios_handoff(') < xhci.find('HCRST'),
    'device pages are freed only once the controller can no longer write them':
        'static void release_device(USB_DEV *d, int controller_safe)' in xhci and 'if (controller_safe)' in xhci and
        'A bounded leak beats freeing pages the controller might still write.' in xhci,
    'high 64-bit BARs are mapped through the uncached MMIO window, not the identity map':
        'mint_mmio_map(base, 0x10000u)' in xhci and 'void *mint_mmio_map(uint64_t physical, uint64_t bytes)' in vmalloc and
        'int mint_paging_map_mmio_page(uint64_t va, uint64_t pa)' in paging and 'PTE_PCD | PTE_PWT' in paging,
    'MMIO mappings carry no allocation record, so vmalloc_free cannot release device pages':
        'mint_vmalloc_free refuses them' in vmalloc,
    'USB HID input is injected through the device-neutral input core with interrupts masked':
        all(k in ps2 for k in ['void mint_input_inject_key(', 'void mint_input_inject_absolute(', 'void mint_input_inject_wheel(', 'irq_save()']),
    'each HID endpoint keeps several transfers queued and validates completed TRB/buffer addresses':
        '#define HID_INFLIGHT 8u' in xhci and 'param < h->ring.pa || param >= h->ring.pa + 4096u' in xhci and
        'buffer_pa < h->buf_pa || buffer_pa >= h->buf_pa + HID_INFLIGHT * HID_SLOT' in xhci,
    'USB is honest about scope: hubs and mass storage are not claimed':
        'No hubs, mass storage or isochronous transfers yet.' in usb_h,
    'boot creates the usb and net polling tasks':
        'mint_task_create("usb", mint_usb_task, 0)' in kernel and 'mint_task_create("net", mint_net_task, 0)' in kernel,
    'the stack is statically sized (ARP, DNS, ping, TCP sockets and buffers)':
        all(k in net for k in ['#define ARP_SLOTS 16u', '#define DNS_SLOTS 8u', '#define TCP_SLOTS 8u',
                               '#define TCP_RX_BUF 32768u', '#define TCP_TX_BUF 16384u']),
    'IPv4 input validates version, header length, total length and checksum, and drops fragments':
        all(k in net for k in ['(h[0] >> 4) != 4u', 'total > length', 'mint_net_checksum_fold(mint_net_checksum_add(0, h, hl)) != 0',
                               'fragments are not reassembled']),
    'UDP and TCP input verify the pseudo-header checksum':
        'mint_net_transport_checksum(src, dst, PROTO_UDP, payload, ulen) != 0' in net and
        'mint_net_transport_checksum(src, dst, PROTO_TCP, p, length) != 0' in net,
    'TCP retransmission and SYN retries are bounded with capped exponential back-off':
        all(k in net for k in ['TCP_SYN_RETRIES 5u', 'TCP_DATA_RETRIES 8u', 'TCP_RTO_MAX 600u', 'go back N']),
    'closed sockets cannot linger forever':
        'linger_deadline = ticks() + 30u * HZ' in net and 'peer never finished closing' in net,
    'DNS replies are accepted only from the configured server for an outstanding query ID':
        'src == net.dns' in net and 'e->state != DNS_PENDING || e->id != id' in net,
    'packet parsers bound every read to the supplied length (pointer-loop guard included)':
        'if (len > length - i) return 0;' in core and 'guard < 128u' in core and 'if (rdlen > length - at) return -2;' in core,
    'network syscalls copy through bounded bounce buffers and validated user ranges':
        'static uint8_t tcp_bounce[4096];' in syscall and 'mint_address_space_validate_user_range(space, arg2, (size_t)capacity, 1)' in syscall and
        'copy_user_cstring(space, arg0, name, sizeof(name))' in syscall,
    'networking ABI is versioned and documents host-order addresses':
        'MINT_NET_INFO_VERSION 1u' in net_h and 'host order' in net_h,
    'Terminal network commands run as cancellable background jobs':
        all(k in apps for k in ['terminal_network_command', 'terminal_cancel_job', 'terminal_job_step', 'Ctrl+C']),
    'automatic connection skips experimental adapters':
        '!(net_info.adapter_flags & MINT_NET_ADAPTER_EXPERIMENTAL)' in apps,
    'Web refuses https:// explicitly instead of sending plaintext to a TLS port':
        'if (prefix_ci(p, "https://")) return -1;' in web and 'cannot make encrypted (TLS) connections yet' in web,
    'Web bounds the response, links, tokens and redirects':
        all(k in web for k in ['WEB_RAW_MAX', 'WEB_LINK_MAX', 'WEB_TOK_MAX', '++fetch.redirects > 5u']),
    'the shell repaints background-updated windows and routes the mouse wheel':
        'app_background()' in desktop and 'MINT_DESKTOP_EVENT_SCROLL' in desktop,
}
failed = []
for name, ok in checks.items():
    print(('PASS: ' if ok else 'FAIL: ') + name + ('' if ok or name[:2] != 'C/' else ' ' + ', '.join(non_ascii)))
    if not ok:
        failed.append(name)
if failed:
    raise SystemExit(1)
