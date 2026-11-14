#pragma once
#include <cstdint>
#include <cstddef>
#include "driving/pc_driving.hpp"

namespace outrun::enhancements {

struct VibrationState {
    float left_motor{0.0f};  // 0.0f to 1.0f (heavy low-frequency rumble: collision, gear shift, rough road)
    float right_motor{0.0f}; // 0.0f to 1.0f (light high-frequency rumble: engine RPM, road vibration, tire slip)
};

// Computes force feedback / vibration values from active player car work buffer (EVWORK_CAR)
// Based on C2C Xbox CalcVibrationValues (0x114C60)
void update_car_vibration(const outrun::driving::Bytes& car, VibrationState& state, float strength = 1.0f);

} // namespace outrun::enhancements
