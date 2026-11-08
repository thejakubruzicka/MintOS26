#!/usr/bin/env python3
"""Render a static preview of the UEFI session selector; not a VM screenshot."""
from pathlib import Path
import sys
from PIL import Image, ImageDraw
sys.path.insert(0, str(Path(__file__).resolve().parent))
import render_native_shell_preview as ui

out=Path(sys.argv[1]) if len(sys.argv)>1 else Path('previews/mintos26-26.5.009.0-boot-selector-preview.png')
out.parent.mkdir(parents=True,exist_ok=True)
im=ui.wallpaper_image()
d=ImageDraw.Draw(im,'RGBA')
# Early selector intentionally avoids the regular desktop menu and dock.
ui.glass_panel(im,(290,182,1630,868),tint=(19,38,56,226),radius=34,blur=18,outline=(255,255,255,115))
d=ImageDraw.Draw(im,'RGBA')
ui.app_icon(im,'finder',375,241,76,ui.C['mint'])
ui.text(d,(476,247),'Choose your MintOS26 session',ui.F_HERO,(247,251,254,255))
ui.text(d,(478,303),'MintOS26 26.5.009.0  ·  MintDE 1.0.8',ui.F_BODY,(190,211,224,255))
# UEFI default card
ui.rounded(d,(365,374,930,626),22,(241,249,252,246),outline=(255,255,255,255),width=2)
ui.app_icon(im,'finder',405,412,76,ui.C['blue'])
ui.text(d,(507,411),'1  UEFI Desktop',ui.F_TITLE,(30,53,71,255))
ui.text(d,(507,459),'Recommended',ui.F_MED,(21,152,126,255))
ui.text(d,(407,522),'Normal boot and recovery environment',ui.F_BODY,(71,96,112,255))
# Native option card
ui.rounded(d,(990,374,1555,626),22,(34,57,76,246),outline=(102,162,185,255),width=2)
ui.app_icon(im,'settings',1030,412,76,ui.C['mint'])
ui.text(d,(1132,411),'2  MintDE 1.0.8',ui.F_TITLE,(250,253,255,255))
ui.text(d,(1132,459),'Experimental',ui.F_MED,(255,183,92,255))
ui.text(d,(1032,522),'Native kernel / one-way handoff',ui.F_BODY,(195,216,227,255))
ui.rounded(d,(365,665,1555,742),16,(255,255,255,22),outline=(255,255,255,60),width=1)
ui.text(d,(397,686),'Press 1 or Enter for UEFI Desktop, or 2 / M for MintDE.',ui.F_BODY,(247,251,254,255))
ui.text(d,(397,780),'No input? UEFI Desktop starts after 3 seconds. Reset the VM to recover from an experimental kernel failure.',ui.F_SMALL,(197,216,228,255))
im.save(out,optimize=False,compress_level=2)
print(f'Rendered boot selector concept: {out} (1920x1080; static concept, not a live UEFI screenshot)')
