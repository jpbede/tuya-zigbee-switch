#ifndef _METERING_CLUSTER_H_
#define _METERING_CLUSTER_H_

#include <stdint.h>
#include "hal/zigbee.h"
#include "base_components/energy_meter.h"

/**
 * Zigbee Metering Cluster (0x0702)
 *
 * Provides accumulated energy measurement:
 * - CurrentSummationDelivered (0x0000) - Total energy in Wh
 *
 * Energy is accumulated from the connected energy IC and
 * persisted to NVM periodically.
 *
 * Uses the generic energy_meter interface to support any energy monitoring IC.
 */

// Metering cluster data
typedef struct {
    uint8_t endpoint;

    // Pointer to generic energy meter interface
    energy_meter_t *meter;

    // ZCL attribute values
    uint64_t current_summation_delivered;  // Energy in Wh (48-bit in ZCL)
    uint8_t status;                         // Metering status
    uint8_t unit_of_measure;               // 0x00 = kWh
    uint32_t multiplier;                   // Multiplier for summation
    uint32_t divisor;                      // Divisor for summation
    uint8_t summation_formatting;          // Format bitmap
    uint8_t metering_device_type;          // 0x00 = Electric metering

    // HAL attribute storage
    hal_zigbee_attribute attr_infos[8];

    // Energy accumulation state
    uint32_t last_energy_value;            // Last energy reading from IC
    uint32_t last_nvm_save_time;           // Last time energy was saved to NVM
    uint32_t last_report_time;             // Last time report was sent
    uint64_t last_reported_energy;         // Last reported energy value
} metering_cluster_t;

/**
 * Initialize Metering cluster with an energy meter
 * @param cluster Pointer to cluster structure
 * @param meter Pointer to initialized energy meter interface
 */
void metering_cluster_init(metering_cluster_t *cluster, energy_meter_t *meter);

/**
 * Add cluster to Zigbee endpoint
 * @param cluster Pointer to cluster structure
 * @param endpoint Pointer to HAL endpoint structure
 */
void metering_cluster_add_to_endpoint(metering_cluster_t *cluster,
                                      hal_zigbee_endpoint *endpoint);

/**
 * Update cluster with latest energy from IC
 * Should be called periodically (e.g., every second)
 * @param cluster Pointer to cluster structure
 */
void metering_cluster_update(metering_cluster_t *cluster);

/**
 * Send attribute report for energy
 * @param cluster Pointer to cluster structure
 */
void metering_cluster_report(metering_cluster_t *cluster);

/**
 * Load accumulated energy from NVM
 * @param cluster Pointer to cluster structure
 */
void metering_cluster_load_energy(metering_cluster_t *cluster);

/**
 * Save accumulated energy to NVM
 * @param cluster Pointer to cluster structure
 */
void metering_cluster_save_energy(metering_cluster_t *cluster);

/**
 * Reset energy counter
 * @param cluster Pointer to cluster structure
 */
void metering_cluster_reset_energy(metering_cluster_t *cluster);

#endif /* _METERING_CLUSTER_H_ */

