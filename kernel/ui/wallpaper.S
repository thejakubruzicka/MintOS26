/* Read-only full-resolution RGB wallpaper resource (1920x1080x3). */
.section .rodata
.balign 16
.global mint_native_wallpaper_rgb
.global mint_native_wallpaper_rgb_end
.type mint_native_wallpaper_rgb,@object
mint_native_wallpaper_rgb:
.incbin "assets/wallpaper-1920x1080.rgb"
mint_native_wallpaper_rgb_end:
.size mint_native_wallpaper_rgb, mint_native_wallpaper_rgb_end-mint_native_wallpaper_rgb
