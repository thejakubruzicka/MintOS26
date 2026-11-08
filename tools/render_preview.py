#!/usr/bin/env python3
"""Render polished desktop, launcher, and diagnostic-window concepts (not live UEFI screenshots)."""
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont,ImageFilter
import sys, math
ROOT=Path(__file__).resolve().parents[1]
WALL=Image.open(ROOT/'assets/wallpaper-master.png').convert('RGB').resize((1920,1080),Image.Resampling.LANCZOS)
FONT='/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf';BOLD='/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
def font(sz,bold=False): return ImageFont.truetype(BOLD if bold else FONT,sz)
def rr(draw,box,r,fill,outline=None,width=1): draw.rounded_rectangle(box,radius=r,fill=fill,outline=outline,width=width)
def glass(im,box,radius=26,fill=(16,29,44,150),blur=10,outline=(255,255,255,70)):
    x0,y0,x1,y1=box
    if blur:
        crop=im.crop((max(0,x0-blur*2),max(0,y0-blur*2),min(im.width,x1+blur*2),min(im.height,y1+blur*2))).filter(ImageFilter.GaussianBlur(blur))
        overlay=Image.new('RGBA',crop.size,(0,0,0,0)); d=ImageDraw.Draw(overlay);d.rounded_rectangle((blur*2,blur*2,blur*2+x1-x0,blur*2+y1-y0),radius=radius,fill=fill,outline=outline,width=2);crop=Image.alpha_composite(crop.convert('RGBA'),overlay);im.paste(crop.crop((blur*2,blur*2,blur*2+x1-x0,blur*2+y1-y0)).convert('RGB'),(x0,y0))
    else:
        ov=Image.new('RGBA',(x1-x0,y1-y0),fill);im.paste(Image.alpha_composite(im.crop(box).convert('RGBA'),ov).convert('RGB'),(x0,y0))
def icon(im,cx,cy,kind,color,size=82):
    d=ImageDraw.Draw(im,'RGBA');r=size//4;rr(d,(cx-size//2,cy-size//2,cx+size//2,cy+size//2),size//4,(*color,244),(255,255,255,120),2)
    white=(248,252,255,255)
    if kind=='apps':
        for yy in range(3):
            for xx in range(3):rr(d,(cx-23+xx*17,cy-23+yy*17,cx-10+xx*17,cy-10+yy*17),4,white)
    elif kind=='files':
        rr(d,(cx-26,cy-16,cx+26,cy+21),5,white);rr(d,(cx-22,cy-24,cx-1,cy-13),4,(255,207,97,255));rr(d,(cx-23,cy-13,cx+23,cy+16),4,(205,222,244,255))
    elif kind=='terminal':
        rr(d,(cx-27,cy-25,cx+27,cy+25),8,(15,33,50,255));d.line((cx-16,cy-4,cx-5,cy+3,cx-16,cy+10),fill=(*color,255),width=4);d.line((cx,cy+11,cx+15,cy+11),fill=white,width=4)
    elif kind=='notes':
        rr(d,(cx-24,cy-28,cx+24,cy+28),6,white);d.line((cx-12,cy-9,cx+12,cy-9),fill=(*color,255),width=3);d.line((cx-12,cy+1,cx+12,cy+1),fill=(*color,255),width=3);d.line((cx-12,cy+11,cx+5,cy+11),fill=(*color,255),width=3)
    elif kind=='calc':
        rr(d,(cx-24,cy-27,cx+24,cy+27),7,(28,39,58,255));rr(d,(cx-16,cy-19,cx+16,cy-5),3,(153,225,210,255))
        for yy in range(3):
            for xx in range(3):rr(d,(cx-16+xx*13,cy+2+yy*12,cx-8+xx*13,cy+10+yy*12),3,white)
    elif kind=='settings':
        d.ellipse((cx-24,cy-24,cx+24,cy+24),fill=white);d.ellipse((cx-10,cy-10,cx+10,cy+10),fill=(*color,255))
    elif kind=='monitor':
        rr(d,(cx-25,cy-20,cx+25,cy+13),6,white);d.rectangle((cx-4,cy+13,cx+4,cy+21),fill=white);d.rectangle((cx-16,cy+21,cx+16,cy+25),fill=white)

def label(d,xy,text,sz,color,bold=False,anchor=None):d.text(xy,text,font=font(sz,bold),fill=color,anchor=anchor)
def create(path,launcher=False,monitor=False,settings=False,finder=False):
    im=WALL.copy().convert('RGBA');d=ImageDraw.Draw(im,'RGBA')
    # top glass menu strip
    glass(im,(0,0,W:=1920,58),0,(12,25,38,172),7,(255,255,255,95));d=ImageDraw.Draw(im,'RGBA')
    label(d,(28,15),'mintOS26',31,(255,255,255,255),True)
    menu=['Desktop','File','Edit','View','Window','Help'];x=220
    for t in menu:label(d,(x,20),t,18,(250,253,255,240),t=='Desktop');x+=d.textlength(t,font=font(18,bold=t=='Desktop'))+28
    label(d,(1888,18),'10:08',16,(255,255,255,255),True,anchor='ra')
    # top left identity
    label(d,(68,86),'mintOS',66,(255,255,255,255),True);label(d,(340,105),'26',42,(28,227,197,255),True)
    # Keep the wallpaper/wordmark clean; desktop taglines were removed from the live shell.
    # desktop shortcuts
    icon(im,1778,220,'files',(33,199,183));d=ImageDraw.Draw(im,'RGBA');label(d,(1778,275),'Documents',16,(255,255,255,245),True,anchor='mt')
    icon(im,1778,365,'notes',(104,104,224));label(d,(1778,420),'Notes',16,(255,255,255,245),True,anchor='mt')
    icon(im,1778,510,'monitor',(48,130,139));label(d,(1778,565),'System',16,(255,255,255,245),True,anchor='mt')
    # dock glass
    glass(im,(34,898,1886,1048),46,(10,25,39,164),20,(255,255,255,92));d=ImageDraw.Draw(im,'RGBA')
    icon(im,100,969,'apps',(22,203,181),76);d.line((161,925,161,1018),fill=(255,255,255,120),width=2)
    items=[('files',(39,199,184)),('terminal',(218,62,104)),('notes',(123,81,214)),('calc',(206,158,57)),('settings',(80,107,213)),('monitor',(36,150,135))]
    xs=[700,820,940,1060,1180,1300]
    for (kind,col),cx in zip(items,xs):icon(im,cx,961,kind,col,80)
    label(d,(100,1020),'Apps',13,(255,255,255,235),True,anchor='mt')
    # active app window, or app launcher state
    if launcher:
        shadow=Image.new('RGBA',im.size,(0,0,0,0));sd=ImageDraw.Draw(shadow);sd.rounded_rectangle((268,152,1658,835),radius=28,fill=(0,0,0,125));shadow=shadow.filter(ImageFilter.GaussianBlur(26));im=Image.alpha_composite(im,shadow);glass(im,(250,132,1640,817),28,(237,246,250,218),5,(255,255,255,230));d=ImageDraw.Draw(im,'RGBA')
        label(d,(290,158),'●',22,(255,94,98,255));label(d,(323,158),'●',22,(255,190,67,255));label(d,(356,158),'●',22,(47,199,126,255));label(d,(398,160),'Applications',22,(37,54,70,255),True)
        label(d,(296,218),'More and more Mint.',32,(29,49,64,255),True);label(d,(297,262),'A growing workspace for your files, tools and ideas.',18,(78,102,117,255))
        apps=[('files','Finder','Browse files',(34,193,178)),('terminal','Terminal','Commands',(216,55,98)),('notes','Notes','Quick notes',(125,82,216)),('calc','Calculator','Everyday math',(202,151,49)),('settings','Settings','Personalize',(73,102,210)),('monitor','Monitor','System health',(33,144,134)),('notes','Calendar','Month view',(230,100,117)),('calc','Clock','Time and zone',(51,132,183)),('terminal','Code Studio','Edit C files',(39,61,82)),('settings','Network','Adapter status',(31,158,155)),('files','Install Assistant','Recovery tools',(101,116,140)),('apps','About MintOS26','Version info',(91,139,113))]
        for i,(k,n,desc,col) in enumerate(apps):
            xx=284+(i%4)*332;yy=294+(i//4)*160
            rr(d,(xx,yy,xx+314,yy+142),20,(248,252,254,234),(207,224,233,245),2);icon(im,xx+53,yy+69,k,col,68);d=ImageDraw.Draw(im,'RGBA');label(d,(xx+100,yy+43),n,18,(37,55,68,255),True);
            descfont=font(13); dd=desc
            while d.textlength(dd,font=descfont)>202 and len(dd)>4: dd=dd[:-4]+'…'
            label(d,(xx+100,yy+76),dd,13,(92,111,125,255));label(d,(xx+100,yy+106),'Open  ›',13,(18,154,142,255),True)
    elif monitor:
        shadow=Image.new('RGBA',im.size,(0,0,0,0));sd=ImageDraw.Draw(shadow);sd.rounded_rectangle((350,160,1570,858),radius=28,fill=(0,0,0,110));shadow=shadow.filter(ImageFilter.GaussianBlur(24));im=Image.alpha_composite(im,shadow);glass(im,(330,140,1550,838),28,(247,250,252,249),0,(255,255,255,240));d=ImageDraw.Draw(im,'RGBA')
        label(d,(356,154),'●',22,(255,94,98,255));label(d,(389,154),'●',22,(255,190,67,255));label(d,(422,154),'●',22,(47,199,126,255));label(d,(466,157),'System Monitor',22,(37,55,68,255),True)
        label(d,(382,218),'System overview',30,(29,49,64,255),True)
        cards=[(382,273,718,353,'DISPLAY','1920 × 1080'),(738,273,1074,353,'INPUT LOOP','UEFI events + 100 Hz timer'),(1094,273,1498,353,'STORAGE','Firmware-managed volume')]
        for x0,y0,x1,y1,cap,value in cards:
            rr(d,(x0,y0,x1,y1),15,(235,243,247,255),(213,226,233,255),2);label(d,(x0+18,y0+13),cap,13,(86,108,121,255),True);label(d,(x0+18,y0+39),value,18,(34,57,71,255),True)
        label(d,(382,390),'Pointer diagnostics',24,(29,49,64,255),True)
        label(d,(382,430),'Protocols   Absolute A: 1     Relative R: 4',19,(58,78,91,255),True)
        label(d,(382,469),'Polls OK   A: 0   R: 218     |     Motion   A: 0   R: 0',19,(58,78,91,255))
        label(d,(382,508),'Not ready   A: 0   R: 114     |     Errors   A: 0   R: 0     |     badA: 0',17,(58,78,91,255))
        label(d,(382,547),'Event wakeups   Timer: 504   Keyboard: 2   Absolute: 0   Relative: 0',17,(58,78,91,255))
        rr(d,(382,602,1498,674),15,(226,242,239,255),(196,225,218,255),1);label(d,(404,618),'KEY DISTINCTION',13,(22,119,103,255),True);label(d,(404,641),'A successful poll is not the same as a real mouse movement report.',18,(32,82,75,255),True)
        label(d,(382,713),'Diagnostic values shown here are illustrative, not a live VM report.',14,(101,120,132,255))
    elif settings:
        shadow=Image.new('RGBA',im.size,(0,0,0,0));sd=ImageDraw.Draw(shadow);sd.rounded_rectangle((270,145,1658,840),radius=30,fill=(0,0,0,105));shadow=shadow.filter(ImageFilter.GaussianBlur(25));im=Image.alpha_composite(im,shadow)
        glass(im,(250,132,1640,817),28,(231,244,250,178),14,(255,255,255,205));d=ImageDraw.Draw(im,'RGBA')
        label(d,(284,148),'●',22,(255,94,98,255));label(d,(317,148),'●',22,(255,190,67,255));label(d,(350,148),'●',22,(47,199,126,255));label(d,(392,151),'Settings',22,(37,54,70,255),True)
        label(d,(288,205),'Preferences',34,(29,49,64,255),True)
        rr(d,(288,249,468,284),15,(23,192,174,245));label(d,(310,257),'Appearance',17,(255,255,255,255),True)
        rr(d,(479,249,721,284),15,(245,250,253,178),(255,255,255,120),1);label(d,(499,257),'Language & Time',17,(45,66,80,255))
        rows=[('Warm alpine','Selected theme',(23,192,174,255)),('Mint clarity','', (36,194,121,255)),('Apricot glow','',(74,193,220,255)),('Startup chime (PC speaker only)','On',None),('Liquid-glass panels and dock blur','On',None),('Pointer style','Mint arrow',None),('Display mode','1920 × 1080',None)]
        for i,(name,val,col) in enumerate(rows):
            yy=297+i*53
            rr(d,(288,yy,1554,yy+44),12,(244,249,252,176),(255,255,255,85),1)
            label(d,(306,yy+12),name,17,(39,59,73,255),i<3)
            if col: d.ellipse((1499,yy+12,1519,yy+32),fill=col)
            else:
                rr(d,(1090,yy+5,1537,yy+39),10,(252,254,255,225),(220,234,240,230),1)
                label(d,(1110,yy+13),val,15,(26,172,162,255),True);label(d,(1507,yy+13),'⌄',17,(68,91,106,255),True)
        # Open dropdown illustrates the intended interaction and legibility.
        glass(im,(1084,674,1543,811),14,(248,252,254,244),4,(255,255,255,230));d=ImageDraw.Draw(im,'RGBA')
        for i,opt in enumerate(['Classic arrow','Mint arrow','Precision crosshair']):
            yy=683+i*40
            if i==1: rr(d,(1092,yy,1535,yy+34),9,(24,190,171,235))
            label(d,(1110,yy+8),opt,15,(255,255,255,255) if i==1 else (36,56,70,255),i==1)
    elif finder:
        shadow=Image.new('RGBA',im.size,(0,0,0,0));sd=ImageDraw.Draw(shadow);sd.rounded_rectangle((268,145,1658,840),radius=30,fill=(0,0,0,105));shadow=shadow.filter(ImageFilter.GaussianBlur(25));im=Image.alpha_composite(im,shadow)
        glass(im,(250,132,1640,817),28,(232,244,250,185),12,(255,255,255,205));d=ImageDraw.Draw(im,'RGBA')
        label(d,(284,148),'●',22,(255,94,98,255));label(d,(317,148),'●',22,(255,190,67,255));label(d,(350,148),'●',22,(47,199,126,255));label(d,(392,151),'Finder',22,(37,54,70,255),True)
        label(d,(286,207),'Files & Volumes',32,(29,49,64,255),True);label(d,(287,250),'\\MINTOS26\\DOCUMENTS',15,(78,102,117,255))
        controls=[('↑ Up',288,260,385),('New folder',398,260,556),('New file',568,260,706),('Delete',718,260,830),('Open',842,260,946),('Previous',960,260,1088),('Next',1100,260,1209)]
        for name,x,y,x1 in controls: rr(d,(x,y,x1,y+40),10,(23,192,174,230) if name in ('↑ Up','Open') else (245,250,253,205),(255,255,255,135),1);label(d,(x+12,y+11),name,13,(255,255,255,255) if name in ('↑ Up','Open') else (39,59,73,255),True)
        rr(d,(286,318,508,756),16,(242,248,251,196),(255,255,255,130),1);label(d,(306,336),'LOCATIONS',12,(83,107,122,255),True)
        for i,(name,sub) in enumerate([('Boot volume','RO · 32 MiB'),('Data volume','RW · 512 MiB'),('EFI System','RO · 32 MiB')]):
            yy=367+i*69;rr(d,(298,yy,496,yy+56),10,(23,192,174,232) if i==1 else (252,254,255,170));label(d,(311,yy+8),name,13,(255,255,255,255) if i==1 else (37,57,71,255),True);label(d,(311,yy+30),sub,11,(229,252,250,255) if i==1 else (91,111,125,255))
        rr(d,(525,318,1604,756),16,(250,253,255,173),(255,255,255,140),1);label(d,(552,335),'NAME',12,(83,107,122,255),True);label(d,(1439,335),'SIZE',12,(83,107,122,255),True)
        rows=[('Folder','Documents'),('Folder','Develop'),('Folder','Install'),('Folder','Logs'),('File','NOTES.TXT'),('File','WELCOME.TXT'),('File','SETTINGS.CFG'),('File','KERNEL.ELF')]
        for i,(kind,name) in enumerate(rows):
            yy=359+i*43
            if i==5: rr(d,(538,yy,1590,yy+38),9,(23,192,174,218))
            label(d,(552,yy+10),'▰' if kind=='Folder' else '▤',15,(255,255,255,255) if i==5 else (24,174,158,255),True)
            label(d,(582,yy+10),name,15,(255,255,255,255) if i==5 else (38,58,72,255),i==5)
            label(d,(1439,yy+10),'Folder' if kind=='Folder' else ('1.2 KB' if name=='WELCOME.TXT' else '86 B'),13,(245,255,255,255) if i==5 else (85,105,119,255))
        glass(im,(525,773,1604,814),12,(245,251,253,213),3,(255,255,255,110));d=ImageDraw.Draw(im,'RGBA');label(d,(547,785),'WELCOME.TXT  ·  Text preview selected. Double-click or Enter to open.',13,(40,61,75,255))
    else:
        shadow=Image.new('RGBA',im.size,(0,0,0,0));sd=ImageDraw.Draw(shadow);sd.rounded_rectangle((430,230,1490,775),radius=26,fill=(0,0,0,95));shadow=shadow.filter(ImageFilter.GaussianBlur(25));im=Image.alpha_composite(im,shadow);glass(im,(410,208,1470,753),26,(246,250,253,247),0,(255,255,255,240));d=ImageDraw.Draw(im,'RGBA');label(d,(435,224),'●',22,(255,94,98,255));label(d,(468,224),'●',22,(255,190,67,255));label(d,(501,224),'●',22,(47,199,126,255));label(d,(545,227),'Welcome to MintOS26',21,(42,61,75,255),True)
        rr(d,(455,292,1425,690),20,(235,243,247,255));label(d,(510,342),'Ready for a fresh start?',32,(29,57,70,255),True);label(d,(512,403),'A high-resolution UEFI desktop with a place for',20,(92,115,129,255));label(d,(512,435),'your files, notes, tools and ideas.',20,(92,115,129,255));rr(d,(512,510,760,572),18,(24,190,171,255));label(d,(636,530),'Open Applications',17,(255,255,255,255),True,anchor='mt');icon(im,1282,485,'apps',(24,190,171),110);d=ImageDraw.Draw(im,'RGBA');label(d,(1282,559),'Made for making.',16,(69,89,101,255),True,anchor='mt')
    # These concept boards are PNGs; PNG's exhaustive optimize pass is very slow
    # for large alpha-composited artwork. Low compression is lossless and keeps
    # preview generation quick enough for a release build.
    im.convert('RGB').save(path, optimize=False, compress_level=1)
if __name__=='__main__':
    out1=Path(sys.argv[1]);out2=Path(sys.argv[2]);create(out1,False);create(out2,True)
    if len(sys.argv)>3:create(Path(sys.argv[3]),False,True)
    if len(sys.argv)>4:create(Path(sys.argv[4]),False,False,True)
    if len(sys.argv)>5:create(Path(sys.argv[5]),False,False,False,True)
    print('Rendered 1920x1080 concept previews (not VM screenshots).')
