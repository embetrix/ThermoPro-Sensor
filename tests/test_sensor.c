// SPDX-License-Identifier: GPL-3.0-only
/*
 * (C) Copyright 2026
 * Embetrix Embedded Systems Solutions, ayoub.zaki@embetrix.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 3 of
 * the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include "app_config.h"
#include "json_output.h"
#include "sensor_registry.h"
#include "thermopro_decoder.h"

#include <stdio.h>
#include <string.h>
#undef NDEBUG
#include <assert.h>

#define TEST_HUMIDITY (THERMOPRO_MAX_HUMIDITY < 48 ? THERMOPRO_MAX_HUMIDITY : 48)
#define WARM_KEY (0xeb00 | THERMOPRO_PACKET_MARKER)
#define COLD_KEY (0xc900 | THERMOPRO_PACKET_MARKER)
#define WRONG_KEY (WARM_KEY ^ 1)

static json_object *example_config(void)
{
    return json_tokener_parse("{\"polling_period_seconds\":30}");
}

static void test_config(void)
{
    AppConfig config;
    json_object *root = example_config();
    assert(app_config_parse(root, &config));
    assert(config.polling_period_seconds == 30 && config.duplicate_data);
    assert(!strcmp(config.adapter_path, "/org/bluez/hci0"));

    json_object_object_add(root, "DuplicateData", json_object_new_boolean(false));
    json_object_object_add(root, "adapter", json_object_new_string("hci1"));
    assert(app_config_parse(root, &config));
    assert(!config.duplicate_data && !strcmp(config.adapter_path, "/org/bluez/hci1"));

    json_object_object_add(root, "DuplicateData", json_object_new_string("false"));
    assert(!app_config_parse(root, &config));
    json_object_object_del(root, "DuplicateData");
    json_object_object_add(root, "polling_period_seconds", json_object_new_int(0));
    assert(!app_config_parse(root, &config));
    json_object_object_add(root, "polling_period_seconds", json_object_new_int(-1));
    assert(!app_config_parse(root, &config));
    json_object_object_add(root, "polling_period_seconds", json_object_new_double(0.5));
    assert(!app_config_parse(root, &config));
    json_object_object_add(root, "polling_period_seconds", json_object_new_int(1));
    json_object_object_add(root, "adapter", json_object_new_string("../hci0"));
    assert(!app_config_parse(root, &config));
    json_object_object_del(root, "adapter");
    json_object_object_add(root, "polling_peroid_seconds", json_object_new_int(5));
    assert(!app_config_parse(root, &config));
    json_object_put(root);
}

static void test_packets_and_output(void)
{
    Sensor sensor = {.name = "Room \"A\"", .has_rssi = true, .rssi = -67};
    const uint8_t warm[THERMOPRO_PAYLOAD_LENGTH] = {0, TEST_HUMIDITY, 2};
    const uint8_t cold[THERMOPRO_PAYLOAD_LENGTH] = {0xff, THERMOPRO_MAX_HUMIDITY, 0};
    const uint8_t invalid_humidity[THERMOPRO_PAYLOAD_LENGTH] = {0, THERMOPRO_MAX_HUMIDITY + 1, 2};

    assert(thermopro_decode(&sensor, WARM_KEY, warm, sizeof(warm)));
    assert(sensor.fresh && sensor.temperature_tenths == 235 && sensor.humidity == TEST_HUMIDITY && !sensor.battery_low);
    assert(strstr(sensor.received_at, "+00:00"));
    json_object *output = sensor_to_json(&sensor);
    json_object *reading = json_object_object_get(output, "reading");
    assert(json_object_get_double(json_object_object_get(reading, "temperature_c")) == 23.5);
    assert(json_object_get_int(json_object_object_get(reading, "rssi_dbm")) == -67);
    json_object *roundtrip = json_tokener_parse(json_object_to_json_string(output));
    assert(!strcmp(json_object_get_string(json_object_object_get(roundtrip, "sensor")), sensor.name));
    json_object_put(roundtrip);
    json_object_put(output);

    assert(thermopro_decode(&sensor, COLD_KEY, cold, sizeof(cold)));
    assert(sensor.temperature_tenths == -55 && sensor.humidity == THERMOPRO_MAX_HUMIDITY && sensor.battery_low);
    assert(!thermopro_decode(&sensor, WARM_KEY, warm, sizeof(warm) - 1));
    assert(!thermopro_decode(&sensor, WRONG_KEY, warm, sizeof(warm)));
    assert(!thermopro_decode(&sensor, WARM_KEY, invalid_humidity, sizeof(invalid_humidity)));
    assert(sensor.temperature_tenths == -55);

    sensor.has_rssi = false;
    output = sensor_to_json(&sensor);
    reading = json_object_object_get(output, "reading");
    assert(json_object_object_get_ex(reading, "rssi_dbm", &roundtrip) && roundtrip == NULL);
    json_object_put(output);
    sensor.fresh = false;
    output = sensor_to_json(&sensor);
    assert(json_object_object_get_ex(output, "reading", &reading) && reading == NULL);
    json_object_put(output);
}

static void test_discovery(void)
{
    AppConfig config;
    SensorRegistry registry = {0};
    json_object *root = json_tokener_parse("{\"discover_sensors\":true,\"polling_period_seconds\":10}");
    assert(app_config_parse(root, &config));
    assert(registry.count == 0);
    Sensor candidate = {0};
    const char *path = "/org/bluez/hci0/dev_EC_20_1E_58_25_10";
    assert(!sensor_registry_resolve(&registry, config.adapter_path, "/org/bluez/hci1/dev_EC_20_1E_58_25_10", &candidate));
    assert(!sensor_registry_resolve(&registry, config.adapter_path, "/org/bluez/hci0", &candidate));
    assert(!sensor_registry_resolve(&registry, config.adapter_path, "/org/bluez/hci0/dev_invalid", &candidate));
    assert(!sensor_registry_resolve(&registry, config.adapter_path, "/org/bluez/hci0/dev_EC_20_1E_58_25_GG", &candidate));
    assert(sensor_registry_resolve(&registry, config.adapter_path, path, &candidate) == &candidate);
    assert(sensor_registry_remember(&registry, &candidate) == 0 && registry.count == 0);
    const uint8_t warm[THERMOPRO_PAYLOAD_LENGTH] = {0, TEST_HUMIDITY, 2};
    assert(!thermopro_decode(&candidate, WRONG_KEY, warm, sizeof(warm)));
    assert(sensor_registry_remember(&registry, &candidate) == 0 && registry.count == 0);
    assert(thermopro_decode(&candidate, WARM_KEY, warm, sizeof(warm)));
    assert(sensor_registry_remember(&registry, &candidate) == 0 && registry.count == 1);
    Sensor *found = sensor_registry_resolve(&registry, config.adapter_path, path, &candidate);
    assert(found == &registry.sensors[0]);
    assert(!strcmp(found->name, "EC:20:1E:58:25:10"));
    assert(found->fresh && found->temperature_tenths == 235);
    assert(sensor_registry_remember(&registry, found) == 0 && registry.count == 1);

    sensor_registry_clear(&registry);

    json_object_object_add(root, "discover_sensors", json_object_new_boolean(false));
    assert(!app_config_parse(root, &config));
    json_object_object_add(root, "discover_sensors", json_object_new_string("true"));
    assert(!app_config_parse(root, &config));
    json_object_object_del(root, "discover_sensors");
    assert(app_config_parse(root, &config) && registry.count == 0);
    sensor_registry_clear(&registry);
    json_object_put(root);
}

static void test_temperature_format(void)
{
    const struct {
        int tenths;
        const char *expected;
    } cases[] = {
        {133, "13.3"}, {-133, "-13.3"}, {240, "24.0"},
        {0, "0.0"}, {-1, "-0.1"}, {-32768, "-3276.8"}, {32767, "3276.7"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Sensor sensor = {.name = "test", .fresh = true, .temperature_tenths = cases[i].tenths};
        json_object *output = sensor_to_json(&sensor);
        json_object *reading = json_object_object_get(output, "reading");
        json_object *temperature = json_object_object_get(reading, "temperature_c");
        assert(json_object_is_type(temperature, json_type_double));
        assert(!strcmp(json_object_to_json_string_ext(temperature, JSON_C_TO_STRING_PLAIN), cases[i].expected));
        json_object_put(output);
    }
}

int main(void)
{
    test_config();
    test_packets_and_output();
    test_temperature_format();
    test_discovery();
    puts("Config, packet decoding, and JSON output tests passed");
    return 0;
}
