#include "metering_cluster.h"
#include "cluster_common.h"
#include "consts.h"
#include "device_config/nvm_items.h"
#include "hal/nvm.h"
#include "hal/timer.h"
#include "hal/printf_selector.h"
#include <string.h>

// NVM save interval (5 minutes)
#define NVM_SAVE_INTERVAL_MS        300000

// Report thresholds
#define ENERGY_REPORT_THRESHOLD_WH  10     // Report when 10 Wh accumulated
#define MIN_REPORT_INTERVAL_MS      10000  // Minimum 10 seconds between reports
#define MAX_REPORT_INTERVAL_MS      300000 // Maximum 5 minutes between reports

// Metering device types
#define METERING_DEVICE_TYPE_ELECTRIC   0x00

// Unit of measure values
#define UNIT_OF_MEASURE_KWH             0x00
#define UNIT_OF_MEASURE_KWH_BCD         0x01

// NVM storage structure for energy
typedef struct {
    uint64_t accumulated_energy_wh;
} metering_nv_data_t;

void metering_cluster_init(metering_cluster_t *cluster, energy_meter_t *meter) {
    if (!cluster || !meter) {
        return;
    }

    memset(cluster, 0, sizeof(metering_cluster_t));
    cluster->meter = meter;

    // Set default values
    cluster->status = 0x00;
    cluster->unit_of_measure = UNIT_OF_MEASURE_KWH;
    cluster->multiplier = 1;
    cluster->divisor = 1000;  // Energy in Wh, divisor=1000 gives kWh
    cluster->summation_formatting = 0x2B;  // 3 digits after decimal, 5 digits total
    cluster->metering_device_type = METERING_DEVICE_TYPE_ELECTRIC;
}

void metering_cluster_add_to_endpoint(metering_cluster_t *cluster,
                                      hal_zigbee_endpoint *endpoint) {
    if (!cluster || !endpoint) {
        return;
    }

    // Load energy from NVM
    metering_cluster_load_energy(cluster);

    // Setup attributes

    // CurrentSummationDelivered (0x0000) - uint48
    // ZCL uses 48-bit for this, but we'll use uint64 internally
    SETUP_ATTR(0, ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED,
               ZCL_DATA_TYPE_UINT48, ATTR_READONLY, cluster->current_summation_delivered);

    // Status (0x0200) - bitmap8
    SETUP_ATTR(1, ZCL_ATTR_METERING_STATUS, ZCL_DATA_TYPE_BITMAP8,
               ATTR_READONLY, cluster->status);

    // UnitOfMeasure (0x0300) - enum8
    SETUP_ATTR(2, ZCL_ATTR_METERING_UNIT_OF_MEASURE, ZCL_DATA_TYPE_ENUM8,
               ATTR_READONLY, cluster->unit_of_measure);

    // Multiplier (0x0301) - uint24
    SETUP_ATTR(3, ZCL_ATTR_METERING_MULTIPLIER, ZCL_DATA_TYPE_UINT24,
               ATTR_READONLY, cluster->multiplier);

    // Divisor (0x0302) - uint24
    printf("Metering: Setting up Divisor attribute with value %u\r\n", cluster->divisor);
    SETUP_ATTR(4, ZCL_ATTR_METERING_DIVISOR, ZCL_DATA_TYPE_UINT24,
               ATTR_READONLY, cluster->divisor);

    // SummationFormatting (0x0303) - bitmap8
    SETUP_ATTR(5, ZCL_ATTR_METERING_SUMMATION_FORMATTING, ZCL_DATA_TYPE_BITMAP8,
               ATTR_READONLY, cluster->summation_formatting);

    // MeteringDeviceType (0x0306) - bitmap8
    SETUP_ATTR(6, ZCL_ATTR_METERING_METERING_DEVICE_TYPE, ZCL_DATA_TYPE_BITMAP8,
               ATTR_READONLY, cluster->metering_device_type);

    int attr_count = 7;

    // Add cluster to endpoint
    endpoint->clusters[endpoint->cluster_count].cluster_id = ZCL_CLUSTER_METERING;
    endpoint->clusters[endpoint->cluster_count].attribute_count = attr_count;
    endpoint->clusters[endpoint->cluster_count].attributes = cluster->attr_infos;
    endpoint->clusters[endpoint->cluster_count].is_server = 1;
    endpoint->cluster_count++;

    printf("Metering: Added to endpoint %d, energy=%llu Wh\r\n",
           endpoint->endpoint, (unsigned long long)cluster->current_summation_delivered);
}

void metering_cluster_update(metering_cluster_t *cluster) {
    if (!cluster || !cluster->meter) {
        return;
    }

    uint32_t current_energy = 0;

    // Get data from energy meter
    energy_meter_data_t data;
    energy_meter_get_data(cluster->meter, &data);

    if (data.valid) {
        current_energy = data.energy;
    }

    printf("Metering: Current energy reading = %u, valid=%d\r\n", current_energy, data.valid);

    // Accumulate energy (handle wrap-around)
    if (current_energy >= cluster->last_energy_value) {
        uint32_t delta = current_energy - cluster->last_energy_value;
        cluster->current_summation_delivered += delta;
    }
    cluster->last_energy_value = current_energy;

    // Periodic NVM save
    uint32_t now = hal_millis();
    if (now - cluster->last_nvm_save_time >= NVM_SAVE_INTERVAL_MS) {
        //metering_cluster_save_energy(cluster);
        cluster->last_nvm_save_time = now;
    }
}

void metering_cluster_report(metering_cluster_t *cluster) {
    if (!cluster) {
        return;
    }

    uint32_t now = hal_millis();

    // Check minimum report interval
    if (now - cluster->last_report_time < MIN_REPORT_INTERVAL_MS) {
        return;
    }

    // Calculate energy change
    uint64_t energy_diff = 0;
    if (cluster->current_summation_delivered >= cluster->last_reported_energy) {
        energy_diff = cluster->current_summation_delivered - cluster->last_reported_energy;
    }

    uint8_t force_report = (now - cluster->last_report_time >= MAX_REPORT_INTERVAL_MS);

    // Report if threshold reached or max interval elapsed
    if (force_report || energy_diff >= ENERGY_REPORT_THRESHOLD_WH) {
        // Send 48-bit value (6 bytes, little-endian)
        uint8_t energy_bytes[6];
        energy_bytes[0] = (uint8_t)(cluster->current_summation_delivered & 0xFF);
        energy_bytes[1] = (uint8_t)((cluster->current_summation_delivered >> 8) & 0xFF);
        energy_bytes[2] = (uint8_t)((cluster->current_summation_delivered >> 16) & 0xFF);
        energy_bytes[3] = (uint8_t)((cluster->current_summation_delivered >> 24) & 0xFF);
        energy_bytes[4] = (uint8_t)((cluster->current_summation_delivered >> 32) & 0xFF);
        energy_bytes[5] = (uint8_t)((cluster->current_summation_delivered >> 40) & 0xFF);

        hal_zigbee_send_report_attr(cluster->endpoint, ZCL_CLUSTER_METERING,
                                    ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED,
                                    ZCL_DATA_TYPE_UINT48, energy_bytes, 6);

        cluster->last_reported_energy = cluster->current_summation_delivered;
        cluster->last_report_time = now;
    }
}

void metering_cluster_load_energy(metering_cluster_t *cluster) {
    if (!cluster) {
        return;
    }

    metering_nv_data_t nv_data;
    hal_nvm_status_t status = hal_nvm_read(NV_ITEM_ENERGY_ACCUMULATION(cluster->endpoint),
                                           sizeof(nv_data), (uint8_t *)&nv_data);

    if (status == HAL_NVM_SUCCESS) {
        cluster->current_summation_delivered = nv_data.accumulated_energy_wh;
        cluster->last_reported_energy = nv_data.accumulated_energy_wh;
        printf("Metering: Loaded energy %llu Wh from NVM\r\n",
               (unsigned long long)cluster->current_summation_delivered);
    } else {
        cluster->current_summation_delivered = 0;
        cluster->last_reported_energy = 0;
        printf("Metering: No energy in NVM, starting from 0\r\n");
    }
}

void metering_cluster_save_energy(metering_cluster_t *cluster) {
    if (!cluster) {
        return;
    }

    metering_nv_data_t nv_data = {
        .accumulated_energy_wh = cluster->current_summation_delivered
    };

    hal_nvm_write(NV_ITEM_ENERGY_ACCUMULATION(cluster->endpoint),
                  sizeof(nv_data), (uint8_t *)&nv_data);

    printf("Metering: Saved energy %llu Wh to NVM\r\n",
           (unsigned long long)cluster->current_summation_delivered);
}

void metering_cluster_reset_energy(metering_cluster_t *cluster) {
    if (!cluster) {
        return;
    }

    cluster->current_summation_delivered = 0;
    cluster->last_energy_value = 0;
    cluster->last_reported_energy = 0;

    // Reset energy meter counter
    if (cluster->meter) {
        energy_meter_reset_energy(cluster->meter);
    }

    // Save to NVM
    metering_cluster_save_energy(cluster);

    printf("Metering: Energy counter reset\r\n");
}

