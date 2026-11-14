#include "enhancements/vibration.hpp"
#include <algorithm>
#include <cmath>

namespace outrun::enhancements {

namespace {

// C2C Xbox sub_1149C0: evaluates road surface flags into vibration intensity
float eval_surface_vibration(std::uint32_t surface_flags, bool is_in_air, bool& is_rumble_strip) {
    if (surface_flags > 0x400) {
        if (surface_flags > 0x100000) {
            switch (surface_flags) {
            case 0x200000u: return 0.80f; // off-road grass
            case 0x400000u: return is_in_air ? 0.25f : 0.90f; // gravel/dirt
            case 0x800000u: return 0.50f;
            default: break;
            }
        } else {
            if (surface_flags == 0x100000) return 0.71f;
            if (surface_flags == 0x8000) return 0.40f;
            if (surface_flags == 0x2000 || surface_flags == 0x800 || surface_flags == 0x1000) return 0.35f;
        }
        return 0.31f;
    }
    if (surface_flags == 0x400) return 0.30f;
    if (surface_flags > 0x10) {
        if (surface_flags == 0x80) return 0.85f;
        if (surface_flags == 0x100) return 0.45f;
        if (surface_flags == 0x200) return 0.35f;
        return 0.31f;
    }
    if (surface_flags == 0x10) return 0.90f;
    switch (surface_flags) {
    case 1u: return 0.0f; // smooth asphalt
    case 2u: // curb / rumble strip
        is_rumble_strip = true;
        return 0.76f;
    case 4u: return 0.70f;
    case 8u: return 0.85f;
    default: return 0.31f;
    }
}

} // namespace

void update_car_vibration(const outrun::driving::Bytes& car, VibrationState& state, float strength) {
    static int impact_sustain = 0;
    static std::int32_t prev_gear = 0;
    static float prev_speed = 0.0f;

    if (car.size() < 0x1000) {
        impact_sustain = 0;
        prev_speed = 0.0f;
        prev_gear = 0;
        state.left_motor = 0.0f;
        state.right_motor = 0.0f;
        return;
    }

    const float speed = car.f32(0x1C4);

    float left = 0.0f;
    float right = 0.0f;

    // 1. Wheel Surface Contact & Road Rumble (when moving)
    if (speed > 0.001f) {
        bool is_rumble_strip = false;
        float max_surface = 0.0f;
        const bool is_in_air = (car.u32(0x4) & 0x4000) != 0; // car airborne flag

        for (int w = 0; w < 4; ++w) {
            const std::uint32_t wheel_surf = car.u32(0x24C + w * 4);
            if (wheel_surf != 0) {
                float s = eval_surface_vibration(wheel_surf, is_in_air, is_rumble_strip);
                max_surface = std::max(max_surface, s);
            }
        }
        // Also check global surface type if wheels did not hit special flags
        if (max_surface < 0.05f) {
            max_surface = eval_surface_vibration(car.u32(0x5C), is_in_air, is_rumble_strip);
        }

        // High frequency motor tracks road texture and bumps
        const float speed_factor = std::min(speed * 0.12f, 0.50f);
        right = max_surface * speed_factor;

        // Rumble strips / curbs give sharp tactile buzz and slight steer-kick
        if (is_rumble_strip) {
            right += (max_surface - 0.5f) * speed * 0.85f;
            left += 0.20f * speed;
        }

        // 2. Lateral Force / Drift & Tire Slip
        const float slip = std::abs(car.f32(0x1D0));
        const float lat_force = std::abs(car.f32(0x1D4));
        if (slip > 0.0015f) {
            left += speed * 0.25f;
            right += slip * 0.40f;
        }
        if (lat_force > 0.1f) {
            right += std::min(lat_force * 0.15f, 0.25f);
        }
    }

    // 3. Gear Shift Haptic Feedback
    const std::int32_t current_gear = car.i32(0x208);
    if (current_gear != prev_gear && speed > 0.005f) {
        prev_gear = current_gear;
        left += 0.45f;
        impact_sustain = 4; // sustain shift haptic for 4 frames
    }

    // 4. Vehicle Collision / Wall Impact / Rebound
    const float impact_fb = (car.size() > 0xbd4) ? car.f32(0xbd0) : 0.0f;
    const bool crash_seq = (car.size() > 0x2f4) && ((car.u32(0x2f0) & 3u) != 0u);
    const bool wall_impact = (car.size() > 4) && ((car.u32(4) & 0x80800000u) != 0u);
    const float speed_drop = prev_speed - speed;
    prev_speed = speed;

    if (impact_fb > 0.05f || crash_seq || wall_impact || (speed_drop > 0.08f && speed > 0.03f)) {
        left += std::max(0.60f, impact_fb);
        right += 0.40f;
        impact_sustain = 8; // sustain collision rumble for 8 frames (~130ms)
    }

    // 5. Engine RPM High-Frequency Buzz (at high revs / redline)
    const std::int16_t rpm = (car.size() > 0xd36) ? car.i16(0xD34) : 0;
    if (rpm > 5500) {
        const float rpm_ratio = std::clamp((rpm - 5500.0f) / 3500.0f, 0.0f, 1.0f);
        right += 0.08f * rpm_ratio;
    }

    // 6. Impact Sustain Decay
    if (speed <= 0.001f && impact_fb <= 0.01f && !crash_seq) {
        impact_sustain = 0;
    } else if (impact_sustain > 0) {
        --impact_sustain;
        left = std::max(left, 0.40f);
    }

    // 7. Clamp and Apply Global Strength
    const float mult = std::clamp(strength, 0.0f, 2.0f);
    state.left_motor = std::clamp(left * mult, 0.0f, 1.0f);
    state.right_motor = std::clamp(right * mult, 0.0f, 1.0f);
}

} // namespace outrun::enhancements
