#ifndef _ELECTRICAL_MEASUREMENT_CLUSTER_H_
#define _ELECTRICAL_MEASUREMENT_CLUSTER_H_

#include <stdint.h>
#include "hal/zigbee.h"
#include "base_components/energy_meter.h"

/**
 * Zigbee Electrical Measurement Cluster (0x0B04)
 *
 * Provides instantaneous electrical measurements:
 * - RMS Voltage (0x0505)
 * - RMS Current (0x0508)
 * - Active Power (0x050B)
 *
 * Plus calibration attributes for adjustment.
 *
 * Uses the generic energy_meter interface to support any energy monitoring IC.
 */

// Electrical Measurement cluster data
typedef struct {
    uint8_t endpoint;

    // Pointer to generic energy meter interface
    energy_meter_t *meter;

    // ZCL attribute values (updated from IC readings)
    uint32_t measurement_type;      // Bitmap of measurement capabilities
    uint16_t rms_voltage;           // Voltage in 0.1V units
    uint16_t rms_current;           // Current in mA
    int16_t active_power;           // Power in W

    // Multiplier/Divisor for ZCL formatting
    uint16_t ac_voltage_multiplier;
    uint16_t ac_voltage_divisor;
    uint16_t ac_current_multiplier;
    uint16_t ac_current_divisor;
    uint16_t ac_power_multiplier;
    uint16_t ac_power_divisor;

    uint32_t freq_cf;
    uint32_t freq_cf1;
    uint8_t sel_state;

    // HAL attribute storage
    hal_zigbee_attribute attr_infos[16];

    // Reporting state
    uint32_t last_report_time;
    uint16_t last_reported_voltage;
    uint16_t last_reported_current;
    int16_t last_reported_power;
} electrical_measurement_cluster_t;

/**
 * Initialize Electrical Measurement cluster with an energy meter
 * @param cluster Pointer to cluster structure
 * @param meter Pointer to initialized energy meter interface
 */
void electrical_measurement_cluster_init(
    electrical_measurement_cluster_t *cluster,
    energy_meter_t *meter);

/**
 * Add cluster to Zigbee endpoint
 * @param cluster Pointer to cluster structure
 * @param endpoint Pointer to HAL endpoint structure
 */
void electrical_measurement_cluster_add_to_endpoint(
    electrical_measurement_cluster_t *cluster,
    hal_zigbee_endpoint *endpoint);

/**
 * Update cluster with latest measurements from energy IC
 * Should be called periodically (e.g., every second)
 * @param cluster Pointer to cluster structure
 */
void electrical_measurement_cluster_update(electrical_measurement_cluster_t *cluster);

/**
 * Send attribute report for changed values
 * @param cluster Pointer to cluster structure
 */
void electrical_measurement_cluster_report(electrical_measurement_cluster_t *cluster);

#endif /* _ELECTRICAL_MEASUREMENT_CLUSTER_H_ */

