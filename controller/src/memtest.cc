#include <cstdint>
#include <cstddef>
#include <random>
#include <array>
#include <cstdio>
#include <memory>

#include "aps6404l.hpp"
#include "log.hpp"
#include "board.h"
#include "tasks.hpp"

// PSRAM driver instance (externally defined in main application)
extern psram::PSRAMDriver* g_psram_driver;

namespace memtest {

constexpr size_t TEST_SIZE = 1024 * 1024;  // 1MB test region
constexpr size_t BUFFER_SIZE = 4096;       // 4KB buffer for transfers
constexpr int NUM_ITERATIONS = 3;          // Number of test iterations

// Clock prescaler values to test (in MHz equivalent)
constexpr uint32_t PRESCALER_VALUES[] = {256, 128, 64, 32, 16, 8, 4, 2};
constexpr size_t NUM_PRESCALERS = sizeof(PRESCALER_VALUES) / sizeof(PRESCALER_VALUES[0]);

class MemTest {
public:
    MemTest() : gen_(rd_()), dist_(0, 255) {
        // Allocate DMA-aligned write buffer
        write_buffer_.reset(new psram::Buffer(BUFFER_SIZE, 32));
        // Allocate DMA-aligned read buffer
        read_buffer_.reset(new psram::Buffer(BUFFER_SIZE, 32));
    }

    bool run_tests() {
        os::info("Starting PSRAM memory test\n");
        os::info("Test region: %zu bytes, Iterations: %d\n", TEST_SIZE, NUM_ITERATIONS);

        bool all_passed = true;

        for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
            os::info("Iteration %d/%d\n", iter + 1, NUM_ITERATIONS);

            if (!test_prng_pattern(iter)) {
                os::error("PRNG pattern test failed on iteration %d\n", iter + 1);
                all_passed = false;
                break;
            }

            if (!test_walking_ones(iter)) {
                os::error("Walking ones test failed on iteration %d\n", iter + 1);
                all_passed = false;
                break;
            }

            if (!test_walking_zeros(iter)) {
                os::error("Walking zeros test failed on iteration %d\n", iter + 1);
                all_passed = false;
                break;
            }

            os::info("Iteration %d passed\n", iter + 1);
        }

        if (all_passed) {
            os::info("All PSRAM tests passed!\n");
        } else {
            os::error("PSRAM tests failed!\n");
        }

        return all_passed;
    }

    /**
     * @brief Find the fastest working clock prescaler
     * @return Pair of (prescaler, passed) - 0 if no prescaler works
     */
    std::pair<uint32_t, bool> find_fastest_prescaler() {
        os::info("Starting memory training to find optimal clock speed...\n");

        // Test from fastest (smallest prescaler) to slowest
        // Start with faster speeds and work down to find the fastest that works
        for (size_t i = 0; i < NUM_PRESCALERS; ++i) {
            uint32_t prescaler = PRESCALER_VALUES[i];
            os::info("Testing prescaler %u (QSPI clock ~%.1f MHz)... ", 
                     prescaler, (160.0 / prescaler));

            auto result = g_psram_driver->psram_set_clock_prescaler(prescaler);
            if (!result.has_value()) {
                os::error("Failed to set prescaler %u: %s\n", 
                          prescaler, result.error().message().c_str());
                continue;
            }

            // Reset PSRAM to ensure clean state with new clock
            result = g_psram_driver->psram_reset();
            if (!result.has_value()) {
                os::error("PSRAM reset failed after prescaler change: %s\n",
                          result.error().message().c_str());
                continue;
            }

            result = g_psram_driver->psram_enter_quad_mode();
            if (!result.has_value()) {
                os::error("Failed to enter quad mode: %s\n",
                          result.error().message().c_str());
                continue;
            }

            // Run a quick test with this prescaler
            if (run_quick_test()) {
                os::info("PASSED\n");
                // Found the fastest working prescaler
                return {prescaler, true};
            } else {
                os::info("FAILED\n");
            }
        }

        os::error("No working prescaler found!\n");
        return {0, false};
    }

private:
    std::random_device rd_;
    std::mt19937 gen_;
    std::uniform_int_distribution<int> dist_;

    std::unique_ptr<psram::Buffer> write_buffer_;
    std::unique_ptr<psram::Buffer> read_buffer_;

    /**
     * @brief Run a quick test (only PRNG pattern, 1 iteration)
     */
    bool run_quick_test() {
        return test_prng_pattern(0);
    }

    bool test_prng_pattern(int seed_offset) {
        os::info("Testing PRNG pattern...\n");

        // Use iteration number as seed for reproducibility
        uint32_t seed = 0xDEADBEEF + seed_offset * 0x12345678;
        gen_.seed(seed);

        size_t test_size = std::min(TEST_SIZE, write_buffer_->psram_size());
        size_t num_blocks = test_size / write_buffer_->psram_size();

        for (size_t block = 0; block < num_blocks; ++block) {
            // Fill write buffer with random data
            for (size_t i = 0; i < write_buffer_->psram_size(); ++i) {
                write_buffer_->psram_data()[i] = static_cast<uint8_t>(dist_(gen_));
            }

            uint32_t address = block * write_buffer_->psram_size();

            // Write to PSRAM
            auto result = g_psram_driver->psram_write(address, write_buffer_->psram_data(), write_buffer_->psram_size());
            if (!result.has_value()) {
                os::error("Write failed at address 0x%06X: %s\n", address, result.error().message().c_str());
                return false;
            }

            // Read back from PSRAM
            result = g_psram_driver->psram_read(address, read_buffer_->psram_data(), read_buffer_->psram_size());
            if (!result.has_value()) {
                os::error("Read failed at address 0x%06X: %s\n", address, result.error().message().c_str());
                return false;
            }

            // Verify data
            if (std::memcmp(write_buffer_->psram_data(), read_buffer_->psram_data(), write_buffer_->psram_size()) != 0) {
                os::error("Data mismatch at address 0x%06X\n", address);
                return false;
            }
        }

        os::info("PRNG pattern test passed\n");
        return true;
    }

    bool test_walking_ones(int ) {
        os::info("Testing walking ones pattern...\n");

        size_t test_size = std::min(TEST_SIZE, write_buffer_->psram_size());
        size_t num_blocks = test_size / write_buffer_->psram_size();

        for (size_t block = 0; block < num_blocks; ++block) {
            // Fill with walking ones pattern
            for (size_t i = 0; i < write_buffer_->psram_size(); ++i) {
                write_buffer_->psram_data()[i] = static_cast<uint8_t>(1 << ((i & 7)));
            }

            uint32_t address = block * write_buffer_->psram_size();

            // Write to PSRAM
            auto result = g_psram_driver->psram_write(address, write_buffer_->psram_data(), write_buffer_->psram_size());
            if (!result.has_value()) {
                os::error("Write failed at address 0x%06X: %s\n", address, result.error().message().c_str());
                return false;
            }

            // Read back from PSRAM
            result = g_psram_driver->psram_read(address, read_buffer_->psram_data(), read_buffer_->psram_size());
            if (!result.has_value()) {
                os::error("Read failed at address 0x%06X: %s\n", address, result.error().message().c_str());
                return false;
            }

            // Verify data
            if (std::memcmp(write_buffer_->psram_data(), read_buffer_->psram_data(), write_buffer_->psram_size()) != 0) {
                os::error("Data mismatch at address 0x%06X\n", address);
                return false;
            }
        }

        os::info("Walking ones test passed\n");
        return true;
    }

    bool test_walking_zeros(int ) {
        os::info("Testing walking zeros pattern...\n");

        size_t test_size = std::min(TEST_SIZE, write_buffer_->psram_size());
        size_t num_blocks = test_size / write_buffer_->psram_size();

        for (size_t block = 0; block < num_blocks; ++block) {
            // Fill with walking zeros pattern
            for (size_t i = 0; i < write_buffer_->psram_size(); ++i) {
                write_buffer_->psram_data()[i] = static_cast<uint8_t>(~(1 << ((i & 7))));
            }

            uint32_t address = block * write_buffer_->psram_size();

            // Write to PSRAM
            auto result = g_psram_driver->psram_write(address, write_buffer_->psram_data(), write_buffer_->psram_size());
            if (!result.has_value()) {
                os::error("Write failed at address 0x%06X: %s\n", address, result.error().message().c_str());
                return false;
            }

            // Read back from PSRAM
            result = g_psram_driver->psram_read(address, read_buffer_->psram_data(), read_buffer_->psram_size());
            if (!result.has_value()) {
                os::error("Read failed at address 0x%06X: %s\n", address, result.error().message().c_str());
                return false;
            }

            // Verify data
            if (std::memcmp(write_buffer_->psram_data(), read_buffer_->psram_data(), write_buffer_->psram_size()) != 0) {
                os::error("Data mismatch at address 0x%06X\n", address);
                return false;
            }
        }

        os::info("Walking zeros test passed\n");
        return true;
    }
};

// Global PSRAM driver pointer (must be set by application before running tests)
psram::PSRAMDriver* g_psram_driver = nullptr;

bool run() {
    if (!g_psram_driver) {
        os::error("PSRAM driver not initialized\n");
        return false;
    }

    MemTest tester;
    return tester.run_tests();
}

/**
 * @brief Memory training - find the fastest working QSPI clock speed
 * @return Prescaler value that passed testing, or 0 if no prescaler works
 */
uint32_t train() {
    if (!g_psram_driver) {
        os::error("PSRAM driver not initialized\n");
        return 0;
    }

    MemTest tester;
    auto [prescaler, passed] = tester.find_fastest_prescaler();

    if (passed) {
        os::info("Memory training complete: optimal prescaler = %u\n", prescaler);
        os::info("QSPI clock speed: ~%.1f MHz\n", (160.0 / prescaler));
    } else {
        os::error("Memory training failed - no prescaler works!\n");
    }

    return prescaler;
}

} // namespace memtest
