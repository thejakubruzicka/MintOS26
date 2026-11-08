#!/usr/bin/env python3
"""Behavior tests for pointer scaling and common HID mouse/tablet report layouts."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = root / "uefi" / "main.c"
harness = r'''#define efi_main mintos_fake_efi_main
#include "SOURCE_PATH"
#include <assert.h>
#include <stdio.h>
int main(void) {
  unsigned char tablet[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x01,
    0x09,0x01,0xa1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x05,0x81,0x01,
    0x05,0x01,0x09,0x30,0x09,0x31,0x16,0x00,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x02,0xc0,0xc0
  };
  unsigned char mouse[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x05,0x81,0x01,
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x03,0x81,0x06,0xc0,0xc0
  };
  USB_HID_POINTER p={0}; p.max_packet=8;
  int ok=parse_hid_pointer_descriptor(tablet,sizeof(tablet),&p);
  assert(ok && p.report_id==1 && (p.absolute_axes&3)==3);
  assert(p.x_bit==8 && p.y_bit==24 && p.x_size==16 && p.y_size==16);
  assert(p.x_min==0 && p.x_max==32767 && p.y_min==0 && p.y_max==32767 && p.report_len==6);
  unsigned char tablet_report[]={0x01,0x05,0x00,0x40,0xff,0x7f};
  assert(hid_report_bits(tablet_report+1,5,p.x_bit,p.x_size)==16384);
  assert(hid_report_bits(tablet_report+1,5,p.y_bit,p.y_size)==32767);
  assert(hid_report_bits(tablet_report+1,5,p.left_bit,1)==1);
  assert(hid_report_bits(tablet_report+1,5,p.right_bit,1)==0);

  p=(USB_HID_POINTER){0}; p.max_packet=8;
  ok=parse_hid_pointer_descriptor(mouse,sizeof(mouse),&p);
  assert(ok && p.report_id==0 && p.absolute_axes==0);
  assert(p.x_bit==8 && p.y_bit==16 && p.x_size==8 && p.y_size==8 && p.report_len==4);
  unsigned char rel_report[]={0x01,0xfb,0x07,0};
  assert(hid_value_signed(hid_report_bits(rel_report,sizeof(rel_report),p.x_bit,p.x_size),p.x_size,p.x_min)==-5);
  assert(hid_value_signed(hid_report_bits(rel_report,sizeof(rel_report),p.y_bit,p.y_size),p.y_size,p.y_min)==7);

  i64 rem=0;
  int normal=scale_relative_delta(500,500,1920,&rem);
  assert(normal>=8 && normal<=10);
  rem=0;
  int big=scale_relative_delta(500,40,1920,&rem);
  assert(big==POINTER_MAX_STEP);
  rem=0;
  int broken=scale_relative_delta(500,0,1920,&rem);
  assert(broken==7);
  rem=0;
  int reverse=scale_relative_delta(-500,0,1920,&rem);
  assert(reverse==-7);
  rem=0;
  assert(scale_relative_delta(500,1,1920,&rem)<=POINTER_MAX_STEP);
  printf("PASS: HID boot-mouse/tablet layouts and report decoding; relative scaling bounds %d/%d/%d px\n",normal,big,broken);
  return 0;
}
'''.replace("SOURCE_PATH", str(source))

with tempfile.TemporaryDirectory(prefix="mintos-pointer-test-") as tmp:
    cfile = Path(tmp) / "test.c"
    exe = Path(tmp) / "test-pointer"
    cfile.write_text(harness)
    subprocess.run(["clang", "-O0", "-w", str(cfile), str(root / "kernel/ui/display_surface.c"), str(root / "kernel/ui/desktop_event.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
