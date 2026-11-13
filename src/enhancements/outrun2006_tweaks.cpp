#include "enhancements/outrun2006_tweaks.hpp"
#include "system/exe_image.hpp"
#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
#include <android/log.h>
#define LOG_TAG "OutRun2006Tweaks"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do { std::printf("[OutRun2006Tweaks] " __VA_ARGS__); std::printf("\n"); } while(0)
#endif

namespace outrun::enhancements {

void apply_outrun2006_tweaks() {
    if (!platform::exe_image_loaded()) {
        return;
    }

    LOGI("=================================================");
    LOGI("Applying emoose/OutRun2006Tweaks suite to memory");
    LOGI("=================================================");

    // 1. UseHiDefCharacters:
    // Forces high-definition models for Alberto, Jennifer, and Clarissa during gameplay
    // 0x6549B0: driver_chrsets[0] = 197 (CHR_DR_MH00)
    // 0x6549D4: heroine_chrsets[3] = 198 (CHR_DR_LH00)
    // 0x6549D8: heroine_chrsets[4] = 199 (CHR_DR_GH00)
    platform::exe_image_patch_u32(0x006549B0u, 197u);
    platform::exe_image_patch_u32(0x006549D4u, 198u);
    platform::exe_image_patch_u32(0x006549D8u, 199u);
    platform::exe_image_patch_u8(0x00487F42u, 197u); // O2SP driver
    platform::exe_image_patch_u8(0x0048803Bu, 198u); // O2SP Jennifer
    platform::exe_image_patch_u8(0x00488045u, 199u); // O2SP Clarissa
    LOGI("[+] UseHiDefCharacters applied (Alberto MH00, Jennifer LH00, Clarissa GH00)");

    // 2. DisableStageCulling:
    // Disables distant stage object culling (bypasses CheckCulling at 0x40501F)
    const std::uint8_t nop2[2] = {0x90, 0x90};
    platform::exe_image_patch(0x0040501Fu, nop2, sizeof(nop2));
    LOGI("[+] DisableStageCulling applied (0x40501F NOP 2)");

    // 3. DisableVehicleLODs:
    // Disables ugly low-poly LOD models on rival and traffic vehicles
    // 0x4AE4E9: NOP; XOR EAX, EAX (forces LOD level to 0)
    const std::uint8_t lod_patch[3] = {0x90, 0x31, 0xC0};
    platform::exe_image_patch(0x004AE4E9u, lod_patch, sizeof(lod_patch));
    LOGI("[+] DisableVehicleLODs applied (0x4AE4E9 NOP/XOR EAX)");

    // 4. FixRightSideBunkiAnimations:
    // Fixes inverted vertices and backwards culling when taking the right highway fork
    const std::uint8_t nop7[7] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    platform::exe_image_patch(0x0044F6DCu, nop7, sizeof(nop7));
    LOGI("[+] FixRightSideBunkiAnimations applied (0x44F6DC NOP 7)");

    // 5. FixIncorrectShading:
    // Replaces broken two-sided lighting shaders with working double-sided vertex shaders
    // 0x73F04C: point VSULT_TWOSIDE_LAMBERT to VSULT_DOUBLE_LAMBERT (0x61FB48)
    // 0x73F050: point VSULT_TWOSIDE_SPECULAR to VSULT_DOUBLE_SPECULAR (0x61FC88)
    platform::exe_image_patch_u32(0x0073F04Cu, 0x0061FB48u);
    platform::exe_image_patch_u32(0x0073F050u, 0x0061FC88u);
    LOGI("[+] FixIncorrectShading applied (double-sided lighting shaders restored)");

    // 6. FixLensFlarePath:
    // Redirects lens flare data from nonexistent common/ to media/
    const char media_lens_path[] = "\\media\\lens_flare_offset.bin";
    platform::exe_image_patch(0x005A29F8u, media_lens_path, sizeof(media_lens_path));
    LOGI("[+] FixLensFlarePath applied (\\media\\lens_flare_offset.bin)");

    // 7. FixBinkLargeMovies:
    // Replaces Bink power-of-two alignment check to support video resolutions > 1024px
    const std::uint8_t bink_patch[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
    platform::exe_image_patch(0x00414730u, bink_patch, sizeof(bink_patch));
    LOGI("[+] FixBinkLargeMovies applied (0x414730)");

    LOGI("OutRun2006Tweaks suite fully installed into game image!");
}

} // namespace outrun::enhancements
