#!/usr/bin/env python3
"""Render static MintDE shell, Settings, Storage, Drivers, Files and Disk Utility concepts.

Uses the repository wallpaper and smooth DejaVu system fonts for concept art.
These are concept previews, not screenshots from a live VM or pixel-exact
output of the freestanding userspace text rasterizer. The About previews are
version-separated concept pages for MintOS26 and MintDE 1.0.8.
"""
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
W, H = 1920, 1080
WALL = ROOT / 'assets/wallpaper-1920x1080.png'
FONT_DIRS = [Path('/usr/share/fonts/truetype/dejavu'), Path('/usr/share/fonts/truetype/liberation2')]

def font(size, bold=False):
    names = ['DejaVuSans-Bold.ttf' if bold else 'DejaVuSans.ttf',
             'LiberationSans-Bold.ttf' if bold else 'LiberationSans-Regular.ttf']
    for folder in FONT_DIRS:
        for name in names:
            p = folder / name
            if p.exists(): return ImageFont.truetype(str(p), size=size)
    return ImageFont.load_default()

F_SMALL = font(15); F_MED = font(18); F_BODY = font(20); F_TITLE = font(26, True)
F_HEAD = font(30, True); F_HERO = font(39, True); F_BRAND = font(34, True)
C = {
 'navy': (28,51,76), 'navy2': (18,37,58), 'text': (34,54,69), 'muted': (85,108,125),
 'white': (250,252,253), 'panel': (245,249,251), 'line': (197,216,226),
 'mint': (32,191,169), 'blue': (84,157,236), 'violet': (139,99,229),
 'orange': (232,162,61), 'rose': (234,91,113), 'green': (56,184,139), 'teal': (32,142,136),
}
APPS = [
 ('Files','Browse volumes','finder',C['mint']), ('Notes','Quick notes','notes',C['violet']),
 ('Terminal','Developer console','terminal',C['rose']), ('Settings','Appearance and input','settings',C['blue']),
 ('Monitor','System activity','monitor',C['teal']), ('Storage & Logs','Read-only disk status','storage',C['orange']),
 ('Drivers','Device support states','drivers',C['blue']), ('Network','Adapter inventory','network',C['mint']),
 ('Disk Utility','Manage blank media','storage',C['orange']), ('Launchpad','All applications','grid',C['blue']),
]
def rounded(draw, box, radius, fill, outline=None, width=1):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)

def text(draw, xy, value, f=F_BODY, fill=None):
    draw.text(xy, value, font=f, fill=fill or C['text'])

def wallpaper_image():
    im = Image.open(WALL).convert('RGB').resize((W,H), Image.Resampling.LANCZOS)
    return im

def glass_panel(im, box, tint=(242,248,251,222), radius=22, blur=9, outline=(255,255,255,220)):
    x0,y0,x1,y1 = box
    crop = im.crop(box).filter(ImageFilter.GaussianBlur(blur)).convert('RGBA')
    overlay = Image.new('RGBA', crop.size, tint)
    panel = Image.alpha_composite(crop, overlay)
    mask = Image.new('L', crop.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0,0,crop.width-1,crop.height-1), radius=radius, fill=255)
    im.paste(panel.convert('RGB'), (x0,y0), mask)
    d = ImageDraw.Draw(im, 'RGBA')
    d.rounded_rectangle(box, radius=radius, outline=outline, width=2)

def menu_bar(im):
    d = ImageDraw.Draw(im, 'RGBA')
    d.rectangle((0,0,W,58), fill=(22,48,76,210))
    d.line((0,58,W,58), fill=(255,255,255,80), width=1)
    text(d,(28,14),'MintDE',F_BRAND,(255,255,255,255))
    text(d,(220,20),'Desktop',F_MED,(255,255,255,255))
    for label,x in [('File',330),('Edit',396),('View',462),('Window',535),('Help',635)]:
        text(d,(x,20),label,F_MED,(247,250,252,255))
    text(d,(W-145,20),'10:08',F_MED,(255,255,255,255))

def app_icon(im, name, x, y, size, color):
    d = ImageDraw.Draw(im, 'RGBA')
    rounded(d,(x,y,x+size,y+size),max(9,size//5),(*color,255),outline=(255,255,255,230),width=2)
    ink=(18,42,61,255); white=(247,251,255,255)
    if name in ('finder','Files'):
        rounded(d,(x+size*.20,y+size*.31,x+size*.81,y+size*.73),5,(207,224,244,255),outline=white,width=2)
        rounded(d,(x+size*.22,y+size*.23,x+size*.48,y+size*.36),4,(255,205,90,255))
    elif name=='terminal':
        rounded(d,(x+size*.15,y+size*.15,x+size*.85,y+size*.85),7,(16,34,52,255))
        d.line((x+size*.29,y+size*.38,x+size*.44,y+size*.49,x+size*.29,y+size*.60),fill=(244,78,116,255),width=max(2,size//16))
        d.line((x+size*.49,y+size*.65,x+size*.72,y+size*.65),fill=white,width=max(2,size//16))
    elif name=='notes':
        rounded(d,(x+size*.20,y+size*.13,x+size*.80,y+size*.87),6,(250,249,255,255))
        for j in range(3): d.line((x+size*.32,y+size*(.35+j*.15),x+size*.68,y+size*(.35+j*.15)),fill=(*color,255),width=max(2,size//20))
    elif name in ('calc','clock'):
        rounded(d,(x+size*.15,y+size*.12,x+size*.85,y+size*.88),7,(29,48,66,255))
        rounded(d,(x+size*.26,y+size*.22,x+size*.74,y+size*.36),3,(139,224,211,255))
        for row in range(3):
            for col in range(3):
                r=size*.045
                cx=x+size*(.31+col*.19); cy=y+size*(.48+row*.15)
                d.ellipse((cx-r,cy-r,cx+r,cy+r),fill=white)
    elif name=='settings':
        d.ellipse((x+size*.22,y+size*.22,x+size*.78,y+size*.78),fill=white)
        d.ellipse((x+size*.38,y+size*.38,x+size*.62,y+size*.62),fill=(*color,255))
    elif name=='monitor':
        rounded(d,(x+size*.17,y+size*.18,x+size*.83,y+size*.68),5,(229,244,247,255))
        d.rectangle((x+size*.44,y+size*.67,x+size*.56,y+size*.79),fill=white)
        d.rectangle((x+size*.31,y+size*.78,x+size*.69,y+size*.84),fill=white)
    elif name=='calendar':
        rounded(d,(x+size*.20,y+size*.13,x+size*.80,y+size*.88),6,(255,251,252,255))
        for j in range(3): d.line((x+size*.32,y+size*(.34+j*.15),x+size*.68,y+size*(.34+j*.15)),fill=(*color,255),width=max(2,size//20))
    elif name=='network':
        d.ellipse((x+size*.21,y+size*.21,x+size*.79,y+size*.79),outline=white,width=max(3,size//13))
        d.ellipse((x+size*.39,y+size*.39,x+size*.61,y+size*.61),fill=white)
    elif name=='storage':
        for j in range(3):
            yy=y+size*(.24+j*.22)
            rounded(d,(x+size*.20,yy,x+size*.80,yy+size*.14),4,(235,244,248,255),outline=white,width=1)
            d.ellipse((x+size*.68,yy+size*.035,x+size*.74,yy+size*.095),fill=(*color,255))
    elif name=='drivers':
        rounded(d,(x+size*.26,y+size*.20,x+size*.74,y+size*.80),7,(238,246,251,255),outline=white,width=2)
        for yy in (.34,.49,.64):
            d.ellipse((x+size*.34,y+size*yy,x+size*.42,y+size*(yy+.08)),fill=(*color,255))
            d.line((x+size*.48,y+size*(yy+.04),x+size*.65,y+size*(yy+.04)),fill=ink,width=max(2,size//18))
    else:
        for row in range(3):
            for col in range(3):
                cx=x+size*(.28+col*.22); cy=y+size*(.28+row*.22); r=size*.075
                d.ellipse((cx-r,cy-r,cx+r,cy+r),fill=white)

def draw_app_card(im, x, y, w, h, app, selected=False):
    name, desc, icon, color = app
    d=ImageDraw.Draw(im,'RGBA')
    fill=(255,255,255,234) if not selected else (255,255,255,246)
    rounded(d,(x,y,x+w,y+h),19,fill,outline=(194,215,226,235),width=2)
    d.rounded_rectangle((x+1,y+1,x+w-1,y+6),radius=3,fill=(*color,255))
    app_icon(im,icon,x+20,y+35, sixty:=64,color)
    d=ImageDraw.Draw(im,'RGBA')
    text(d,(x+100,y+40),name,F_TITLE,C['text'])
    text(d,(x+100,y+80),desc,F_SMALL,C['muted'])
    text(d,(x+100,y+119),'Open  ›',F_SMALL,(0,145,132,255))

def dock(im):
    d=ImageDraw.Draw(im,'RGBA')
    box=(365,894,1555,1048)
    # Translucent dark dock: all launch targets live in the dock, no desktop shortcuts.
    glass_panel(im,box,tint=(29,36,38,188),radius=40,blur=16,outline=(255,255,255,136))
    d=ImageDraw.Draw(im,'RGBA')
    for i, data in enumerate(APPS):
        xx=424+i*108
        app_icon(im,data[2],xx,920,68,data[3])
        if i < len(APPS)-1:
            d.ellipse((xx+31,1003,xx+37,1009),fill=(255,255,255,150))

def draw_brand(im):
    # Clean workspace: don't draw desktop shortcut icons (Launchpad is in the dock).
    d=ImageDraw.Draw(im,'RGBA')
    text(d,(64,82),'MintDE',font(58,True),(255,255,255,245))
    text(d,(64,151),'Mint Desktop Environment',font(23,False),(242,250,255,235))
    rounded(d,(64,202,392,250),20,(19,39,58,116),outline=(255,255,255,95),width=1)
    text(d,(86,216),'Version 1.0.2  ·  Native workspace',F_SMALL,(255,255,255,245))

def draw_traffic_lights(im,x,y):
    d=ImageDraw.Draw(im,'RGBA')
    for i,c in enumerate([(255,96,91,255),(255,190,66,255),(43,201,131,255)]):
        d.ellipse((x+i*34,y,x+17+i*34,y+17),fill=c)

def desktop_preview(out):
    """Show MintDE's clean initial workspace with its retained glass dock."""
    im=wallpaper_image(); menu_bar(im)
    dock(im)
    draw_cursor(im,930,610)
    im.save(out,optimize=False,compress_level=2)

def menu_dropdown_preview(out):
    """Static concept of a translucent menu dropdown, not a live screenshot."""
    im=wallpaper_image(); menu_bar(im)
    dock(im)
    d=ImageDraw.Draw(im,'RGBA')
    box=(300,52,560,294)
    glass_panel(im,box,tint=(235,246,252,224),radius=17,blur=12,outline=(255,255,255,190))
    d=ImageDraw.Draw(im,'RGBA')
    text(d,(321,68),'File',F_TITLE,C['text'])
    rows=[('New Window','⌘N'),('Open…','⌘O'),('Show All Volumes',''),('Refresh',''),('Close Window','⌘W')]
    for i,(label,key) in enumerate(rows):
        yy=112+i*34
        if i==2: rounded(d,(309,yy-3,551,yy+27),8,(41,191,171,48))
        text(d,(321,yy),label,F_SMALL,C['text'])
        if key: text(d,(478,yy),key,F_SMALL,C['muted'])
        if i==1: d.line((318,yy+28,542,yy+28),fill=(165,190,204,155),width=1)
    draw_cursor(im,445,185)
    im.save(out,optimize=False,compress_level=2)

def draw_cursor(im,x,y):
    d=ImageDraw.Draw(im,'RGBA')
    d.polygon([(x,y),(x+3,y+31),(x+11,y+22),(x+20,y+40),(x+27,y+36),(x+18,y+19),(x+31,y+18)],fill=(255,255,255,255),outline=(20,32,43,255))

def settings_preview(out):
    im=wallpaper_image(); menu_bar(im)
    box=(300,112,1615,820); glass_panel(im,box,tint=(243,248,251,226),radius=27,blur=12)
    d=ImageDraw.Draw(im,'RGBA')
    draw_traffic_lights(im,345,143)
    text(d,(452,138),'System Settings',font(27,True),C['text'])
    text(d,(352,202),'Personalize your workspace',F_HEAD,C['text'])
    text(d,(352,245),'Settings apply instantly; session data resets on reboot.',F_BODY,C['muted'])
    # sidebar
    rounded(d,(332,278,578,770),18,(224,235,241,230),outline=(255,255,255,210),width=1)
    for i,(label,icon) in enumerate([('Appearance','◐'),('Display','▣'),('Keyboard','⌨'),('Pointer','➤'),('Network','◎'),('Storage','▤'),('About','ⓘ')]):
        yy=299+i*61
        if i==0: rounded(d,(346,yy-5,562,yy+43),10,(255,255,255,245),outline=(198,220,229,255),width=1)
        text(d,(360,yy),icon,F_TITLE,C['mint'] if i==0 else C['muted'])
        text(d,(405,yy+3),label,F_MED,C['text'] if i==0 else C['muted'])
    # content cards
    rounded(d,(600,278,1584,503),18,(255,255,255,236),outline=(199,218,227,255),width=1)
    text(d,(632,301),'Appearance',F_TITLE,C['text'])
    text(d,(632,335),'Theme, accent, typography and desktop background',F_SMALL,C['muted'])
    controls=[('Theme','Light  ▾'),('Accent','Mint  ▾'),('Text size','Normal  ▾'),('Wallpaper','Mountain lake  ▾'),('Reduce motion','Off  ◯')]
    for i,(name,val) in enumerate(controls):
        yy=373+i*24
        text(d,(635,yy),name,F_SMALL,C['text'])
        if i==4:
            rounded(d,(1406,yy-1,1460,yy+22),11,(219,231,236,255))
            d.ellipse((1408,yy+1,1428,yy+20),fill=(255,255,255,255))
        else:
            rounded(d,(1240,yy-4,1538,yy+20),8,(246,250,252,255),outline=(197,216,226,255),width=1)
            text(d,(1253,yy-1),val,F_SMALL,C['text'])
    rounded(d,(600,523,1584,770),18,(255,255,255,236),outline=(199,218,227,255),width=1)
    text(d,(632,547),'Hardware & activity',F_TITLE,C['text'])
    text(d,(632,583),'Keyboard',F_SMALL,C['muted']); text(d,(756,583),'Connected',F_SMALL,(19,157,125,255))
    text(d,(632,615),'Pointer',F_SMALL,C['muted']); text(d,(756,615),'Connected',F_SMALL,(19,157,125,255))
    text(d,(632,659),'Key events',F_SMALL,C['muted']); text(d,(782,659),'124',F_MED,C['text'])
    text(d,(632,691),'Pointer moves',F_SMALL,C['muted']); text(d,(782,691),'2,418',F_MED,C['text'])
    text(d,(632,723),'Queue drops',F_SMALL,C['muted']); text(d,(782,723),'0',F_MED,C['text'])
    rounded(d,(1266,696,1538,744),12,(229,242,243,255),outline=(171,212,217,255),width=1)
    text(d,(1325,711),'Refresh counters',F_MED,(18,126,124,255))
    im.save(out,optimize=False,compress_level=2)

def storage_preview(out):
    im=wallpaper_image(); menu_bar(im)
    box=(250,124,1648,822); glass_panel(im,box,tint=(242,248,251,229),radius=27,blur=12)
    d=ImageDraw.Draw(im,'RGBA'); draw_traffic_lights(im,300,151)
    text(d,(405,146),'Storage & Logs',font(28,True),C['text'])
    text(d,(1038,154),'CONCEPT MOCKUP · not live guest output',F_SMALL,(235,145,61,255))
    tabs=[('Devices',C['mint']),('Controllers',C['blue']),('All logs',C['muted']),('Warn+',C['orange']),('Errors',C['rose']),('Refresh',C['blue']),('Export',C['mint'])]
    tx=294
    for label,col in tabs:
        tw=28+len(label)*9
        rounded(d,(tx,193,tx+tw,226),8,(255,255,255,235),outline=(204,222,230,255),width=1)
        text(d,(tx+10,201),label,F_SMALL,(*col,255)); tx += tw+8
    text(d,(294,248),'Controllers   1       Devices   1        Probe complete',F_MED,C['muted'])
    rounded(d,(287,278,1608,690),18,(248,251,252,242),outline=(202,222,230,255),width=1)
    d.rectangle((287,278,293,690),fill=(*C['blue'],255))
    text(d,(311,296),'sda',F_TITLE,C['text']); text(d,(430,300),'VBOX HARDDISK',F_MED,C['text'])
    text(d,(311,336),'Serial',F_SMALL,C['muted']); text(d,(408,336),'MintOS26-DEV-0043',F_MED,C['text'])
    text(d,(311,375),'Capacity',F_SMALL,C['muted']); text(d,(408,375),'512 MiB',F_MED,C['text']); text(d,(610,375),'1,048,576 sectors',F_MED,C['text'])
    text(d,(311,414),'LBA0 signature',F_SMALL,C['muted']); text(d,(455,414),'55 AA',F_MED,(20,157,130,255)); text(d,(564,414),'repeat MATCH',F_MED,(20,157,130,255))
    text(d,(311,453),'Disk ID',F_SMALL,C['muted']); text(d,(408,453),'0x26300421',F_MED,C['text']); text(d,(558,453),'developer marker matched',F_MED,(20,157,130,255))
    text(d,(311,492),'Partition 1',F_SMALL,C['muted']); text(d,(408,492),'FAT16 · type 0x06',F_MED,C['text']); text(d,(650,492),'LBA 2048 · 1,046,528 sectors',F_MED,C['text'])
    rounded(d,(309,520,1583,609),13,(232,246,242,245),outline=(173,218,204,255),width=1)
    text(d,(330,537),'EXAMPLE: READ-ONLY MOUNT',F_TITLE,(17,135,111,255))
    text(d,(330,574),'NOTES.TXT and SETTINGS.DAT readable · persistent writes remain disabled',F_MED,C['text'])
    text(d,(311,650),'Writes disabled. No media writes attempted. RAM fallback remains available.',F_SMALL,(220,128,55,255))
    im.save(out,optimize=False,compress_level=2)

def drivers_preview(out):
    im=wallpaper_image(); menu_bar(im)
    box=(250,124,1648,822); glass_panel(im,box,tint=(242,248,251,229),radius=27,blur=12)
    d=ImageDraw.Draw(im,'RGBA'); draw_traffic_lights(im,300,151)
    text(d,(405,146),'Hardware & Drivers',font(28,True),C['text'])
    text(d,(294,205),'Device discovery is not the same as driver support.',F_BODY,C['muted'])
    records=[('Software framebuffer','ACTIVE','No GPU acceleration'),('PCI inventory','ACTIVE','Bounded configuration scan'),('PS/2 keyboard','ACTIVE','Translated set-1 input'),('PS/2 mouse','ACTIVE','3-byte packet service'),('AHCI / SATA','READ ONLY','Bounded diagnostic sector reads'),('FAT16','READ ONLY','Mount and file reads; no writes'),('VMware SVGA','DETECTED','Inventory only; no display driver'),('E1000 Ethernet','DETECTED','No packet I/O or TCP/IP'),('USB host controllers','DETECTED','No USB mass-storage driver'),('NVMe','NOT IMPLEMENTED','No native NVMe I/O'),('ACPI / power','NOT IMPLEMENTED','No power-state transitions'),('Audio','NOT IMPLEMENTED','No native audio output')]
    startx,starty,cw,ch,gap=292,254,620,72,18
    for i,(name,state,desc) in enumerate(records):
        col=i%2; row=i//2; x=startx+col*(cw+22); y=starty+row*(ch+13)
        rounded(d,(x,y,x+cw,y+ch),12,(255,255,255,238),outline=(202,222,230,255),width=1)
        colr=(20,157,130,255) if state in ('ACTIVE','READ ONLY') else (228,143,59,255)
        d.rectangle((x,y,x+4,y+ch),fill=colr)
        text(d,(x+18,y+12),name,F_MED,C['text']); text(d,(x+cw-178,y+12),state,F_SMALL,colr)
        text(d,(x+18,y+42),desc,F_SMALL,C['muted'])
    text(d,(294,754),'MintOS26 uses its own freestanding drivers, not macOS kexts.',F_SMALL,C['muted'])
    im.save(out,optimize=False,compress_level=2)

def files_preview(out):
    im=wallpaper_image(); menu_bar(im)
    box=(180,112,1740,888); glass_panel(im,box,tint=(243,248,251,224),radius=25,blur=11)
    d=ImageDraw.Draw(im,'RGBA'); draw_traffic_lights(im,214,141)
    text(d,(323,133),'Files',font(27,True),C['text'])
    text(d,(218,198),'Locations',F_TITLE,C['text'])
    rounded(d,(208,244,510,834),15,(224,236,242,242),outline=(194,214,224,255),width=1)
    for i,(label,sub) in enumerate([('Favorites',''),('Session (RAM)','Volatile'),('Developer volume','Read-only'),('Documents',''),('Downloads',''),('System logs','')]):
        yy=268+i*74
        if i==1: rounded(d,(224,yy-7,494,yy+47),10,(255,255,255,246),outline=(185,216,221,255),width=1)
        text(d,(244,yy),label,F_MED,C['text'] if i!=2 else C['teal'])
        if sub: text(d,(244,yy+26),sub,F_SMALL,C['muted'])
    rounded(d,(530,185,1708,231),12,(255,255,255,235),outline=(196,216,226,255),width=1)
    text(d,(551,198),'Session (RAM)  /  Documents',F_MED,C['text'])
    list_box=(530,249,1110,833); preview_box=(1126,249,1708,833)
    rounded(d,list_box,14,(255,255,255,241),outline=(200,219,227,255),width=1)
    rounded(d,preview_box,14,(255,255,255,241),outline=(200,219,227,255),width=1)
    text(d,(554,269),'Name',F_SMALL,C['muted']); text(d,(1019,269),'Size',F_SMALL,C['muted'])
    for i,(name,size,kind) in enumerate([('WELCOME.TXT','112 B','text'),('NOTES.TXT','0 B','text'),('ABOUT.TXT','103 B','text'),('Screenshots','Folder','folder'),('Source','Folder','folder')]):
        yy=314+i*58
        if i==1: rounded(d,(544,yy-7,1096,yy+40),9,(222,240,247,255))
        if kind=='folder': rounded(d,(557,yy+2,578,yy+18),4,(255,205,90,255),outline=(73,133,191,255),width=1)
        else: rounded(d,(560,yy-1,576,yy+22),3,(246,250,253,255),outline=(134,164,180,255),width=1)
        text(d,(594,yy),name,F_MED,C['text']); text(d,(1015,yy),size,F_SMALL,C['muted'])
    text(d,(1151,269),'Preview',F_TITLE,C['text'])
    text(d,(1151,323),'NOTES.TXT',F_MED,C['muted'])
    for i,line_text in enumerate(['Notes are stored in session RAM.','They reset when the VM reboots.','The mounted developer volume,','when available, is currently','read-only.']):
        text(d,(1151,371+i*36),line_text,F_BODY,C['text'])
    rounded(d,(1151,640,1682,700),12,(232,246,242,255),outline=(181,221,210,255),width=1)
    text(d,(1171,653),'SESSION FILES  ·  VOLATILE',F_MED,C['teal'])
    text(d,(1151,735),'No write access to mounted media in this release.',F_SMALL,C['orange'])
    draw_cursor(im,1260,600); im.save(out,optimize=False,compress_level=2)

def disk_utility_preview(out):
    im=wallpaper_image(); menu_bar(im)
    box=(170,108,1750,895); glass_panel(im,box,tint=(243,248,251,226),radius=25,blur=11)
    d=ImageDraw.Draw(im,'RGBA'); draw_traffic_lights(im,204,137)
    text(d,(313,129),'Disk Utility',font(27,True),C['text'])
    text(d,(210,194),'Prepare a blank disposable disk',F_HEAD,C['text'])
    text(d,(210,235),'Formatting erases data on the selected device.',F_BODY,C['muted'])
    rounded(d,(210,280,1710,505),17,(255,255,255,242),outline=(201,220,229,255),width=1)
    text(d,(237,299),'Detected SATA devices',F_TITLE,C['text'])
    for i,(name,serial,cap,state,selected) in enumerate([
        ('sda','VBafb808d0-79406223','512 MiB','Blank · metadata zero',True),
        ('sdb','VBf3183507-9879e295','512 MiB','Blank · metadata zero',False)]):
        yy=355+i*75
        if selected: rounded(d,(230,yy-9,1687,yy+52),12,(225,242,248,255),outline=(104,169,203,255),width=2)
        text(d,(252,yy),name,F_TITLE,C['text']); text(d,(340,yy+1),serial,F_MED,C['muted'])
        text(d,(1110,yy),cap,F_MED,C['text']); text(d,(1298,yy),state,F_MED,C['teal'])
    rounded(d,(210,526,1710,600),14,(255,242,226,255),outline=(238,187,131,255),width=1)
    text(d,(234,543),'SAFETY CHECK',F_MED,(177,91,24,255))
    text(d,(412,543),'Only initialize a disposable 32 MiB – 2 GiB disk after blank metadata is confirmed.',F_MED,C['text'])
    rounded(d,(210,623,810,824),16,(255,255,255,242),outline=(201,220,229,255),width=1)
    text(d,(236,648),'1. Verify device identity',F_MED,C['text'])
    text(d,(236,689),'2. Initialize blank disk',F_MED,C['text'])
    text(d,(236,730),'3. Enter exact confirmation phrase',F_MED,C['text'])
    rounded(d,(850,623,1710,824),16,(255,255,255,242),outline=(201,220,229,255),width=1)
    text(d,(878,648),'Destructive confirmation',F_TITLE,(177,91,24,255))
    text(d,(878,696),'Type this phrase, then press Enter:',F_MED,C['text'])
    rounded(d,(878,732,1220,782),9,(237,244,248,255),outline=(180,204,216,255),width=1)
    text(d,(895,745),'FORMAT sda',F_TITLE,C['teal'])
    text(d,(1251,745),'Read-only mount after verification',F_SMALL,C['muted'])
    draw_cursor(im,1486,782); im.save(out,optimize=False,compress_level=2)

def about_preview(out, mintde=False):
    im=wallpaper_image(); menu_bar(im)
    d=ImageDraw.Draw(im,'RGBA')
    box=(260,118,1660,824)
    glass_panel(im,box,tint=(239,247,251,226),radius=29,blur=13)
    d=ImageDraw.Draw(im,'RGBA'); draw_traffic_lights(im,302,148)
    title='MintDE' if mintde else 'MintOS26'
    version='Version 1.0.2' if mintde else 'Version 26.5.009.0'
    subtitle='Mint Desktop Environment' if mintde else 'x86-64 operating system'
    icon='settings' if mintde else 'finder'
    color=C['mint'] if mintde else C['blue']
    app_icon(im,icon,326,224,122,color)
    text(d,(486,229),title,F_HERO,C['text'])
    text(d,(489,286),version,F_TITLE,color)
    text(d,(489,331),subtitle,F_BODY,C['muted'])
    rows = ( [('INTERFACE','Menu bar, draggable windows, software pointer and dock'),
              ('BUILT-IN APPS','Files, Notes, Settings, System Monitor and storage tools'),
              ('INPUT','Kernel event service, PS/2 keyboard and mouse'),
              ('STATUS','MintDE 1.0.8; native boot is still experimental')]
            if mintde else
            [('BOOT MODEL','UEFI Desktop is the safe default and recovery environment'),
             ('KERNEL','GDT/TSS, IDT, paging, NX and guarded exception stacks'),
             ('USER SERVICES','ELF64 ring-3 processes and validated system calls'),
             ('STORAGE','FAT16 read-only mount; general file writes not yet enabled')])
    start=398
    for i,(label,value) in enumerate(rows):
        y=start+i*83
        rounded(d,(324,y,1597,y+66),13,(255,255,255,239),outline=(204,222,231,255),width=1)
        d.rectangle((324,y,329,y+66),fill=(*color,255))
        text(d,(349,y+10),label,F_SMALL,C['muted'])
        text(d,(545,y+11),value,F_MED,C['text'])
    text(d,(326,751),'About MintOS26' if not mintde else 'About MintDE',F_SMALL,C['muted'])
    text(d,(326,777),'Separate versions: MintOS26 26.5.009.0  •  MintDE 1.0.8',F_SMALL,C['muted'])
    dock(im)
    im.save(out,optimize=False,compress_level=2)

def main():
    defaults = [
        ROOT/'previews/mintos26-26.5.009.0-native-shell-preview.png',
        ROOT/'previews/mintos26-26.5.009.0-native-shell-settings-preview.png',
        ROOT/'previews/mintos26-26.5.009.0-storage-preview.png',
        ROOT/'previews/mintos26-26.5.009.0-drivers-preview.png',
        ROOT/'previews/mintos26-26.5.009.0-files-preview.png',
        ROOT/'previews/mintos26-26.5.009.0-disk-utility-preview.png',
        ROOT/'previews/mintos26-26.5.009.0-about-mintos-preview.png',
        ROOT/'previews/mintos26-26.5.009.0-about-mintde-preview.png',
        ROOT/'previews/mintos26-26.5.009.0-menu-dropdown-preview.png',
    ]
    outs = [Path(sys.argv[i]) if len(sys.argv)>i else defaults[i-1] for i in range(1,10)]
    for out in outs: out.parent.mkdir(parents=True,exist_ok=True)
    desktop_preview(outs[0]); settings_preview(outs[1]); storage_preview(outs[2]); drivers_preview(outs[3]); files_preview(outs[4]); disk_utility_preview(outs[5]); about_preview(outs[6],False); about_preview(outs[7],True); menu_dropdown_preview(outs[8])
    labels = ['MintDE Clean Workspace','MintDE Settings','Storage & Logs','Hardware & Drivers','MintDE Files','MintDE Disk Utility','About MintOS26','About MintDE 1.0.8','MintDE File Menu Dropdown']
    for label,out in zip(labels,outs):
        print(f'Rendered {label} concept: {out} (1920x1080; static concept, not live VM)')

if __name__=='__main__': main()
