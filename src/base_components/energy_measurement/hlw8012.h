#ifndef _HLW8012_H_
#define _HLW8012_H_

#include <stdint.h>
#include "hal/gpio.h"
#include "base_components/energy_meter.h"
#include "hal/tasks.h"

/**
 * HLW8012 Energy Monitoring IC Driver (HLW8012 compatible)
 *
 * The HLW8012 is a pulse-based energy monitoring IC that measures:
 * - RMS Voltage (via CF1 pin when SEL is high)
 * - RMS Current (via CF1 pin when SEL is low)
 * - Active Power (via CF pin)
 * - Energy (accumulated from power pulses)
 *
 * Pins:
 * - CF: Power pulse output (frequency proportional to active power)
 * - CF1: Current/Voltage pulse output (selected by SEL pin)
 * - SEL: Select current (low) or voltage (high) measurement on CF1
 *
 * This driver implements the generic energy_meter interface.
 */

 // Using fixed-point arithmetic with 16-bit fractional part (scale factor 65536)
 #define HLW8012_FIXED_POINT_SCALE   65536    // 2^16

// Pre-calculated multipliers based on BL0937 reference voltage (1.218V) and coefficients
// I_coef = 0.001 (1mV/A), V_coef = 2351 (mV/V)
// power_multiplier = Vref^2 * V_coef / I_coef / 1721506 = 1.218^2 * 2351 / 0.001 / 1721506 ≈ 2.026
// voltage_multiplier = Vref * V_coef / 15397 = 1.218 * 2351 / 15397 ≈ 0.186
// current_multiplier = Vref / I_coef / 94638 = 1.218 / 0.001 / 94638 ≈ 0.01287
#define HLW8012_POWER_MULTIPLIER    132777   // 2.026 * 65536
#define HLW8012_VOLTAGE_MULTIPLIER  12190    // 0.186 * 65536  
#define HLW8012_CURRENT_MULTIPLIER  843      // 0.01287 * 65536

// SEL pin toggle interval (every N measurement cycles)
#define HLW8012_SEL_TOGGLE_CYCLE_INTERVAL  5

// Measurement timeout - if no pulses received in this time, value is 0
#define HLW8012_PULSE_TIMEOUT_MS        20000  // 20 seconds

// Sampling interval for pulse counting
#define HLW8012_SAMPLE_INTERVAL_MS  5000

// HLW8012 measurement data structure
typedef struct {
    uint32_t cf_pulse_count;                // Total CF pulses (from hardware counter)
    uint32_t cf_last_pulse_time;            // Last CF pulse timestamp (for timeout detection)
    uint32_t cf_total_pulse_count;          // CF pulse count at last sample

    uint32_t cf1_pulse_count;               // CF1 pulse count (from hardware counter)
    uint32_t cf1_last_pulse_time;           // Last CF1 pulse timestamp (for timeout detection)
    uint32_t cf1_total_pulse_count;         // CF1 pulse count at last sample

    uint32_t last_sample_time;              // Last sample timestamp

    // Tick-based pulse counting state
    uint32_t cf_tick_pulse_count;           // CF pulses accumulated during current interval
    uint32_t cf1_tick_pulse_count;          // CF1 pulses accumulated during current interval
    uint8_t cf_last_gpio_state;             // Previous CF GPIO state for edge detection
    uint8_t cf1_last_gpio_state;            // Previous CF1 GPIO state for edge detection

    // Converted values
    uint16_t voltage;                       // Voltage in 0.1V units (e.g., 2200 = 220.0V)
    uint16_t current;                       // Current in mA (e.g., 1500 = 1.5A)
    int16_t power;                          // Power in W (e.g., 330 = 330W)
    uint32_t energy;                        // Energy in Wh

    uint8_t sel_state;                      // Current SEL pin state (0=current, 1=voltage)
    uint8_t valid;                          // 1 if measurements are valid

    uint32_t freq_cf;
    uint32_t freq_cf1;
} hlw8012_data_t;

// HLW8012 device instance
typedef struct {
    hal_gpio_pin_t cf_pin;         // Power pulse pin
    hal_gpio_pin_t cf1_pin;        // Current/Voltage pulse pin
    hal_gpio_pin_t sel_pin;        // Select pin (low=current, high=voltage)

    hlw8012_data_t data;           // Measurement data
    hal_task_t update_task;        // Periodic task for measurement updates

    uint8_t cycle_count;           // Number of completed measurement cycles
    uint8_t initialized;           // Initialization flag

    // Embedded generic energy meter interface
    energy_meter_t meter;
} hlw8012_t;

/**
 * Initialize HLW8012 driver
 * @param dev Pointer to HLW8012 device structure
 * @param cf_pin CF (power) pulse pin
 * @param cf1_pin CF1 (current/voltage) pulse pin
 * @param sel_pin SEL pin for current/voltage selection
 * @return 0 on success, -1 on error
 */
int hlw8012_init(hlw8012_t *dev, hal_gpio_pin_t cf_pin, hal_gpio_pin_t cf1_pin, hal_gpio_pin_t sel_pin);

/**
 * Get current measurement data
 * @param dev Pointer to HLW8012 device structure
 * @return Pointer to measurement data structure
 */
hlw8012_data_t *hlw8012_get_data(hlw8012_t *dev);

/**
 * Reset energy counter
 * @param dev Pointer to HLW8012 device structure
 */
void hlw8012_reset_energy(hlw8012_t *dev);

/**
 * Get the generic energy meter interface for this HLW8012 device
 * @param dev Pointer to HLW8012 device structure
 * @return Pointer to embedded energy_meter_t structure
 */
energy_meter_t *hlw8012_as_energy_meter(hlw8012_t *dev);

/**
 * Tick function for non-blocking pulse counting
 * Call this on every main loop iteration to check GPIO states and count pulses
 * @param dev Pointer to HLW8012 device structure
 */
void hlw8012_tick(hlw8012_t *dev);

#endif /* _HLW8012_H_ */

