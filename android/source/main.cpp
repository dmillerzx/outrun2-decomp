#include <android/log.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <pthread.h>

#include "runtime/game_host.hpp"
#include "platform/pc_scene_renderer.hpp"
#include "platform/native_runtime.hpp"
#include "platform/retail_asset_store.hpp"
#include "system/exe_image.hpp"
#include "enhancements/gpu_backend.hpp"
#include "enhancements/frame_rate.hpp"
#include "enhancements/outrun2006_tweaks.hpp"
#include "enhancements/vibration.hpp"
#include "audio.hpp"
#include "renderer.hpp"
#include "vk_context.hpp"
#include <jni.h>
#include <SDL_haptic.h>
#include <SDL_system.h>

#define LOG_TAG "OutRun2006-Android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace rt = outrun::runtime;

namespace {

static SDL_GameController* s_active_controller = nullptr;
static SDL_Haptic* s_active_haptic = nullptr;

static void trigger_android_haptics(float left_motor, float right_motor) {
    const float max_intensity = std::max(left_motor, right_motor);
    const Uint16 left_u16 = (max_intensity > 0.01f) ? static_cast<Uint16>(std::clamp(int(left_motor * 65535.0f), 0, 65535)) : 0;
    const Uint16 right_u16 = (max_intensity > 0.01f) ? static_cast<Uint16>(std::clamp(int(right_motor * 65535.0f), 0, 65535)) : 0;

    if (s_active_controller) {
        SDL_GameControllerRumble(s_active_controller, left_u16, right_u16, max_intensity > 0.01f ? 50 : 0);
        SDL_Joystick* joy = SDL_GameControllerGetJoystick(s_active_controller);
        if (joy) {
            SDL_JoystickRumble(joy, left_u16, right_u16, max_intensity > 0.01f ? 50 : 0);
        }
    }
    if (s_active_haptic) {
        if (max_intensity > 0.01f) {
            SDL_HapticRumblePlay(s_active_haptic, max_intensity, 50);
        } else {
            SDL_HapticRumbleStop(s_active_haptic);
        }
    }

    // Direct JNI actuation to SDLControllerManager
    JNIEnv* env = reinterpret_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (env) {
        static jclass s_mgr_class = nullptr;
        static jmethodID s_haptic_run_mid = nullptr;
        static jmethodID s_haptic_stop_mid = nullptr;
        if (!s_mgr_class) {
            jclass local = env->FindClass("org/libsdl/app/SDLControllerManager");
            if (local) {
                s_mgr_class = reinterpret_cast<jclass>(env->NewGlobalRef(local));
                env->DeleteLocalRef(local);
                s_haptic_run_mid = env->GetStaticMethodID(s_mgr_class, "hapticRun", "(IFI)V");
                s_haptic_stop_mid = env->GetStaticMethodID(s_mgr_class, "hapticStop", "(I)V");
            }
        }
        if (s_mgr_class) {
            if (max_intensity > 0.01f && s_haptic_run_mid) {
                env->CallStaticVoidMethod(s_mgr_class, s_haptic_run_mid, 999999, max_intensity, 50);
            } else if (s_haptic_stop_mid) {
                env->CallStaticVoidMethod(s_mgr_class, s_haptic_stop_mid, 999999);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    static int s_log_haptic_interval = 0;
    if (max_intensity > 0.05f && ++s_log_haptic_interval % 60 == 1) {
        LOGI("Haptic vibration active: L=%.2f R=%.2f max=%.2f", left_motor, right_motor, max_intensity);
    }
}

static int s_pfd[2];
static pthread_t s_log_thr;
static void* log_thread_func(void*) {
    ssize_t rdsz;
    char buf[2048];
    while ((rdsz = read(s_pfd[0], buf, sizeof(buf) - 1)) > 0) {
        if (buf[rdsz - 1] == '\n') --rdsz;
        buf[rdsz] = 0;
        __android_log_print(ANDROID_LOG_INFO, "OutRun2006-Decomp", "%s", buf);
    }
    return nullptr;
}

static void redirect_stdout_to_logcat() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (pipe(s_pfd) == 0) {
        dup2(s_pfd[1], 1);
        dup2(s_pfd[1], 2);
        pthread_create(&s_log_thr, nullptr, log_thread_func, nullptr);
        pthread_detach(s_log_thr);
    }
}

class AndroidPresenter final : public rt::Presenter {
public:
    explicit AndroidPresenter(outrun::ps5_runtime::Ps5Renderer& r, rt::GameHost& host, outrun::platform::NativeRuntimeContext& context)
        : r_(r), host_(host), context_(context) {}
    bool set_start_loading_scene(std::uint32_t scene) override { return outrun::ps5_runtime::ps5_renderer_set_start_loading_scene(r_, scene); }
    bool set_start_loading_visible(bool v) override { return outrun::ps5_runtime::ps5_renderer_set_start_loading_visible(r_, v); }
    bool set_frontend_visible(bool v) override { return outrun::ps5_runtime::ps5_renderer_set_frontend_visible(r_, v); }
    bool set_frontend_glyphs(const std::vector<outrun::platform::FrontendGlyph>& g) override { return outrun::ps5_runtime::ps5_renderer_set_frontend_glyphs(r_, g); }
    bool set_frontend_images(const std::vector<outrun::platform::FrontendListImage>& i) override { return outrun::ps5_runtime::ps5_renderer_set_frontend_images(r_, i); }
    bool set_frontend_icons(const std::vector<outrun::platform::FrontendWindowIcon>& i) override { return outrun::ps5_runtime::ps5_renderer_set_frontend_icons(r_, i); }
    bool set_frontend_token(std::uint32_t t) override { return outrun::ps5_runtime::ps5_renderer_set_frontend_token(r_, t); }
    bool set_frontend_overlay(std::uint32_t t, float f) override { return outrun::ps5_runtime::ps5_renderer_set_frontend_overlay(r_, t, f); }
    bool set_frontend_game_backdrop(bool v) override { return outrun::ps5_runtime::ps5_renderer_set_frontend_game_backdrop(r_, v); }
    bool set_movie_frame(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h) override { return outrun::ps5_runtime::ps5_renderer_set_movie_frame(r_, rgba, w, h); }
    bool set_pc_scene(std::function<void(outrun::platform::PcD3D9Device&)> scene) override { return outrun::ps5_runtime::ps5_renderer_set_pc_scene(r_, std::move(scene)); }
    bool draw() override {
        // Enforce 60.0 Hz frame pacing (16.666 ms per frame) to prevent game logic fast-forward
        using clock = std::chrono::steady_clock;
        static auto target_time = clock::now();
        const auto now = clock::now();
        if (now < target_time) {
            std::this_thread::sleep_for(target_time - now);
        }
        target_time = std::max(clock::now(), target_time + std::chrono::nanoseconds(16666667));

        auto result = outrun::ps5_runtime::ps5_renderer_draw(r_);
        if (!result) {
            const auto err = outrun::ps5_runtime::ps5_renderer_pc_error(r_);
            if (++draw_count_ % 120 == 0) {
                LOGI("[Presenter::draw] transient surface/swapchain unavailable (%s); maintaining host simulation", err.c_str());
            }
            // On mobile Android, sleep/wake and shade transitions destroy ANativeWindow.
            // Never return false to game_host, which permanently latches render_failed=true.
            return true;
        }
        if (++draw_count_ % 120 == 0) {
            const auto stats = outrun::ps5_runtime::ps5_renderer_stats(r_);
            const auto& start = context_.start_mode;
            LOGI("[Presenter::draw] res=%d frame=%u stats: f=%u l=%u fe=%u | mode=%u start_act=%d stage=%u own_stg=%u miss=0x%x own_miss=0x%x fault=0x%x ('%s') waits=%u",
                 int(result), draw_count_, stats.frames, stats.loading_frames,
                 stats.frontend_frames,
                 context_.mode_state.current,
                 int(start.active),
                 start.stage,
                 start.scene_owner_stage,
                 start.last_missing_service,
                 start.scene_owner_missing_service,
                 start.scene_owner_fault,
                 start.scene_owner_fault_reason.c_str(),
                 start.gate_waits);
        }

        // Controller Vibration / Force Feedback haptic feedback:
        // Strictly active only during live race modes (mode 16=Race, mode 18=Heart Attack/Course)
        const bool in_race = (context_.mode_state.current == 16u || context_.mode_state.current == 18u);
        if (in_race) {
            const auto& car_data = context_.event_function36.car_select.car_799d18;
            if (!car_data.empty()) {
                outrun::driving::Bytes car(const_cast<std::uint8_t*>(car_data.data()), car_data.size());
                outrun::enhancements::VibrationState vib{};
                outrun::enhancements::update_car_vibration(car, vib, 1.0f);
                trigger_android_haptics(vib.left_motor, vib.right_motor);
            } else {
                trigger_android_haptics(0.0f, 0.0f);
            }
        } else {
            // Instantly silence all rumble whenever outside of active racing (menus, pause, attract)
            trigger_android_haptics(0.0f, 0.0f);
        }

        return true;
    }
    bool frontend_animation_complete() const override { return outrun::ps5_runtime::ps5_renderer_stats(r_).frontend_animation_complete; }
    void scene_to_frame() override { outrun::ps5_runtime::ps5_renderer_scene_to_frame(r_); }
    void cycle_pc_diagnostic() override {}
    void disable_pc_specialised_shaders() override {}
private:
    outrun::ps5_runtime::Ps5Renderer& r_;
    rt::GameHost& host_;
    outrun::platform::NativeRuntimeContext& context_;
    unsigned draw_count_{0};
};

class AndroidAudioOutput final : public rt::AudioOutput {
public:
    bool open(std::string& e) override { return audio_.open(e); }
    bool submit(const std::vector<std::int16_t>& pcm, std::string& e) override { return audio_.submit(pcm, e); }
    void close() override { audio_.close(); }
    std::uint64_t submitted_frames() const override { return audio_.submitted_frames(); }
    std::size_t queued_frames() const override { return audio_.queued_frames(); }
private:
    outrun::ps5_runtime::Ps5Audio audio_;
};

// Search list for OutRun 2006 PC data
std::string find_game_directory() {
    const std::vector<std::string> candidate_paths = {
        "/storage/EEEF-F762/ROMs/ports/outrun2006",
        "/storage/emulated/0/ROMs/ports/outrun2006",
        "/sdcard/ROMs/ports/outrun2006",
        "/sdcard/Android/data/org.outrun2.c2c/files"
    };

    for (const auto& path : candidate_paths) {
        std::string check = path + "/" + outrun::platform::ExeImageCacheName;
        std::FILE* f = std::fopen(check.c_str(), "rb");
        if (f) {
            std::fclose(f);
            LOGI("Found valid game cache in: %s", path.c_str());
            return path;
        }
    }
    return candidate_paths[0]; // fallback
}

} // namespace

extern "C" int SDL_main(int argc, char* argv[]) {
    redirect_stdout_to_logcat();

    LOGI("==========================================");
    LOGI("OutRun 2006 Coast 2 Coast - Android Native");
    LOGI("==========================================");

    rt::product() = {"OutRun 2006 Coast 2 Coast", "outrun2006.log"};
    rt::home() = find_game_directory();
    LOGI("Application home set to: %s", rt::home().c_str());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0) {
        LOGE("Failed to initialize SDL: %s", SDL_GetError());
        return 1;
    }

    // Retroid Pocket 5 display resolution: 1920x1080
    const int target_w = 1920;
    const int target_h = 1080;

    SDL_Window* window = SDL_GL_GetCurrentWindow();
    if (!window) {
        window = SDL_GetWindowFromID(1);
    }
    if (!window) {
        window = SDL_CreateWindow(
            "OutRun 2006 Coast 2 Coast",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            target_w,
            target_h,
            SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN
        );
    }

    if (!window) {
        LOGE("Failed to create or acquire SDL window: %s", SDL_GetError());
        std::exit(1);
    }

    LOGI("Created 1080p Vulkan fullscreen window");
    outrun::ps5_runtime::vulkan::set_android_window(window);

    // Gamepad controller detection
    SDL_GameController* controller = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            controller = SDL_GameControllerOpen(i);
            if (controller) {
                LOGI("Connected GameController: %s", SDL_GameControllerName(controller));
                s_active_controller = controller;
                break;
            }
        }
    }

    if (s_active_controller && !s_active_haptic) {
        SDL_Joystick* joy = SDL_GameControllerGetJoystick(s_active_controller);
        if (joy) {
            s_active_haptic = SDL_HapticOpenFromJoystick(joy);
        }
    }
    if (!s_active_haptic && SDL_NumHaptics() > 0) {
        s_active_haptic = SDL_HapticOpen(0);
    }
    if (s_active_haptic) {
        if (SDL_HapticRumbleInit(s_active_haptic) < 0) {
            LOGI("SDL_HapticRumbleInit not supported: %s", SDL_GetError());
            SDL_HapticClose(s_active_haptic);
            s_active_haptic = nullptr;
        } else {
            LOGI("Initialized SDL Haptic device: %s", SDL_HapticName(SDL_HapticIndex(s_active_haptic)));
        }
    }

    rt::GameHost diag{};
    AndroidAudioOutput audio;
    diag.audio_output = &audio;
    diag.save_directory = rt::home() + "/SaveGame";
    diag.widescreen = true; // Default to 16:9 widescreen

    auto start_time = std::chrono::steady_clock::now();
    diag.movie_clock_reset = [&start_time] { start_time = std::chrono::steady_clock::now(); };
    diag.movie_clock_read = [&start_time](bool& paused) {
        paused = false;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    };

    static std::uint64_t last_held = 0;
    diag.read_pad = [&controller] {
        SDL_PumpEvents();
        rt::PadSnapshot p{};

        // Dynamic controller re-detection if disconnected or initialized late
        if (!controller) {
            for (int i = 0; i < SDL_NumJoysticks(); ++i) {
                if (SDL_IsGameController(i)) {
                    controller = SDL_GameControllerOpen(i);
                    if (controller) {
                        LOGI("Dynamically connected GameController: %s", SDL_GameControllerName(controller));
                        s_active_controller = controller;
                        if (!s_active_haptic) {
                            SDL_Joystick* joy = SDL_GameControllerGetJoystick(s_active_controller);
                            if (joy) s_active_haptic = SDL_HapticOpenFromJoystick(joy);
                            if (!s_active_haptic && SDL_NumHaptics() > 0) s_active_haptic = SDL_HapticOpen(0);
                            if (s_active_haptic && SDL_HapticRumbleInit(s_active_haptic) < 0) {
                                SDL_HapticClose(s_active_haptic);
                                s_active_haptic = nullptr;
                            }
                        }
                        break;
                    }
                }
            }
        }

        std::uint64_t held = 0;

        if (controller) {
            p.connected = true;

            // Stick reading with Hall Effect deadzone (Law 8: 10% deadband standard)
            auto apply_deadzone = [](int16_t v, int16_t deadzone) -> int16_t {
                if (std::abs(v) < deadzone) return 0;
                return v;
            };
            constexpr int16_t kDeadzone10Pct = 3276;
            p.left.x = apply_deadzone(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX), kDeadzone10Pct);
            p.left.y = apply_deadzone(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY), kDeadzone10Pct);
            p.right.x = apply_deadzone(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX), kDeadzone10Pct);
            p.right.y = apply_deadzone(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY), kDeadzone10Pct);

            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A)) held |= rt::PadA;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B)) held |= rt::PadB;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X)) held |= rt::PadX;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y)) held |= rt::PadY;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) held |= rt::PadL;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) held |= rt::PadR;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START)) held |= rt::PadPlus;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK)) held |= rt::PadMinus;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP)) held |= rt::PadUp;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) held |= rt::PadDown;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) held |= rt::PadLeft;
            if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) held |= rt::PadRight;

            // Analog triggers with full pedal check fix (FixFullPedalChecks: 100% saturation)
            int16_t trig_l = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            int16_t trig_r = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            if (trig_l > 8000) held |= rt::PadZL;
            if (trig_r > 8000) held |= rt::PadZR;
        }

        // Keyboard & synthetic ADB keyevent fallback with event polling
        static std::uint64_t latched_keys = 0;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_KEYDOWN) {
                auto sc = ev.key.keysym.scancode;
                if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_SPACE) latched_keys |= rt::PadPlus;
                if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_BACKSPACE) latched_keys |= rt::PadMinus;
                if (sc == SDL_SCANCODE_Z || sc == SDL_SCANCODE_C) latched_keys |= rt::PadA;
                if (sc == SDL_SCANCODE_X || sc == SDL_SCANCODE_V) latched_keys |= rt::PadB;
                if (sc == SDL_SCANCODE_A) latched_keys |= rt::PadX;
                if (sc == SDL_SCANCODE_S) latched_keys |= rt::PadY;
                if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_W) latched_keys |= rt::PadUp;
                if (sc == SDL_SCANCODE_DOWN) latched_keys |= rt::PadDown;
                if (sc == SDL_SCANCODE_LEFT) latched_keys |= rt::PadLeft;
                if (sc == SDL_SCANCODE_RIGHT) latched_keys |= rt::PadRight;
                if (sc == SDL_SCANCODE_Q || sc == SDL_SCANCODE_LSHIFT) latched_keys |= rt::PadL;
                if (sc == SDL_SCANCODE_E || sc == SDL_SCANCODE_RSHIFT) latched_keys |= rt::PadR;
                LOGI("Drained SDL_KEYDOWN: scancode=%d latched=0x%llx", (int)sc, (unsigned long long)latched_keys);
            }
        }
        if (latched_keys) {
            p.connected = true;
            held |= latched_keys;
            latched_keys = 0;
        }

        p.held = held;
        p.down = held & ~last_held;
        p.up = ~held & last_held;
        if (p.down) {
            LOGI("Pad button down: 0x%llx", (unsigned long long)p.down);
        }
        last_held = held;
        return p;
    };

    // Load OR2006C2C.cache into the EXE image system
    std::string cache_path = rt::home() + "/" + outrun::platform::ExeImageCacheName;
    std::string cache_err;
    LOGI("Loading cache from: %s", cache_path.c_str());
    if (!outrun::platform::exe_image_load_cache(cache_path, &cache_err)) {
        LOGE("Failed to load OR2006C2C.cache: %s", cache_err.c_str());
        return 18;
    }
    LOGI("OR2006C2C.cache successfully loaded into memory!");

    // Apply complete emoose/OutRun2006Tweaks suite to game image
    outrun::enhancements::apply_outrun2006_tweaks();

    outrun::platform::NativeRuntimeContext context{};
    diag.runtime = &context;

    outrun::platform::RetailAssetStore retail{};
    std::string retail_error;
    std::string retail_root = rt::home();

    LOGI("Opening retail asset store at: %s", retail_root.c_str());
    if (!outrun::platform::retail_asset_store_open(retail, retail_root, &retail_error)) {
        LOGE("Failed to open retail asset store: %s", retail_error.c_str());
        std::exit(4);
    }

    rt::GameData data{};
    std::uint32_t fail_code = 0;
    const char* fail_phase = nullptr;

    LOGI("Loading OutRun 2006 game assets from: %s", retail_root.c_str());
    if (!rt::load_game_data(context, retail, retail_root, data,
        [](const std::string& line) {
            LOGI("AssetLoader: %s", line.c_str());
        }, fail_code, fail_phase, retail_error)) {
        LOGE("Failed to load game assets (phase %s, code %u): %s", fail_phase, fail_code, retail_error.c_str());
        std::exit(int(fail_code));
    }
    LOGI("Game assets loaded successfully!");

    outrun::ps5_runtime::Ps5Renderer renderer{};
    std::string renderer_error;
    if (!outrun::ps5_runtime::ps5_renderer_initialize(renderer, nullptr, renderer_error, &data.start_loading, nullptr, nullptr, nullptr)) {
        LOGE("Renderer initialization failed: %s", renderer_error.c_str());
        std::exit(5);
    }

    // Set 1080p widescreen Vulkan presentation
    outrun::ps5_runtime::ps5_renderer_set_video(renderer, target_w, target_h, true, 1, false);

    if (!rt::bind_frontend_timing(context.event_function36.frontend_sprites, data) ||
        !outrun::ps5_runtime::ps5_renderer_attach_frontend_sprites(renderer, context.event_function36.frontend_sprites)) {
        LOGE("Failed to attach frontend sprites!");
        std::exit(5);
    }

    if (auto* pc_device = outrun::ps5_runtime::ps5_renderer_pc_device(renderer)) {
        rt::attach_pc_scene(diag, context, retail, *pc_device);
        outrun::ps5_runtime::ps5_renderer_set_pc_sprites(renderer, true);
        outrun::ps5_runtime::ps5_renderer_set_pc_text(renderer, true);
    }

    AndroidPresenter presenter(renderer, diag, context);
    diag.presenter = &presenter;

    rt::RuntimeBinding binding{};
    if (!rt::bind_runtime(diag, context, binding)) {
        LOGE("Failed to bind runtime!");
        std::exit(6);
    }

    rt::FrontendSession fe{};
    const char* fe_phase = nullptr;
    std::string fe_error;
    if (!rt::open_frontend(diag, context, retail, &data.fonts, fe, fe_phase, fe_error)) {
        LOGE("Failed to open frontend: %s", fe_error.c_str());
        std::exit(7);
    }

    (void)outrun::ps5_runtime::ps5_renderer_set_frontend_visible(renderer, false);

    LOGI("Starting native OutRun 2006 runtime engine loop!");
    const auto rc = outrun::platform::run_native_runtime(context, binding.services);
    LOGI("Runtime loop completed with return code: %u (render_failed=%d audio_open=%d movie_err=%s music_err=%s)",
         rc, int(diag.render_failed), int(diag.audio_open), diag.movie_error.c_str(), diag.music_error.c_str());

    rt::close_frontend(diag, context, fe);
    outrun::ps5_runtime::ps5_renderer_shutdown(renderer);

    audio.close();
    diag.audio_output = nullptr;

    if (s_active_haptic) {
        SDL_HapticClose(s_active_haptic);
        s_active_haptic = nullptr;
    }
    if (controller) {
        SDL_GameControllerClose(controller);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::exit(0);
}
