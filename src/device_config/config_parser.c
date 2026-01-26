#include "hal/gpio.h"
#include "hal/printf_selector.h"
#include "hal/zigbee.h"
#include "zigbee/basic_cluster.h"
#include "zigbee/consts.h"
#include "zigbee/group_cluster.h"
#include "zigbee/relay_cluster.h"
#include "zigbee/switch_cluster.h"
#include "zigbee/cover_cluster.h"
#include "zigbee/electrical_measurement_cluster.h"
#include "zigbee/metering_cluster.h"

#include <stdint.h>
#include <string.h>

#include "base_components/led.h"
#include "base_components/network_indicator.h"
#include "base_components/energy_measurement/hlw8012.h"
#include "config_nv.h"
#include "device_config/reset.h"
#include "hal/system.h"
#include "hal/zigbee.h"
#include "hal/zigbee_ota.h"

// Forward declarations
void periferals_init(void);
void energy_monitoring_init(void);
void _energy_monitoring_task_handler(void *arg);

// extern ota_preamble_t baseEndpoint_otaInfo;

network_indicator_t network_indicator = {
    .leds = {NULL, NULL, NULL, NULL},
    .has_dedicated_led = 0,
    .manual_state_when_connected = 1,
};

led_t leds[5];
uint8_t leds_cnt = 0;

button_t buttons[5];
uint8_t buttons_cnt = 0;

relay_t relays[10]; // 4 relay endpoints + 3 cover endpoints
uint8_t relays_cnt = 0;

zigbee_basic_cluster basic_cluster = {
    .deviceEnable = 1,
};

zigbee_group_cluster group_cluster = {};

zigbee_switch_cluster switch_clusters[4];
uint8_t switch_clusters_cnt = 0;

zigbee_relay_cluster relay_clusters[4];
uint8_t relay_clusters_cnt = 0;

zigbee_cover_cluster cover_clusters[3];
uint8_t cover_clusters_cnt = 0;

// Energy monitoring
hlw8012_t hlw8012_device;
energy_meter_t *energy_meter = NULL;
electrical_measurement_cluster_t elec_meas_cluster;
metering_cluster_t metering_cluster;
uint8_t energy_monitoring_enabled = 0;
uint8_t energy_monitoring_endpoint = 0;
hal_task_t energy_monitoring_task;

hal_zigbee_cluster clusters[32];
hal_zigbee_endpoint endpoints[10];

uint8_t allow_simultaneous_latching_pulses = 0;

uint32_t parse_int(const char *s);
char *seek_until(char *cursor, char needle);
char *extract_next_entry(char **cursor);

void on_reset_clicked(void *_) { hal_factory_reset(); }

void parse_config() {
  device_config_read_from_nv();
  char *cursor = device_config_str.data;

  const char *zb_manufacturer = extract_next_entry(&cursor);

  basic_cluster.manuName[0] = strlen(zb_manufacturer);
  if (basic_cluster.manuName[0] > 31) {
    printf("Manufacturer too big\r\n");
    reset_all();
  }
  memcpy(basic_cluster.manuName + 1, zb_manufacturer,
         basic_cluster.manuName[0]);

  const char *zb_model = extract_next_entry(&cursor);
  basic_cluster.modelId[0] = strlen(zb_model);
  if (basic_cluster.modelId[0] > 31) {
    printf("Model too big\r\n");
    reset_all();
  }
  memcpy(basic_cluster.modelId + 1, zb_model, basic_cluster.modelId[0]);

  bool has_dedicated_status_led = false;
  char *entry;
  for (entry = extract_next_entry(&cursor); *entry != '\0';
       entry = extract_next_entry(&cursor)) {
    if (entry[0] == 'S' && entry[1] == 'L' && entry[2] == 'P') {
      // Simultaneous Latching Pulses == SLP
      allow_simultaneous_latching_pulses = 1;
    } else if (entry[0] == 'B') {
      hal_gpio_pin_t pin = hal_gpio_parse_pin(entry + 1);
      hal_gpio_pull_t pull = hal_gpio_parse_pull(entry + 3);
      hal_gpio_init(pin, 1, pull);

      buttons[buttons_cnt].pin = pin;
      buttons[buttons_cnt].long_press_duration_ms = 2000;
      buttons[buttons_cnt].multi_press_duration_ms = 800;
      buttons[buttons_cnt].on_long_press = on_reset_clicked;
      buttons_cnt++;
    } else if (entry[0] == 'L') {
      hal_gpio_pin_t pin = hal_gpio_parse_pin(entry + 1);
      hal_gpio_init(pin, 0, HAL_GPIO_PULL_NONE);
      leds[leds_cnt].pin = pin;
      leds[leds_cnt].on_high = entry[3] != 'i';

      led_init(&leds[leds_cnt]);

      network_indicator.leds[0] = &leds[leds_cnt];
      network_indicator.leds[1] = NULL;
      network_indicator.has_dedicated_led = true;

      has_dedicated_status_led = true;
      leds_cnt++;
    } else if (entry[0] == 'I') {
      hal_gpio_pin_t pin = hal_gpio_parse_pin(entry + 1);
      hal_gpio_init(pin, 0, HAL_GPIO_PULL_NONE);
      leds[leds_cnt].pin = pin;
      leds[leds_cnt].on_high = entry[3] != 'i';
      led_init(&leds[leds_cnt]);

      for (int index = 0; index < 4; index++) {
        if (relay_clusters[index].indicator_led == NULL) {
          relay_clusters[index].indicator_led = &leds[leds_cnt];
          break;
        }
      }

      if (!has_dedicated_status_led) {
        for (int index = 0; index < 4; index++) {
          if (network_indicator.leds[index] == NULL) {
            network_indicator.leds[index] = &leds[leds_cnt];
            break;
          }
        }
      }
      leds_cnt++;
    } else if (entry[0] == 'S') {
      hal_gpio_pin_t pin = hal_gpio_parse_pin(entry + 1);
      hal_gpio_pull_t pull = hal_gpio_parse_pull(entry + 3);
      hal_gpio_init(pin, 1, pull);

      buttons[buttons_cnt].pin = pin;
      buttons[buttons_cnt].long_press_duration_ms = 800;
      buttons[buttons_cnt].multi_press_duration_ms = 800;

      switch_clusters[switch_clusters_cnt].switch_idx = switch_clusters_cnt;
      switch_clusters[switch_clusters_cnt].mode =
          ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_TOGGLE;
      switch_clusters[switch_clusters_cnt].action =
          ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_TOGGLE_SIMPLE;
      switch_clusters[switch_clusters_cnt].relay_mode =
          ZCL_ONOFF_CONFIGURATION_RELAY_MODE_SHORT;
      switch_clusters[switch_clusters_cnt].binded_mode =
          ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT;
      switch_clusters[switch_clusters_cnt].relay_index =
          switch_clusters_cnt + 1;
      switch_clusters[switch_clusters_cnt].button = &buttons[buttons_cnt];
      switch_clusters[switch_clusters_cnt].level_move_rate = 50;
      buttons_cnt++;
      switch_clusters_cnt++;
    } else if (entry[0] == 'R') {
      hal_gpio_pin_t pin = hal_gpio_parse_pin(entry + 1);
      hal_gpio_init(pin, 0, HAL_GPIO_PULL_NONE);

      relays[relays_cnt].pin = pin;
      relays[relays_cnt].on_high = 1;

      if (entry[3] != '\0') {
        pin = hal_gpio_parse_pin(entry + 3);
        hal_gpio_init(pin, 0, HAL_GPIO_PULL_NONE);
        relays[relays_cnt].off_pin = pin;
        relays[relays_cnt].is_latching = 1;
      }

      relay_clusters[relay_clusters_cnt].relay_idx = relay_clusters_cnt;
      relay_clusters[relay_clusters_cnt].relay = &relays[relays_cnt];

      relays_cnt++;
      relay_clusters_cnt++;
    } else if (entry[0] == 'C') {
      hal_gpio_pin_t open_pin = hal_gpio_parse_pin(entry + 1);
      hal_gpio_pin_t close_pin = hal_gpio_parse_pin(entry + 3);

      hal_gpio_init(open_pin, 0, HAL_GPIO_PULL_NONE);
      hal_gpio_init(close_pin, 0, HAL_GPIO_PULL_NONE);

      relays[relays_cnt].pin = open_pin;
      relays[relays_cnt].on_high = 1;
      relays[relays_cnt].is_latching = 0;
      relay_t *open_relay = &relays[relays_cnt++];

      relays[relays_cnt].pin = close_pin;
      relays[relays_cnt].on_high = 1;
      relays[relays_cnt].is_latching = 0;
      relay_t *close_relay = &relays[relays_cnt++];

      cover_clusters[cover_clusters_cnt].open_relay = open_relay;
      cover_clusters[cover_clusters_cnt].close_relay = close_relay;
      cover_clusters[cover_clusters_cnt].cover_idx = cover_clusters_cnt;
      cover_clusters_cnt++;
    } else if (entry[0] == 'i') {
      uint32_t image_type = parse_int(entry + 1);
      hal_zigbee_set_image_type(image_type);
    } else if (entry[0] == 'M') {
      for (int index = 0; index < switch_clusters_cnt; index++) {
        switch_clusters[index].mode =
            ZCL_ONOFF_CONFIGURATION_SWITCH_TYPE_MOMENTARY;
      }
    } else if (entry[0] == 'E' && entry[1] == 'P') {
      // HLW8012/BL0937 energy monitoring: EP<CF_PIN><CF1_PIN><SEL_PIN>

      printf("Config: Found energy monitoring entry\r\n");
      printf("Config: Parsing HLW8012 pins\r\n");
      printf("Config: Entry='%s'\r\n", entry);

      hal_gpio_pin_t cf_pin = hal_gpio_parse_pin(entry + 2);
      hal_gpio_pin_t cf1_pin = hal_gpio_parse_pin(entry + 4);
      hal_gpio_pin_t sel_pin = hal_gpio_parse_pin(entry + 6);

      if (cf_pin != HAL_INVALID_PIN && cf1_pin != HAL_INVALID_PIN && sel_pin != HAL_INVALID_PIN) {
        if (hlw8012_init(&hlw8012_device, cf_pin, cf1_pin, sel_pin) == 0) {
          energy_meter = hlw8012_as_energy_meter(&hlw8012_device);
          electrical_measurement_cluster_init(&elec_meas_cluster, energy_meter);
          metering_cluster_init(&metering_cluster, energy_meter);
          energy_monitoring_enabled = 1;
          energy_monitoring_endpoint = 1; // Default to endpoint 1
          printf("Config: HLW8012 on CF=%04x CF1=%04x SEL=%04x\r\n", cf_pin, cf1_pin, sel_pin);
        }
      }
    }
  }

  periferals_init();

  energy_monitoring_init();

  printf("Initializing Zigbee with %d switches, %d relays, %d covers\r\n",
         switch_clusters_cnt, relay_clusters_cnt, cover_clusters_cnt);

  uint8_t total_endpoints = switch_clusters_cnt + relay_clusters_cnt + cover_clusters_cnt;

  hal_zigbee_cluster *cluster_ptr = clusters;

  // special case when no switches or relays are defined, so we can init a
  // "clean" device and configure it while running endpoint 1 still needs to be
  // initialised even though wenn no switches or relays are defined, so it can
  // join the network!
  if (total_endpoints == 0)
    total_endpoints = 1;

  for (int index = 0; index < total_endpoints; index++) {
    endpoints[index].endpoint = index + 1;
    endpoints[index].profile_id = 0x0104;
    endpoints[index].device_id = 0xffff;
  }

  endpoints[0].clusters = cluster_ptr;
  basic_cluster_add_to_endpoint(&basic_cluster, &endpoints[0]);

  hal_ota_cluster_setup(&endpoints[0].clusters[endpoints[0].cluster_count]);
  endpoints[0].cluster_count++;

  // Add energy monitoring clusters to endpoint 0 BEFORE advancing cluster_ptr
  // This must happen here because the loops below will reassign cluster_ptr
  if (energy_monitoring_enabled) {
    electrical_measurement_cluster_add_to_endpoint(&elec_meas_cluster, &endpoints[0]);
    metering_cluster_add_to_endpoint(&metering_cluster, &endpoints[0]);
    printf("Energy monitoring clusters added to endpoint 1\r\n");
  }

  for (int index = 0; index < switch_clusters_cnt; index++) {
    if (index != 0) {
      cluster_ptr += endpoints[index - 1].cluster_count;
      endpoints[index].clusters = cluster_ptr;
    }
    switch_cluster_add_to_endpoint(&switch_clusters[index], &endpoints[index]);
  }
  for (int index = 0; index < relay_clusters_cnt; index++) {
    cluster_ptr += endpoints[switch_clusters_cnt + index - 1].cluster_count;
    endpoints[switch_clusters_cnt + index].clusters = cluster_ptr;
    relay_cluster_add_to_endpoint(&relay_clusters[index],
                                  &endpoints[switch_clusters_cnt + index]);
    // Group cluster is stateless, safe to add to multiple endpoints
    group_cluster_add_to_endpoint(&group_cluster,
                                  &endpoints[switch_clusters_cnt + index]);
  }
  int cover_base = switch_clusters_cnt + relay_clusters_cnt;
  for (int index = 0; index < cover_clusters_cnt; index++) {
    cluster_ptr += endpoints[cover_base + index - 1].cluster_count;
    endpoints[cover_base + index].clusters = cluster_ptr;
    cover_cluster_add_to_endpoint(&cover_clusters[index],
                                           &endpoints[cover_base + index]);
  }

  hal_zigbee_init(endpoints, total_endpoints);
  while (cursor != (char *)device_config_str.data) {
    cursor--;
    if (*cursor == '\0') {
      *cursor = ';';
    }
  }

  printf("Config parsed successfully\r\n");
}

void network_indicator_on_network_status_change(
    hal_zigbee_network_status_t new_status) {
  printf("Network status changed to %d\r\n", new_status);
  if (new_status == HAL_ZIGBEE_NETWORK_JOINED) {
    network_indicator_connected(&network_indicator);
    update_relay_clusters();
  } else {
    network_indicator_not_connected(&network_indicator);
  }
}

void periferals_init() {
  for (int index = 0; index < buttons_cnt; index++) {
    btn_init(&buttons[index]);
  }
  for (int index = 0; index < leds_cnt; index++) {
    led_init(&leds[index]);
  }
  for (int index = 0; index < relays_cnt; index++) {
    relay_init(&relays[index]);
  }
  if (hal_zigbee_get_network_status() == HAL_ZIGBEE_NETWORK_JOINED) {
    network_indicator_connected(&network_indicator);
  } else {
    network_indicator_not_connected(&network_indicator);
  }
  hal_register_on_network_status_change_callback(
      network_indicator_on_network_status_change);
}

void energy_monitoring_init() {
  if (!energy_monitoring_enabled) {
    return;
  }

  energy_monitoring_task.handler = _energy_monitoring_task_handler;
  energy_monitoring_task.arg = NULL;
  hal_tasks_init(&energy_monitoring_task);
  hal_tasks_schedule(&energy_monitoring_task, 1000);
}

void _energy_monitoring_task_handler(void *arg) {
  (void)arg;
  
  // Update measurements from energy IC
  electrical_measurement_cluster_update(&elec_meas_cluster);
  metering_cluster_update(&metering_cluster);

  // Send reports if values changed significantly
  if (hal_zigbee_get_network_status() == HAL_ZIGBEE_NETWORK_JOINED) {
    electrical_measurement_cluster_report(&elec_meas_cluster);
    metering_cluster_report(&metering_cluster);
  }

  hal_tasks_schedule(&energy_monitoring_task, 1000);
}

// Helper functions

char *seek_until(char *cursor, char needle) {
  while (*cursor != needle && *cursor != '\0') {
    cursor++;
  }
  return (cursor);
}

char *extract_next_entry(char **cursor) {
  char *end = seek_until(*cursor, ';');

  *end = '\0';
  char *res = *cursor;
  *cursor = end + 1;
  return (res);
}

uint32_t parse_int(const char *s) {
  if (!s)
    return 0;

  uint32_t n = 0;
  while (*s >= '0' && *s <= '9') {
    n = n * 10 + (uint32_t)(*s - '0');
    s++;
  }
  return n;
}
