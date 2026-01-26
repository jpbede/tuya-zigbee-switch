#include "electrical_measurement_cluster.h"
#include "cluster_common.h"
#include "consts.h"
#include "hal/timer.h"
#include "hal/printf_selector.h"
#include <string.h>

// Reporting thresholds
#define VOLTAGE_REPORT_THRESHOLD    5    // 0.5V change triggers report
#define CURRENT_REPORT_THRESHOLD    50   // 50mA change triggers report
#define POWER_REPORT_THRESHOLD      5    // 5W change triggers report
#define MIN_REPORT_INTERVAL_MS      1000 // Minimum 1 second between reports
#define MAX_REPORT_INTERVAL_MS      300000 // Maximum 5 minutes between reports

// Measurement type bitmap values
#define MEAS_TYPE_AC_ACTIVE         (1 << 0)
#define MEAS_TYPE_AC_REACTIVE       (1 << 1)
#define MEAS_TYPE_AC_APPARENT       (1 << 2)
#define MEAS_TYPE_PHASE_A           (1 << 3)

void electrical_measurement_cluster_init(
    electrical_measurement_cluster_t *cluster,
    energy_meter_t *meter) {

    if (!cluster || !meter) {
        return;
    }

    memset(cluster, 0, sizeof(electrical_measurement_cluster_t));
    cluster->meter = meter;

    // Set default values
    cluster->measurement_type = MEAS_TYPE_AC_ACTIVE | MEAS_TYPE_PHASE_A;
    cluster->ac_voltage_multiplier = 1;
    cluster->ac_voltage_divisor = 100;     // Voltage in 0.1V, divisor=10 gives V
    cluster->ac_current_multiplier = 1;
    cluster->ac_current_divisor = 1000;   // Current in mA, divisor=1000 gives A
    cluster->ac_power_multiplier = 1;
    cluster->ac_power_divisor = 1;        // Power in W
}

void electrical_measurement_cluster_add_to_endpoint(
    electrical_measurement_cluster_t *cluster,
    hal_zigbee_endpoint *endpoint) {

    if (!cluster || !endpoint) {
        return;
    }

    // Measurement Type (0x0000) - bitmap32
    SETUP_ATTR(0, ZCL_ATTR_ELEC_MEAS_MEASUREMENT_TYPE, ZCL_DATA_TYPE_BITMAP32,
               ATTR_READONLY, cluster->measurement_type);

    // RMS Voltage (0x0505) - uint16
    SETUP_ATTR(1, ZCL_ATTR_ELEC_MEAS_RMS_VOLTAGE, ZCL_DATA_TYPE_UINT16,
               ATTR_READONLY, cluster->rms_voltage);

    // RMS Current (0x0508) - uint16
    SETUP_ATTR(2, ZCL_ATTR_ELEC_MEAS_RMS_CURRENT, ZCL_DATA_TYPE_UINT16,
               ATTR_READONLY, cluster->rms_current);

    // Active Power (0x050B) - int16
    SETUP_ATTR(3, ZCL_ATTR_ELEC_MEAS_ACTIVE_POWER, ZCL_DATA_TYPE_INT16,
               ATTR_READONLY, cluster->active_power);

    // AC Voltage Multiplier (0x0600) - uint16
    SETUP_ATTR(4, ZCL_ATTR_ELEC_MEAS_AC_VOLTAGE_MULTIPLIER, ZCL_DATA_TYPE_UINT16,
               ATTR_READONLY, cluster->ac_voltage_multiplier);

    // AC Voltage Divisor (0x0601) - uint16
    SETUP_ATTR(5, ZCL_ATTR_ELEC_MEAS_AC_VOLTAGE_DIVISOR, ZCL_DATA_TYPE_UINT16,
               ATTR_READONLY, cluster->ac_voltage_divisor);

    // AC Current Multiplier (0x0602) - uint16
    SETUP_ATTR(6, ZCL_ATTR_ELEC_MEAS_AC_CURRENT_MULTIPLIER, ZCL_DATA_TYPE_UINT16,
               ATTR_READONLY, cluster->ac_current_multiplier);

    // AC Current Divisor (0x0603) - uint16
    SETUP_ATTR(7, ZCL_ATTR_ELEC_MEAS_AC_CURRENT_DIVISOR, ZCL_DATA_TYPE_UINT16,
               ATTR_READONLY, cluster->ac_current_divisor);

    // AC Power Multiplier (0x0604) - uint16
    SETUP_ATTR(8, ZCL_ATTR_ELEC_MEAS_AC_POWER_MULTIPLIER, ZCL_DATA_TYPE_UINT16,
               ATTR_READONLY, cluster->ac_power_multiplier);

    // AC Power Divisor (0x0605) - uint16
    SETUP_ATTR(9, ZCL_ATTR_ELEC_MEAS_AC_POWER_DIVISOR, ZCL_DATA_TYPE_UINT16,
               ATTR_READONLY, cluster->ac_power_divisor);

    SETUP_ATTR(10, ZCL_ATTR_ELEC_MEAS_CUST_FREQUENCY_CF, ZCL_DATA_TYPE_UINT32,
               ATTR_READONLY, cluster->freq_cf);

    SETUP_ATTR(11, ZCL_ATTR_ELEC_MEAS_CUST_FREQUENCY_CF1, ZCL_DATA_TYPE_UINT32,
               ATTR_READONLY, cluster->freq_cf1);

    SETUP_ATTR(12, ZCL_ATTR_ELEC_MEAS_CUST_FREQUENCY_SEL_STATE, ZCL_DATA_TYPE_UINT8,
               ATTR_READONLY, cluster->sel_state);

    int attr_count = 13;

    // Add cluster to endpoint
    endpoint->clusters[endpoint->cluster_count].cluster_id = ZCL_CLUSTER_ELECTRICAL_MEASUREMENT;
    endpoint->clusters[endpoint->cluster_count].attribute_count = attr_count;
    endpoint->clusters[endpoint->cluster_count].attributes = cluster->attr_infos;
    endpoint->clusters[endpoint->cluster_count].is_server = 1;
    endpoint->cluster_count++;
}

void electrical_measurement_cluster_update(electrical_measurement_cluster_t *cluster) {
    if (!cluster || !cluster->meter) {
        return;
    }

    // Get data from energy meter
    energy_meter_data_t data;
    energy_meter_get_data(cluster->meter, &data);

    if (data.valid) {
        cluster->rms_voltage = data.voltage;
        cluster->rms_current = data.current;
        cluster->active_power = data.power;
        cluster->freq_cf = data.freq_cf;
        cluster->freq_cf1 = data.freq_cf1;
        cluster->sel_state = data.sel_state;
    }
}

void electrical_measurement_cluster_report(electrical_measurement_cluster_t *cluster) {
    if (!cluster) {
        return;
    }

    uint32_t now = hal_millis();

    // Check minimum report interval
    if (now - cluster->last_report_time < MIN_REPORT_INTERVAL_MS) {
        return;
    }

    // Check if any value changed significantly or max interval reached
    int16_t voltage_diff = (int16_t)cluster->rms_voltage - (int16_t)cluster->last_reported_voltage;
    int16_t current_diff = (int16_t)cluster->rms_current - (int16_t)cluster->last_reported_current;
    int16_t power_diff = cluster->active_power - cluster->last_reported_power;

    if (voltage_diff < 0) voltage_diff = -voltage_diff;
    if (current_diff < 0) current_diff = -current_diff;
    if (power_diff < 0) power_diff = -power_diff;

    uint8_t force_report = (now - cluster->last_report_time >= MAX_REPORT_INTERVAL_MS);

    // Report voltage if changed
    if (force_report || voltage_diff >= VOLTAGE_REPORT_THRESHOLD) {
        hal_zigbee_send_report_attr(cluster->endpoint, ZCL_CLUSTER_ELECTRICAL_MEASUREMENT,
                                    ZCL_ATTR_ELEC_MEAS_RMS_VOLTAGE, ZCL_DATA_TYPE_UINT16,
                                    &cluster->rms_voltage, sizeof(cluster->rms_voltage));
        cluster->last_reported_voltage = cluster->rms_voltage;
    }

    // Report current if changed
    if (force_report || current_diff >= CURRENT_REPORT_THRESHOLD) {
        hal_zigbee_send_report_attr(cluster->endpoint, ZCL_CLUSTER_ELECTRICAL_MEASUREMENT,
                                    ZCL_ATTR_ELEC_MEAS_RMS_CURRENT, ZCL_DATA_TYPE_UINT16,
                                    &cluster->rms_current, sizeof(cluster->rms_current));
        cluster->last_reported_current = cluster->rms_current;
    }

    // Report power if changed
    if (force_report || power_diff >= POWER_REPORT_THRESHOLD) {
        hal_zigbee_send_report_attr(cluster->endpoint, ZCL_CLUSTER_ELECTRICAL_MEASUREMENT,
                                    ZCL_ATTR_ELEC_MEAS_ACTIVE_POWER, ZCL_DATA_TYPE_INT16,
                                    &cluster->active_power, sizeof(cluster->active_power));
        cluster->last_reported_power = cluster->active_power;
    }

    cluster->last_report_time = now;
}

