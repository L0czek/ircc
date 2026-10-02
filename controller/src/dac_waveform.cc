#include "dac_waveform.hpp"
#include <cmath>

// =============================================================================
// Constants
// =============================================================================

constexpr float PI = 3.14159265358979323846f;

// =============================================================================
// Voltage Conversion Functions
// =============================================================================

uint16_t voltage_to_dac_code(float voltage, float reference_voltage) {
    // Clamp voltage to valid range
    if (voltage < 0.0f) voltage = 0.0f;
    if (voltage > reference_voltage) voltage = reference_voltage;

    // Convert to DAC code (12-bit)
    return static_cast<uint16_t>((voltage / reference_voltage) * DAC_MAX_VALUE + 0.5f);
}

float dac_code_to_voltage(uint16_t dac_code, float reference_voltage) {
    // Clamp DAC code to valid range
    if (dac_code > DAC_MAX_VALUE) dac_code = DAC_MAX_VALUE;

    // Convert to voltage
    return (static_cast<float>(dac_code) / static_cast<float>(DAC_MAX_VALUE)) * reference_voltage;
}

// =============================================================================
// Waveform Generation Functions
// =============================================================================

size_t generate_sine_wave(uint16_t* buffer, size_t num_samples, float amplitude, float offset) {
    if (!buffer || num_samples == 0 || num_samples > MAX_WAVEFORM_SAMPLES) {
        return 0;
    }

    for (size_t i = 0; i < num_samples; ++i) {
        // Generate sine wave: offset + amplitude * sin(2*pi*i/num_samples)
        float sample = offset + amplitude * std::sinf(2.0f * PI * static_cast<float>(i) / static_cast<float>(num_samples));
        buffer[i] = voltage_to_dac_code(sample);
    }

    return num_samples;
}

size_t generate_square_wave(uint16_t* buffer, size_t num_samples, float amplitude, float offset) {
    if (!buffer || num_samples == 0 || num_samples > MAX_WAVEFORM_SAMPLES) {
        return 0;
    }

    // Calculate mid-point for square wave
    uint16_t high_level = voltage_to_dac_code(offset + amplitude);
    uint16_t low_level = voltage_to_dac_code(offset - amplitude);

    for (size_t i = 0; i < num_samples; ++i) {
        // Square wave: alternating high and low
        if (i < num_samples / 2) {
            buffer[i] = high_level;
        } else {
            buffer[i] = low_level;
        }
    }

    return num_samples;
}

size_t generate_triangle_wave(uint16_t* buffer, size_t num_samples, float amplitude, float offset) {
    if (!buffer || num_samples == 0 || num_samples > MAX_WAVEFORM_SAMPLES) {
        return 0;
    }

    uint16_t high_level = voltage_to_dac_code(offset + amplitude);
    uint16_t low_level = voltage_to_dac_code(offset - amplitude);
    uint16_t mid_level = voltage_to_dac_code(offset);

    for (size_t i = 0; i < num_samples; ++i) {
        // Triangle wave: ramp up then ramp down
        if (i < num_samples / 2) {
            // Ramp up
            float t = static_cast<float>(i) / static_cast<float>(num_samples / 2);
            buffer[i] = static_cast<uint16_t>(low_level + (high_level - low_level) * t + 0.5f);
        } else {
            // Ramp down
            float t = static_cast<float>(i - num_samples / 2) / static_cast<float>(num_samples / 2);
            buffer[i] = static_cast<uint16_t>(high_level - (high_level - low_level) * t + 0.5f);
        }
    }

    return num_samples;
}

size_t generate_sawtooth_wave(uint16_t* buffer, size_t num_samples, float amplitude, float offset) {
    if (!buffer || num_samples == 0 || num_samples > MAX_WAVEFORM_SAMPLES) {
        return 0;
    }

    uint16_t high_level = voltage_to_dac_code(offset + amplitude);
    uint16_t low_level = voltage_to_dac_code(offset - amplitude);

    for (size_t i = 0; i < num_samples; ++i) {
        // Sawtooth wave: linear ramp with quick reset
        float t = static_cast<float>(i) / static_cast<float>(num_samples);
        buffer[i] = static_cast<uint16_t>(low_level + (high_level - low_level) * t + 0.5f);
    }

    return num_samples;
}
