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

#include "thingsboard_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#undef NDEBUG
#include <assert.h>

static void test_config(void)
{
    AppConfig config;
    json_object *root = json_tokener_parse("{\"polling_period_seconds\":10}");
    assert(app_config_parse(root, &config) && !config.mqtt.enabled);
    json_object_object_add(root, "mqtt", json_tokener_parse("{\"host\":\"localhost\",\"access_token\":\"test-token\"}"));
    assert(app_config_parse(root, &config));
    assert(config.mqtt.enabled && config.mqtt.tls && config.mqtt.port == 8883);
    assert(config.mqtt.max_pending_sensors == 256);
    assert(!strcmp(config.mqtt.host, "localhost") && !strcmp(config.mqtt.access_token, "test-token"));
    json_object_object_add(root, "mqtt", json_tokener_parse("{\"host\":\"127.0.0.1\",\"access_token\":\"test-token\",\"tls\":false,\"port\":23456,\"max_pending_sensors\":2}"));
    assert(app_config_parse(root, &config));
    assert(!config.mqtt.tls && config.mqtt.port == 23456 && config.mqtt.max_pending_sensors == 2);
    json_object_object_add(root, "mqtt", json_tokener_parse("{\"enabled\":false}"));
    assert(app_config_parse(root, &config) && !config.mqtt.enabled);
    const char *bad[] = {
        "null", "true", "[]", "{}", "{\"host\":\"localhost\"}",
        "{\"enabled\":\"false\"}", "{\"enabled\":false,\"tls\":1}",
        "{\"enabled\":false,\"port\":0}", "{\"enabled\":false,\"port\":65536}",
        "{\"enabled\":false,\"port\":1.5}", "{\"enabled\":false,\"port\":true}",
        "{\"enabled\":false,\"max_pending_sensors\":0}",
        "{\"enabled\":false,\"max_pending_sensors\":10001}",
        "{\"enabled\":false,\"max_pending_sensors\":1.5}",
        "{\"enabled\":false,\"host\":false}",
        "{\"enabled\":false,\"host\":\"mqtt://localhost\"}",
        "{\"enabled\":false,\"host\":\"local host\"}",
        "{\"enabled\":false,\"host\":\"local\\u0000host\"}",
        "{\"enabled\":false,\"access_token\":\"x\\u0000y\"}",
        "{\"enabled\":false,\"tls\":false,\"ca_file\":\"ca.pem\"}",
        "{\"enabled\":false,\"ca_file\":123}",
        "{\"enabled\":false,\"typo\":1}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        json_object_object_add(root, "mqtt", json_tokener_parse(bad[i]));
        assert(!app_config_parse(root, &config));
    }
    json_object_put(root);
}

static void test_payload(void)
{
    Sensor sensor = {.address = "EC:20:1E:58:25:10", .temperature_tenths = 133,
                     .humidity = 48, .battery_low = true, .received_at_ms = INT64_C(1794046826202)};
    json_object *payload = thingsboard_mqtt_reading_payload(&sensor);
    json_object *entries = json_object_object_get(payload, sensor.address);
    assert(json_object_array_length(entries) == 1);
    json_object *entry = json_object_array_get_idx(entries, 0);
    assert(json_object_get_int64(json_object_object_get(entry, "ts")) == sensor.received_at_ms);
    json_object *values = json_object_object_get(entry, "values");
    assert(!strcmp(json_object_to_json_string(json_object_object_get(values, "temperature_c")), "13.3"));
    assert(json_object_get_int(json_object_object_get(values, "humidity_pct")) == 48);
    assert(json_object_get_boolean(json_object_object_get(values, "battery_low")));
    assert(!json_object_object_get_ex(values, "rssi_dbm", &entries));
    json_object_put(payload);
    sensor.temperature_tenths = -55;
    sensor.has_rssi = true;
    sensor.rssi = -67;
    payload = thingsboard_mqtt_reading_payload(&sensor);
    entry = json_object_array_get_idx(json_object_object_get(payload, sensor.address), 0);
    values = json_object_object_get(entry, "values");
    assert(json_object_get_double(json_object_object_get(values, "temperature_c")) == -5.5);
    assert(json_object_get_int(json_object_object_get(values, "rssi_dbm")) == -67);
    json_object_put(payload);
}

/* The Python test supplies readings and checks MQTT packets on a local socket */
static int run_client(const char *path)
{
    AppConfig config;
    ThingsBoardMqtt *output;
    if (!app_config_load(path, &config) || !thingsboard_mqtt_open(&output, &config.mqtt))
        return 1;
    puts("ready");
    fflush(stdout);
    Sensor sensor = {.humidity = 48, .battery_low = true};
    int fresh, rssi;
    while (scanf("%17s %d %" SCNd64 " %d %d", sensor.address, &sensor.temperature_tenths,
                 &sensor.received_at_ms, &fresh, &rssi) == 5) {
        sensor.fresh = fresh != 0;
        sensor.has_rssi = rssi != 0;
        sensor.rssi = (int16_t)rssi;
        thingsboard_mqtt_report(output, &sensor, 1);
        puts("queued");
        fflush(stdout);
    }
    thingsboard_mqtt_close(output);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2)
        return run_client(argv[1]);
    test_config();
    test_payload();
    ThingsBoardMqtt *output = NULL;
    MqttConfig config = {0};
    assert(thingsboard_mqtt_open(&output, &config) && !output);
    thingsboard_mqtt_report(output, NULL, 0);
    thingsboard_mqtt_close(output);
    puts("MQTT config and payload tests passed");
    return 0;
}
