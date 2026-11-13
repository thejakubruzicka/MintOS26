/* Personality syscalls 0x4D58-0x4D5F (26.5.014.0): sound and the boot
 * animation hand-off. Called from kernel/abi/syscall.c with the current
 * address space; every user pointer is validated and copied here. */
#include <stdint.h>
#include <stddef.h>
#include "../include/address_space.h"
#include "../include/hda.h"
#include "../include/boot_splash.h"
#include "../include/log.h"
#include "../include/uapi/mintos/audio.h"
#include "../include/uapi/mintos/syscall.h"

int64_t mint_personality_syscall(const MINT_ADDRESS_SPACE *space, uint64_t number, uint64_t arg0,
                                 uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (number) {
    case MINT_SYS_AUDIO_INFO: {
        static MINT_AUDIO_INFO local;
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(local), 1)) return -MINT_EFAULT;
        if (!mint_audio_get_info(&local)) return -MINT_ENODEV;
        return mint_address_space_copy_to_user(space, arg0, &local, sizeof(local)) ? 0 : -MINT_EFAULT;
    }
    case MINT_SYS_AUDIO_PLAY_SOUND:
        if (arg0 >= MINT_SOUND_COUNT || arg1 > 1000u) return -MINT_EINVAL;
        return mint_audio_play_sound((uint32_t)arg0, (uint32_t)arg1);
    case MINT_SYS_AUDIO_QUEUE: {
        if (!arg1 || (arg2 != 1u && arg2 != 2u) || arg3 > 1000u || arg1 > MINT_AUDIO_QUEUE_MAX_FRAMES) return -MINT_EINVAL;
        uint64_t samples = arg1 * arg2;
        if (samples > MINT_AUDIO_QUEUE_MAX_FRAMES) return -MINT_EINVAL;
        size_t bytes = (size_t)samples * sizeof(int16_t);
        if (arg0 > UINT64_MAX - bytes || !mint_address_space_validate_user_range(space, arg0, bytes, 0)) return -MINT_EFAULT;
        int32_t slot = -1;
        int16_t *dst = mint_audio_pool_acquire((uint32_t)samples, &slot);
        if (!dst) return mint_audio_driver_state() == MINT_AUDIO_RUNNING ? -MINT_EBUSY : -MINT_ENODEV;
        if (!mint_address_space_copy_from_user(space, dst, arg0, bytes)) return -MINT_EFAULT;
        return mint_audio_pool_play(slot, (uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3);
    }
    case MINT_SYS_AUDIO_VOLUME:
        if (arg0 > 0xFFFFu || arg1 > MINT_AUDIO_MUTE_KEEP) return -MINT_EINVAL;
        return mint_audio_set_volume((uint32_t)arg0, (uint32_t)arg1);
    case MINT_SYS_AUDIO_CONTROL:
        if (arg0 == MINT_AUDIO_OP_START) {
            if (arg1 != MINT_AUDIO_START_TOKEN) return -MINT_EPERM;
            mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_AUDIO, "explicit audio driver start requested");
            return mint_hda_start() ? 0 : (mint_audio_driver_state() == MINT_AUDIO_ABSENT ? -MINT_ENODEV : -MINT_EIO);
        }
        if (arg0 == MINT_AUDIO_OP_STOP) return mint_hda_stop() ? 0 : -MINT_EIO;
        if (arg0 == MINT_AUDIO_OP_SILENCE) return mint_audio_silence();
        return -MINT_EINVAL;
    case MINT_SYS_SYSTEM_STATUS: {
        MINT_SYSTEM_STATUS st;
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(st), 1)) return -MINT_EFAULT;
        if (!mint_system_status(&st)) return -MINT_ENODEV;
        return mint_address_space_copy_to_user(space, arg0, &st, sizeof(st)) ? 0 : -MINT_EFAULT;
    }
    case MINT_SYS_BOOT_SPLASH:
        if (arg0 == MINT_SPLASH_OP_FINISH) { mint_boot_splash_finish(); return 0; }
        if (arg0 == MINT_SPLASH_OP_STATUS) return mint_boot_splash_active() ? 1 : 0;
        return -MINT_EINVAL;
    default:
        return -MINT_ENOSYS;
    }
}
