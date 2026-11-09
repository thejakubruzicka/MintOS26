/* Read-only full-resolution RGB wallpaper resources (1920x1080x3), light and dark. */
.section .rodata
.balign 16
.global mint_native_wallpaper_rgb
.global mint_native_wallpaper_rgb_end
.type mint_native_wallpaper_rgb,@object
mint_native_wallpaper_rgb:
.incbin "assets/wallpaper-1920x1080.rgb"
mint_native_wallpaper_rgb_end:
.size mint_native_wallpaper_rgb, mint_native_wallpaper_rgb_end-mint_native_wallpaper_rgb
/* 26.5.010.0: dark-appearance variant of the original "Mint Waves" design. */
.balign 16
.global mint_native_wallpaper_dark_rgb
.global mint_native_wallpaper_dark_rgb_end
.type mint_native_wallpaper_dark_rgb,@object
mint_native_wallpaper_dark_rgb:
.incbin "assets/wallpaper-dark-1920x1080.rgb"
mint_native_wallpaper_dark_rgb_end:
.size mint_native_wallpaper_dark_rgb, mint_native_wallpaper_dark_rgb_end-mint_native_wallpaper_dark_rgb
