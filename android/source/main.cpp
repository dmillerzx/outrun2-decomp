#include <android/log.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>

#include "runtime/game_host.hpp"
#include "platform/pc_scene_renderer.hpp"
#include "platform/native_runtime.hpp"
#include "platform/retail_asset_store.hpp"
#include "setup_display.hpp"
#include "system/exe_image.hpp"
#include "system/files.hpp"
#include "system/setup_screen.hpp"
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

namespace rt = outrun::runtime;

namespace {

static std::mutex g_log_mutex;
static std::FILE* g_log_file = nullptr;
static std::string g_log_path;

static void write_log_line(int prio, const char* tag, const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    __android_log_print(prio, tag, "%s", buf);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) {
        std::fprintf(g_log_file, "[%s] %s\n", tag, buf);
        std::fflush(g_log_file);
    }
}

#define LOGI(...) write_log_line(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) write_log_line(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static void open_persistent_log(const std::string& ext_dir) {
    if (ext_dir.empty()) {
        return;
    }
    outrun::platform::make_directory(ext_dir);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_path = ext_dir + "/outrun2006.log";
    if (!g_log_file) {
        g_log_file = std::fopen(g_log_path.c_str(), "wb");
    }
}

static SDL_GameController* s_active_controller = nullptr;
static SDL_Haptic* s_active_haptic = nullptr;

static void trigger_android_haptics(float left_motor, float right_motor, bool force_stop = false) {
    static bool s_haptics_active = false;
    const float max_intensity = std::max(left_motor, right_motor);
    const bool want_active = (max_intensity > 0.01f) && !force_stop;
    if (!want_active && !s_haptics_active && !force_stop) {
        return;
    }
    s_haptics_active = want_active;

    const Uint16 left_u16 = want_active ? static_cast<Uint16>(std::clamp(int(left_motor * 65535.0f), 0, 65535)) : 0;
    const Uint16 right_u16 = want_active ? static_cast<Uint16>(std::clamp(int(right_motor * 65535.0f), 0, 65535)) : 0;

    if (s_active_controller) {
        SDL_GameControllerRumble(s_active_controller, left_u16, right_u16, want_active ? 50 : 0);
    }
    if (s_active_haptic) {
        if (want_active) {
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
            if (want_active && s_haptic_run_mid) {
                env->CallStaticVoidMethod(s_mgr_class, s_haptic_run_mid, 999999, max_intensity, 50);
            } else if (s_haptic_stop_mid) {
                env->CallStaticVoidMethod(s_mgr_class, s_haptic_stop_mid, 999999);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    static int s_log_haptic_interval = 0;
    if (want_active && max_intensity > 0.05f && ++s_log_haptic_interval % 60 == 1) {
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
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (g_log_file) {
            std::fprintf(g_log_file, "[OutRun2006-Decomp] %s\n", buf);
            std::fflush(g_log_file);
        }
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
        // and when active gameplay is not paused
        const bool paused = (context_.event_function36.title_pause_flag_95b214 != 0u ||
                             context_.event_function36.title_pause_flag_7d2614 != 0u);
        const bool in_race = (context_.mode_state.current == 16u || context_.mode_state.current == 18u) && !paused;
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
    [[maybe_unused]] rt::GameHost& host_;
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

static bool android_file_exists(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::fclose(f);
    return true;
}

static bool directory_has_exe(const std::string& dir) {
    for (const auto& name : outrun::platform::directory_names(dir)) {
        if (name.size() >= 4) {
            std::string tail = name.substr(name.size() - 4);
            for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (tail == ".exe") {
                return true;
            }
        }
    }
    return false;
}

static std::string read_saved_game_path(const std::string& ext_dir) {
    if (ext_dir.empty()) {
        return {};
    }
    std::FILE* f = std::fopen((ext_dir + "/game_path.txt").c_str(), "rb");
    if (!f) {
        return {};
    }
    char buf[1024]{};
    std::string out;
    if (std::fgets(buf, sizeof(buf), f)) {
        out = buf;
    }
    std::fclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

static std::vector<std::string> build_candidate_paths(const std::string& ext_dir) {
    std::vector<std::string> candidates;
    auto push_unique = [&](const std::string& p) {
        if (p.empty()) return;
        std::string clean = p;
        while (clean.size() > 1 && clean.back() == '/') clean.pop_back();
        if (std::find(candidates.begin(), candidates.end(), clean) == candidates.end()) {
            candidates.push_back(clean);
        }
    };

    push_unique(read_saved_game_path(ext_dir));
    const bool isolated_test = !ext_dir.empty() && android_file_exists(ext_dir + "/.isolated_test");
    if (!isolated_test) {
        if (DIR* dir = opendir("/storage")) {
            while (struct dirent* ent = readdir(dir)) {
                const std::string name(ent->d_name);
                if (name == "." || name == ".." || name == "emulated" || name == "self") {
                    continue;
                }
                push_unique("/storage/" + name + "/ROMs/ports/outrun2006");
            }
            closedir(dir);
        }
        push_unique("/storage/emulated/0/ROMs/ports/outrun2006");
        push_unique("/sdcard/ROMs/ports/outrun2006");
    }
    push_unique(ext_dir);
    if (!isolated_test) {
        push_unique("/sdcard/Android/data/org.outrun2.c2c/files");
    }
    return candidates;
}

static bool select_game_directory(const std::string& ext_dir, std::string& out_dir, std::vector<std::string>& checked_paths) {
    checked_paths = build_candidate_paths(ext_dir);
    for (const auto& path : checked_paths) {
        const bool has_cache = android_file_exists(path + "/" + outrun::platform::ExeImageCacheName);
        if (outrun::platform::retail_asset_store_is_game_root(path) && (has_cache || directory_has_exe(path))) {
            out_dir = path;
            return true;
        }
    }
    for (const auto& path : checked_paths) {
        if (outrun::platform::retail_asset_store_is_game_root(path)) {
            out_dir = path;
            return true;
        }
    }
    for (const auto& path : checked_paths) {
        if (android_file_exists(path + "/" + outrun::platform::ExeImageCacheName) || directory_has_exe(path)) {
            out_dir = path;
            return true;
        }
    }
    out_dir.clear();
    return false;
}

static bool android_has_all_files_access() {
    JNIEnv* env = reinterpret_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = reinterpret_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        return true;
    }
    jclass cls = env->GetObjectClass(activity);
    jmethodID mid = cls ? env->GetStaticMethodID(cls, "hasAllFilesAccess", "()Z") : nullptr;
    bool granted = true;
    if (mid) {
        granted = env->CallStaticBooleanMethod(cls, mid) == JNI_TRUE;
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return granted;
}

static std::string android_show_startup_prompt(const std::string& title, const std::string& message, bool show_grant_storage) {
    JNIEnv* env = reinterpret_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = reinterpret_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        return "EXIT";
    }
    jclass cls = env->GetObjectClass(activity);
    jmethodID mid = cls ? env->GetStaticMethodID(cls, "showStartupPrompt", "(Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/String;") : nullptr;
    std::string out = "EXIT";
    if (mid) {
        jstring jtitle = env->NewStringUTF(title.c_str());
        jstring jmsg = env->NewStringUTF(message.c_str());
        jstring jres = reinterpret_cast<jstring>(env->CallStaticObjectMethod(cls, mid, jtitle, jmsg, show_grant_storage ? JNI_TRUE : JNI_FALSE));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else if (jres) {
            const char* chars = env->GetStringUTFChars(jres, nullptr);
            if (chars) {
                out = chars;
                env->ReleaseStringUTFChars(jres, chars);
            }
            env->DeleteLocalRef(jres);
        }
        if (jtitle) env->DeleteLocalRef(jtitle);
        if (jmsg) env->DeleteLocalRef(jmsg);
    }
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return out;
}

} // namespace

extern "C" int SDL_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    const char* ext_cstr = SDL_AndroidGetExternalStoragePath();
    const std::string ext_dir = (ext_cstr && *ext_cstr) ? std::string(ext_cstr) : std::string("/sdcard/Android/data/org.outrun2.c2c/files");
    open_persistent_log(ext_dir);
    redirect_stdout_to_logcat();

    LOGI("==========================================");
    LOGI("OutRun 2006 Coast 2 Coast - Android Native");
    LOGI("==========================================");
    if (!g_log_path.empty()) {
        LOGI("Persistent log path: %s", g_log_path.c_str());
    }

    rt::product() = {"OutRun 2006 Coast 2 Coast", "outrun2006.log"};

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0) {
        LOGE("Failed to initialize SDL: %s", SDL_GetError());
        android_show_startup_prompt("Initialization Error", std::string("SDL initialization failed: ") + SDL_GetError(), false);
        return 1;
    }

    SDL_DisplayMode dm{};
    int target_w = 1920;
    int target_h = 1080;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
        target_w = std::min(1920, std::max(dm.w, dm.h));
        target_h = std::min(1080, std::min(dm.w, dm.h));
    }

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
        android_show_startup_prompt("Window Error", std::string("Failed to create Vulkan window: ") + SDL_GetError(), false);
        SDL_Quit();
        return 1;
    }

    int draw_w = target_w;
    int draw_h = target_h;
    SDL_Vulkan_GetDrawableSize(window, &draw_w, &draw_h);
    if (draw_w > 0 && draw_h > 0) {
        target_w = std::min(1920, draw_w);
        target_h = std::min(1080, draw_h);
    }
    LOGI("Created Vulkan fullscreen window (%dx%d)", target_w, target_h);
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

        // Keyboard, synthetic ADB keyevent, and controller hot-plug handling
        static std::uint64_t held_keys = 0;
        auto map_scancode = [](SDL_Scancode sc) -> std::uint64_t {
            if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_SPACE) return rt::PadPlus;
            if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_BACKSPACE) return rt::PadMinus;
            if (sc == SDL_SCANCODE_Z || sc == SDL_SCANCODE_C) return rt::PadA;
            if (sc == SDL_SCANCODE_X || sc == SDL_SCANCODE_V) return rt::PadB;
            if (sc == SDL_SCANCODE_A) return rt::PadX;
            if (sc == SDL_SCANCODE_S) return rt::PadY;
            if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_W) return rt::PadUp;
            if (sc == SDL_SCANCODE_DOWN) return rt::PadDown;
            if (sc == SDL_SCANCODE_LEFT) return rt::PadLeft;
            if (sc == SDL_SCANCODE_RIGHT) return rt::PadRight;
            if (sc == SDL_SCANCODE_Q || sc == SDL_SCANCODE_LSHIFT) return rt::PadL;
            if (sc == SDL_SCANCODE_E || sc == SDL_SCANCODE_RSHIFT) return rt::PadR;
            return 0;
        };
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_CONTROLLERDEVICEREMOVED && controller) {
                SDL_Joystick* joy = SDL_GameControllerGetJoystick(controller);
                if (joy && SDL_JoystickInstanceID(joy) == ev.cdevice.which) {
                    LOGI("GameController disconnected (instance=%d)", ev.cdevice.which);
                    trigger_android_haptics(0.0f, 0.0f, true);
                    if (s_active_haptic) {
                        SDL_HapticClose(s_active_haptic);
                        s_active_haptic = nullptr;
                    }
                    SDL_GameControllerClose(controller);
                    controller = nullptr;
                    s_active_controller = nullptr;
                }
            } else if (ev.type == SDL_KEYDOWN) {
                held_keys |= map_scancode(ev.key.keysym.scancode);
            } else if (ev.type == SDL_KEYUP) {
                held_keys &= ~map_scancode(ev.key.keysym.scancode);
            }
        }
        if (held_keys) {
            p.connected = true;
            held |= held_keys;
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

    // Enable persistent Vulkan pipeline cache in app-private internal storage
    if (!std::getenv("OR2_PS5_VK_CACHE")) {
        const char* internal_dir = SDL_AndroidGetInternalStoragePath();
        const std::string vk_cache_dir = (internal_dir && *internal_dir) ? std::string(internal_dir) : ext_dir;
        const std::string vk_cache_path = vk_cache_dir + "/vulkan-pipelines.cache";
        setenv("OR2_PS5_VK_CACHE", vk_cache_path.c_str(), 1);
        LOGI("Vulkan pipeline cache path: %s", vk_cache_path.c_str());
    }

    for (;;) {
        std::string retail_root;
        std::vector<std::string> checked_paths;
        const bool has_storage = android_has_all_files_access();
        if (!select_game_directory(ext_dir, retail_root, checked_paths)) {
            std::string msg = "No OutRun 2006 Coast 2 Coast (Steam PC) installation was found.\n\n";
            if (!has_storage) {
                msg += "All Files Access is currently disabled. Tap 'All Files Access' to allow scanning MicroSD and ROM folders, or tap 'Choose Folder' to select your OutRun 2006 folder.\n\n";
            } else {
                msg += "Copy your Steam PC OutRun 2006 folder (OR2006C2C.EXE + Cars, Stage, Media, Scripts, Sprite) to one of these paths or tap 'Choose Folder':\n";
            }
            for (const auto& p : checked_paths) {
                msg += "- " + p + "\n";
            }
            if (!g_log_path.empty()) {
                msg += "\nLog: " + g_log_path;
            }
            LOGE("No game folder found (has_all_files_access=%d)", int(has_storage));
            if (android_show_startup_prompt("Game Files Not Found", msg, !has_storage) != "RETRY") {
                break;
            }
            continue;
        }

        rt::home() = retail_root;
        diag.save_directory = retail_root + "/SaveGame";
        LOGI("Selected game root: %s", retail_root.c_str());

        bool retry_requested = false;
        try {
            // Prepare OR2006C2C.EXE -> OR2006C2C.cache on first launch (or load existing cache)
            {
                outrun::ps5_runtime::SetupDisplay setup_display;
                auto setup = setup_display.platform();
                setup.keep_running = [] {
                    SDL_PumpEvents();
                    return true;
                };
                std::string setup_err;
                const bool ready = outrun::platform::exe_setup_prepare(retail_root, ext_dir, setup, &setup_err);
                setup_display.close();
                if (!ready) {
                    LOGE("EXE setup failed in %s: %s", retail_root.c_str(), setup_err.c_str());
                    std::string msg = "Could not verify OR2006C2C.EXE from:\n" + retail_root + "\n\n" + setup_err +
                                      "\n\nPlace your Steam PC OR2006C2C.EXE (or generated OR2006C2C.cache) inside the game folder, or tap 'Choose Folder'.";
                    if (!g_log_path.empty()) msg += "\n\nLog: " + g_log_path;
                    if (android_show_startup_prompt("OR2006C2C.EXE Setup Error", msg, !has_storage) == "RETRY") {
                        retry_requested = true;
                        continue;
                    }
                    break;
                }
            }
            LOGI("EXE image prepared and loaded into memory!");

            // Apply complete emoose/OutRun2006Tweaks suite to game image
            outrun::enhancements::apply_outrun2006_tweaks();

            auto context_owner = std::make_unique<outrun::platform::NativeRuntimeContext>();
            auto& context = *context_owner;
            diag.runtime = &context;

            outrun::platform::RetailAssetStore retail{};
            std::string retail_error;
            LOGI("Opening retail asset store at: %s", retail_root.c_str());
            if (!outrun::platform::retail_asset_store_open(retail, retail_root, &retail_error)) {
                LOGE("Failed to open retail asset store: %s", retail_error.c_str());
                std::string msg = "Missing PC game asset folders in:\n" + retail_root + "\n\n" + retail_error +
                                  "\n\nEnsure Cars/, Scripts/, Stage/, and Sprite/ from your Steam OutRun 2006 copy are present.";
                if (!g_log_path.empty()) msg += "\n\nLog: " + g_log_path;
                if (android_show_startup_prompt("Game Assets Missing", msg, !has_storage) == "RETRY") {
                    retry_requested = true;
                    continue;
                }
                break;
            }

            auto data_owner = std::make_unique<rt::GameData>();
            auto& data = *data_owner;
            std::uint32_t fail_code = 0;
            const char* fail_phase = nullptr;
            LOGI("Loading OutRun 2006 game assets from: %s", retail_root.c_str());
            if (!rt::load_game_data(context, retail, retail_root, data,
                [](const std::string& line) {
                    LOGI("AssetLoader: %s", line.c_str());
                }, fail_code, fail_phase, retail_error)) {
                LOGE("Failed to load game assets (phase %s, code %u): %s",
                     fail_phase ? fail_phase : "?", fail_code, retail_error.c_str());
                std::string msg = "Asset load failed in:\n" + retail_root + "\n(" +
                                  (fail_phase ? std::string(fail_phase) : std::string("load")) + "): " + retail_error;
                if (!g_log_path.empty()) msg += "\n\nLog: " + g_log_path;
                if (android_show_startup_prompt("Asset Load Error", msg, !has_storage) == "RETRY") {
                    retry_requested = true;
                    continue;
                }
                break;
            }
            LOGI("Game assets loaded successfully!");

            outrun::ps5_runtime::Ps5Renderer renderer{};
            std::string renderer_error;
            if (!outrun::ps5_runtime::ps5_renderer_initialize(renderer, nullptr, renderer_error, &data.start_loading, nullptr, nullptr, nullptr)) {
                LOGE("Renderer initialization failed: %s", renderer_error.c_str());
                std::string msg = "Vulkan renderer initialization failed:\n" + renderer_error;
                if (!g_log_path.empty()) msg += "\n\nLog: " + g_log_path;
                if (android_show_startup_prompt("Vulkan Renderer Error", msg, false) == "RETRY") {
                    retry_requested = true;
                    continue;
                }
                break;
            }

            // Set widescreen Vulkan presentation
            outrun::ps5_runtime::ps5_renderer_set_video(renderer, target_w, target_h, true, 1, false);

            if (!rt::bind_frontend_timing(context.event_function36.frontend_sprites, data) ||
                !outrun::ps5_runtime::ps5_renderer_attach_frontend_sprites(renderer, context.event_function36.frontend_sprites)) {
                LOGE("Failed to attach frontend sprites!");
                outrun::ps5_runtime::ps5_renderer_shutdown(renderer);
                if (android_show_startup_prompt("Renderer Error", "Failed to attach frontend sprites.", false) == "RETRY") {
                    retry_requested = true;
                    continue;
                }
                break;
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
                outrun::ps5_runtime::ps5_renderer_shutdown(renderer);
                if (android_show_startup_prompt("Runtime Error", "Failed to bind native runtime services.", false) == "RETRY") {
                    retry_requested = true;
                    continue;
                }
                break;
            }

            rt::FrontendSession fe{};
            const char* fe_phase = nullptr;
            std::string fe_error;
            if (!rt::open_frontend(diag, context, retail, &data.fonts, fe, fe_phase, fe_error)) {
                LOGE("Failed to open frontend: %s", fe_error.c_str());
                outrun::ps5_runtime::ps5_renderer_shutdown(renderer);
                std::string msg = "Failed to open frontend (" + std::string(fe_phase ? fe_phase : "?") + "): " + fe_error;
                if (!g_log_path.empty()) msg += "\n\nLog: " + g_log_path;
                if (android_show_startup_prompt("Frontend Error", msg, false) == "RETRY") {
                    retry_requested = true;
                    continue;
                }
                break;
            }

            (void)outrun::ps5_runtime::ps5_renderer_set_frontend_visible(renderer, false);

            LOGI("Starting native OutRun 2006 runtime engine loop!");
            const auto rc = outrun::platform::run_native_runtime(context, binding.services);
            LOGI("Runtime loop completed with return code: %u (render_failed=%d audio_open=%d movie_err=%s music_err=%s)",
                 rc, int(diag.render_failed), int(diag.audio_open), diag.movie_error.c_str(), diag.music_error.c_str());

            rt::close_frontend(diag, context, fe);
            outrun::ps5_runtime::ps5_renderer_shutdown(renderer);
            break;
        } catch (const std::exception& e) {
            LOGE("Unhandled C++ exception during startup/runtime: %s", e.what());
            std::string msg = std::string("Unexpected error:\n") + e.what();
            if (!g_log_path.empty()) msg += "\n\nLog: " + g_log_path;
            if (android_show_startup_prompt("Runtime Exception", msg, false) == "RETRY") {
                retry_requested = true;
                continue;
            }
            break;
        } catch (...) {
            LOGE("Unhandled unknown exception during startup/runtime");
            std::string msg = "Unexpected fatal error during startup.";
            if (!g_log_path.empty()) msg += "\n\nLog: " + g_log_path;
            if (android_show_startup_prompt("Runtime Exception", msg, false) == "RETRY") {
                retry_requested = true;
                continue;
            }
            break;
        }
        if (!retry_requested) {
            break;
        }
    }

    trigger_android_haptics(0.0f, 0.0f, true);
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

    return 0;
}
