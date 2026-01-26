#include "hlw8012.h"
#include "hal/timer.h"
#include "hal/printf_selector.h"
#include <string.h>

// Forward declarations for vtable functions
static void hlw8012_meter_get_data(void *ctx, energy_meter_data_t *data);
static void hlw8012_meter_reset_energy(void *ctx);
static void hlw8012_meter_tick(void *ctx);
void _update_measurement_handler(void *arg);
void _cycle_sel_pin(hlw8012_t *dev);

// Energy meter operations vtable for HLW8012
static const energy_meter_ops_t hlw8012_energy_meter_ops = {
    .get_data = hlw8012_meter_get_data,
    .reset_energy = hlw8012_meter_reset_energy,
    .tick = hlw8012_meter_tick,
};

/**
 * Convert pulse count over sample interval to frequency in mHz
 * @param pulse_count Number of pulses counted in the sample interval
 * @return Frequency in mHz
 */
static uint32_t pulses_to_frequency(uint32_t pulse_count) {
    return (uint32_t)((uint64_t)pulse_count * 1000000 / HLW8012_SAMPLE_INTERVAL_MS);
}

int hlw8012_init(hlw8012_t *dev, hal_gpio_pin_t cf_pin, hal_gpio_pin_t cf1_pin, hal_gpio_pin_t sel_pin) {
    if (!dev) {
        return -1;
    }

    memset(dev, 0, sizeof(hlw8012_t));
    dev->cf_pin = cf_pin;
    dev->cf1_pin = cf1_pin;
    dev->sel_pin = sel_pin;

    // Initialize CF pin as input
    // HLW8012 CF pulses are active high
    hal_gpio_init(cf_pin, 1, HAL_GPIO_PULL_NONE);

    // Initialize CF1 pin as input
    hal_gpio_init(cf1_pin, 1, HAL_GPIO_PULL_NONE);

    // Initialize SEL pin as output, start with voltage measurement (SEL high)
    hal_gpio_init(sel_pin, 0, HAL_GPIO_PULL_NONE);
    hal_gpio_set(sel_pin);
    dev->data.sel_state = 1;  // Measuring voltage

    dev->data.last_sample_time = hal_millis();

    // Initialize tick-based pulse counting state
    dev->data.cf_tick_pulse_count = 0;
    dev->data.cf1_tick_pulse_count = 0;
    dev->data.cf_last_gpio_state = hal_gpio_read(cf_pin);
    dev->data.cf1_last_gpio_state = hal_gpio_read(cf1_pin);

    dev->initialized = 1;

    dev->update_task.handler = _update_measurement_handler;
    dev->update_task.arg = dev;
    hal_tasks_init(&dev->update_task);

    // Initialize embedded energy meter interface
    energy_meter_init(&dev->meter, &hlw8012_energy_meter_ops, dev, ENERGY_METER_HLW8012);

    printf("HLW8012: Initialized on CF=%04x CF1=%04x SEL=%04x (using tick-based pulse counting)\r\n", cf_pin, cf1_pin, sel_pin);

    // Schedule first measurement update
    hal_tasks_schedule(&dev->update_task, HLW8012_SAMPLE_INTERVAL_MS);

    return 0;
}

void _update_measurement_handler(void *arg) {
    hlw8012_t *dev = (hlw8012_t *)arg;

    if (!dev || !dev->initialized) {
        return;
    }

    uint32_t now = hal_millis();

    // Use tick-accumulated pulse counts
    uint32_t cf_total_pulses = dev->data.cf_tick_pulse_count;
    uint32_t cf1_total_pulses = dev->data.cf1_tick_pulse_count;
    uint32_t cf_pulses = cf_total_pulses - dev->data.cf_total_pulse_count;
    uint32_t cf1_pulses = cf1_total_pulses - dev->data.cf1_total_pulse_count;

    // Update total pulse counts
    dev->data.cf_total_pulse_count = cf_total_pulses;
    dev->data.cf1_total_pulse_count = cf1_total_pulses;

    // Check for CF (power) timeout - no pulses received recently
    if ((dev->data.cf_last_pulse_time > 0 &&
        (now - dev->data.cf_last_pulse_time) > HLW8012_PULSE_TIMEOUT_MS)) {
        dev->data.power = 0;
        dev->data.freq_cf = 0;
    }

    // Check for CF1 (current/voltage) timeout
    if ((dev->data.cf1_last_pulse_time > 0 &&
        (now - dev->data.cf1_last_pulse_time) > HLW8012_PULSE_TIMEOUT_MS)) {
        if (dev->data.sel_state) {
            dev->data.voltage = 0;
        } else {
            dev->data.current = 0;
        }
        dev->data.freq_cf1 = 0;
    }

    uint32_t freq_cf_mhz = pulses_to_frequency(cf_pulses); // in mHz
    if (freq_cf_mhz <= 1) {
        // don't count single pulse as power
        freq_cf_mhz = 0;
    }
    dev->data.freq_cf = freq_cf_mhz;

    uint32_t freq_cf1_mhz = pulses_to_frequency(cf1_pulses); // in mHz
    if (freq_cf1_mhz <= 1) {
        // don't count single pulse as voltage/current
        freq_cf1_mhz = 0;
    }
    dev->data.freq_cf1 = freq_cf1_mhz;

    // Calculate power from CF frequency
    if (freq_cf_mhz != 0) {
        dev->data.power = (uint16_t)((freq_cf_mhz * HLW8012_POWER_MULTIPLIER) / HLW8012_FIXED_POINT_SCALE);

        // Accumulate energy (Wh = W * seconds / 3600) using integer arithmetic
        dev->data.energy += ((uint32_t)cf_pulses * HLW8012_POWER_MULTIPLIER) / (HLW8012_FIXED_POINT_SCALE * 3600);
    }

    // Calculate voltage or current from CF1 frequency
    // Only calculate after first cycle.
    // Appearently the first CF1 pulses after SEL change are unreliable.
    if (freq_cf1_mhz != 0 && dev->cycle_count != 0) {
        if (dev->data.sel_state) {
            dev->data.voltage = (uint16_t)((freq_cf1_mhz * HLW8012_VOLTAGE_MULTIPLIER) / HLW8012_FIXED_POINT_SCALE);
        } else {
            dev->data.current = (uint16_t)((freq_cf1_mhz * HLW8012_CURRENT_MULTIPLIER) / HLW8012_FIXED_POINT_SCALE);
        }
    }
    
    dev->data.valid = 1;
    dev->data.last_sample_time = now;
    dev->cycle_count++;

    // Cycle SEL pin if needed
    _cycle_sel_pin(dev);

    // Reschedule next measurement update
    hal_tasks_schedule(&dev->update_task, HLW8012_SAMPLE_INTERVAL_MS);
}

void _cycle_sel_pin(hlw8012_t *dev) {
    // Toggle SEL pin after defined number of cycles
    if (dev->cycle_count == HLW8012_SEL_TOGGLE_CYCLE_INTERVAL) {
        dev->data.cf1_last_pulse_time = 0;
        dev->data.cf1_total_pulse_count = 0;
        dev->data.cf1_pulse_count = 0;
        dev->data.cf1_tick_pulse_count = 0;

        // Toggle SEL state
        if (dev->data.sel_state) {
            hal_gpio_clear(dev->sel_pin);
            dev->data.sel_state = 0;  // Now measuring current
        } else {
            hal_gpio_set(dev->sel_pin);
            dev->data.sel_state = 1;  // Now measuring voltage
        }

        // Reset CF1 pulse counting after mode switch
        dev->cycle_count = 0;
    }
}

hlw8012_data_t *hlw8012_get_data(hlw8012_t *dev) {
    if (!dev) {
        return NULL;
    }
    return &dev->data;
}

void hlw8012_reset_energy(hlw8012_t *dev) {
    if (!dev) {
        return;
    }
    dev->data.energy = 0;
    
    // Reset hardware counters
    dev->data.cf_pulse_count = 0;
    dev->data.cf_total_pulse_count = 0;
    dev->data.cf_tick_pulse_count = 0;
    dev->data.cf1_pulse_count = 0;
    dev->data.cf1_total_pulse_count = 0;
    dev->data.cf1_tick_pulse_count = 0;
}

// Energy meter vtable wrapper implementations
static void hlw8012_meter_get_data(void *ctx, energy_meter_data_t *data) {
    hlw8012_t *dev = (hlw8012_t *)ctx;
    if (!dev || !data) {
        return;
    }
    data->voltage = dev->data.voltage;
    data->current = dev->data.current;
    data->power = dev->data.power;
    data->energy = dev->data.energy;

    data->freq_cf = dev->data.freq_cf;
    data->freq_cf1 = dev->data.freq_cf1;
    data->sel_state = dev->data.sel_state;

    data->valid = dev->data.valid;
}

static void hlw8012_meter_reset_energy(void *ctx) {
    hlw8012_reset_energy((hlw8012_t *)ctx);
}

static void hlw8012_meter_tick(void *ctx) {
    hlw8012_tick((hlw8012_t *)ctx);
}

energy_meter_t *hlw8012_as_energy_meter(hlw8012_t *dev) {
    if (!dev || !dev->initialized) {
        return NULL;
    }
    return &dev->meter;
}

void hlw8012_tick(hlw8012_t *dev) {
    if (!dev || !dev->initialized) {
        return;
    }

    uint32_t now = hal_millis();

    // Read current GPIO states
    uint8_t cf_state = hal_gpio_read(dev->cf_pin);
    uint8_t cf1_state = hal_gpio_read(dev->cf1_pin);

    if (dev->data.cf_last_gpio_state != cf_state) {
        dev->data.cf_tick_pulse_count++;
        dev->data.cf_last_pulse_time = now;
    }

    if (dev->data.cf1_last_gpio_state != cf1_state) {
        dev->data.cf1_tick_pulse_count++;
        dev->data.cf1_last_pulse_time = now;
    }

    // Update last GPIO states
    dev->data.cf_last_gpio_state = cf_state;
    dev->data.cf1_last_gpio_state = cf1_state;
}

