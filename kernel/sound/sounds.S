/* MintOS built-in sounds (26.5.014.0): 48 kHz signed 16-bit PCM generated
 * at build time by tools/make_sounds.py into build/sounds/ (original
 * synthesis, no samples). Startup and notification are stereo, the rest mono. */
.section .rodata
.macro MINT_SOUND name, file
.balign 64
.global mint_sound_\name\()_start
.global mint_sound_\name\()_end
.type mint_sound_\name\()_start,@object
mint_sound_\name\()_start:
.incbin "build/sounds/\file\().pcm"
mint_sound_\name\()_end:
.size mint_sound_\name\()_start, mint_sound_\name\()_end-mint_sound_\name\()_start
.endm
MINT_SOUND startup, startup
MINT_SOUND trash, trash
MINT_SOUND empty_trash, empty_trash
MINT_SOUND notification, notification
MINT_SOUND volume, volume
MINT_SOUND screenshot, screenshot
MINT_SOUND alert_mint, alert_mint
MINT_SOUND alert_dewdrop, alert_dewdrop
MINT_SOUND alert_fern, alert_fern
MINT_SOUND alert_ripple, alert_ripple
MINT_SOUND alert_woodblock, alert_woodblock
