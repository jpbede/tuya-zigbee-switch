#ifndef _ENERGY_METER_H_
#define _ENERGY_METER_H_

#include <stdint.h>

/**
 * Generic Energy Meter Interface
 *
 * This provides a common abstraction layer for different energy monitoring ICs
 * (HLW8012, BL0937, BL0940, CSE7766, etc.) using a vtable pattern.
 *
 * Each IC driver embeds an energy_meter_t struct and implements the ops vtable,
 * allowing the Zigbee clusters to work with any energy meter without type-switching.
 */

// Forward declaration
typedef struct energy_meter energy_meter_t;

// Energy meter IC types
typedef enum {
    ENERGY_METER_NONE = 0,
    ENERGY_METER_HLW8012,
    ENERGY_METER_BL0937,
} energy_meter_type_t;

// Common measurement data structure
typedef struct {
    uint16_t voltage;       // Voltage in 0.1V units (e.g., 2200 = 220.0V)
    uint16_t current;       // Current in mA (e.g., 1500 = 1.5A)
    int16_t power;          // Power in W (e.g., 330 = 330W)
    uint32_t energy;        // Energy in Wh
    uint8_t valid;          // 1 if measurements are valid

    uint32_t freq_cf;
    uint32_t freq_cf1;
    uint8_t sel_state;
} energy_meter_data_t;

// Operations vtable for energy meter implementations
typedef struct {
    /**
     * Get current measurement data
     * @param ctx Pointer to IC-specific device structure
     * @param data Output structure to fill with measurements
     */
    void (*get_data)(void *ctx, energy_meter_data_t *data);

    /**
     * Reset energy counter
     * @param ctx Pointer to IC-specific device structure
     */
    void (*reset_energy)(void *ctx);

    /**
     * Tick handler for time-based updates (optional)
     * @param ctx Pointer to IC-specific device structure
     */
    void (*tick)(void *ctx);
} energy_meter_ops_t;

// Generic energy meter instance
struct energy_meter {
    const energy_meter_ops_t *ops;  // Pointer to operations vtable
    void *ctx;                       // Pointer to IC-specific device structure
    energy_meter_type_t type;        // IC type identifier
};

/**
 * Initialize an energy meter structure
 * @param meter Pointer to energy meter structure
 * @param ops Pointer to operations vtable
 * @param ctx Pointer to IC-specific context
 * @param type IC type identifier
 */
static inline void energy_meter_init(energy_meter_t *meter,
                                     const energy_meter_ops_t *ops,
                                     void *ctx,
                                     energy_meter_type_t type) {
    if (!meter) {
        return;
    }
    meter->ops = ops;
    meter->ctx = ctx;
    meter->type = type;
}

/**
 * Get current measurement data
 * @param meter Pointer to energy meter structure
 * @param data Output structure to fill with measurements
 */
static inline void energy_meter_get_data(energy_meter_t *meter,
                                         energy_meter_data_t *data) {
    if (meter && meter->ops && meter->ops->get_data && data) {
        meter->ops->get_data(meter->ctx, data);
    }
}

/**
 * Reset energy counter
 * @param meter Pointer to energy meter structure
 */
static inline void energy_meter_reset_energy(energy_meter_t *meter) {
    if (meter && meter->ops && meter->ops->reset_energy) {
        meter->ops->reset_energy(meter->ctx);
    }
}

/**
 * Tick handler for time-based updates
 * @param meter Pointer to energy meter structure
 */
static inline void energy_meter_tick(energy_meter_t *meter) {
    if (meter && meter->ops && meter->ops->tick) {
        meter->ops->tick(meter->ctx);
    }
}

/**
 * Check if energy meter is valid/initialized
 * @param meter Pointer to energy meter structure
 * @return 1 if valid, 0 otherwise
 */
static inline int energy_meter_is_valid(energy_meter_t *meter) {
    return meter && meter->ops && meter->ctx;
}

#endif /* _ENERGY_METER_H_ */

