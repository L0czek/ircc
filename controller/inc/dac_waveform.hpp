#ifndef __DAC_WAVEFORM_HPP__
#define __DAC_WAVEFORM_HPP__

#include <cstdint>
#include <cstddef>

// =============================================================================
// DAC Waveform Types (avoid conflict with nanopb-generated WaveformType)
// =============================================================================

enum class DACWaveformType : uint8_t {
    SINE = 0,
    SQUARE = 1,
    TRIANGLE = 2,
    SAWTOOTH = 3,
    CUSTOM = 4
};

// =============================================================================
// Constants
// =============================================================================

constexpr size_t MAX_WAVEFORM_SAMPLES = 256;
constexpr float DEFAULT_DAC_REFERENCE_VOLTAGE = 3.3f;
constexpr uint16_t DAC_MAX_VALUE = 4095;  // 12-bit DAC

// =============================================================================
// Waveform Generation Functions
// =============================================================================

/**
 * @brief Generate sine wave samples
 * @param buffer Buffer to store samples (must be at least num_samples)
 * @param num_samples Number of samples to generate (max MAX_WAVEFORM_SAMPLES)
 * @param amplitude Amplitude in volts (0-3.3V)
 * @param offset DC offset in volts (default 1.65V for center)
 * @return Number of samples generated
 */
size_t generate_sine_wave(uint16_t* buffer, size_t num_samples, float amplitude, float offset = 1.65f);

/**
 * @brief Generate square wave samples
 * @param buffer Buffer to store samples (must be at least num_samples)
 * @param num_samples Number of samples to generate (max MAX_WAVEFORM_SAMPLES)
 * @param amplitude Amplitude in volts (0-3.3V)
 * @param offset DC offset in volts (default 1.65V for center)
 * @return Number of samples generated
 */
size_t generate_square_wave(uint16_t* buffer, size_t num_samples, float amplitude, float offset = 1.65f);

/**
 * @brief Generate triangle wave samples
 * @param buffer Buffer to store samples (must be at least num_samples)
 * @param num_samples Number of samples to generate (max MAX_WAVEFORM_SAMPLES)
 * @param amplitude Amplitude in volts (0-3.3V)
 * @param offset DC offset in volts (default 1.65V for center)
 * @return Number of samples generated
 */
size_t generate_triangle_wave(uint16_t* buffer, size_t num_samples, float amplitude, float offset = 1.65f);

/**
 * @brief Generate sawtooth wave samples
 * @param buffer Buffer to store samples (must be at least num_samples)
 * @param num_samples Number of samples to generate (max MAX_WAVEFORM_SAMPLES)
 * @param amplitude Amplitude in volts (0-3.3V)
 * @param offset DC offset in volts (default 1.65V for center)
 * @return Number of samples generated
 */
size_t generate_sawtooth_wave(uint16_t* buffer, size_t num_samples, float amplitude, float offset = 1.65f);

/**
 * @brief Convert voltage to DAC code
 * @param voltage Voltage in volts (0-3.3V)
 * @param reference_voltage Reference voltage in volts (default 3.3V)
 * @return DAC code (0-4095)
 */
uint16_t voltage_to_dac_code(float voltage, float reference_voltage = DEFAULT_DAC_REFERENCE_VOLTAGE);

/**
 * @brief Convert DAC code to voltage
 * @param dac_code DAC code (0-4095)
 * @param reference_voltage Reference voltage in volts (default 3.3V)
 * @return Voltage in volts
 */
float dac_code_to_voltage(uint16_t dac_code, float reference_voltage = DEFAULT_DAC_REFERENCE_VOLTAGE);

#endif
