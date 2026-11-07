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

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool config_string(json_object *object, const char *key, char *buffer, size_t capacity)
{
    json_object *value;
    if (!json_object_object_get_ex(object, key, &value))
        return true;
    if (!json_object_is_type(value, json_type_string))
        return false;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (length >= capacity || strlen(text) != length)
        return false;
    memcpy(buffer, text, length + 1);
    return true;
}

/* Check the config without using Bluetooth */
bool app_config_parse(json_object *root, AppConfig *config)
{
    json_object *period, *duplicates, *adapter, *discovery, *database, *mqtt;
    const char *adapter_name = "hci0";
    const char *error = NULL;

#define REQUIRE(condition, message) \
    do { if (!(condition)) { error = (message); goto fail; } } while (0)

    memset(config, 0, sizeof(*config));
    REQUIRE(json_object_is_type(root, json_type_object), "config must be a JSON object");
    json_object_object_foreach(root, key, value) {
        (void)value;
        REQUIRE(!strcmp(key, "polling_period_seconds") ||
                !strcmp(key, "DuplicateData") || !strcmp(key, "adapter") ||
                !strcmp(key, "discover_sensors") || !strcmp(key, "database") || !strcmp(key, "mqtt"),
                "unknown config key (use polling_period_seconds, DuplicateData, adapter, discover_sensors, database, mqtt)");
    }
    if (json_object_object_get_ex(root, "discover_sensors", &discovery)) {
        REQUIRE(json_object_is_type(discovery, json_type_boolean) && json_object_get_boolean(discovery),
                "discover_sensors must be true (discovery is always on)");
    }
    REQUIRE(json_object_object_get_ex(root, "polling_period_seconds", &period) &&
            json_object_is_type(period, json_type_int) &&
            json_object_get_int64(period) >= 1 && json_object_get_int64(period) <= INT_MAX,
            "polling_period_seconds must be an integer from 1 to 2147483647");
    config->polling_period_seconds = (unsigned)json_object_get_int64(period);
    config->duplicate_data = true;
    if (json_object_object_get_ex(root, "DuplicateData", &duplicates)) {
        REQUIRE(json_object_is_type(duplicates, json_type_boolean), "DuplicateData must be true or false");
        config->duplicate_data = json_object_get_boolean(duplicates);
    }
    if (json_object_object_get_ex(root, "adapter", &adapter)) {
        REQUIRE(json_object_is_type(adapter, json_type_string), "adapter must be a string like hci0");
        adapter_name = json_object_get_string(adapter);
        REQUIRE((size_t)json_object_get_string_len(adapter) == strlen(adapter_name), "adapter must not contain a NUL byte");
    }
    REQUIRE(strlen(adapter_name) < 32 && !strncmp(adapter_name, "hci", 3) && adapter_name[3],
            "adapter must start with hci followed by digits");
    for (const char *p = adapter_name + 3; *p; p++)
        REQUIRE(isdigit((unsigned char)*p), "adapter must start with hci followed by digits");
    snprintf(config->adapter_path, sizeof(config->adapter_path), "/org/bluez/%s", adapter_name);
    if (json_object_object_get_ex(root, "database", &database)) {
        json_object *path, *days, *mode, *size;
        REQUIRE(json_object_is_type(database, json_type_object), "database must be an object");
        json_object_object_foreach(database, key, value) {
            (void)value;
            REQUIRE(!strcmp(key, "path") || !strcmp(key, "retention_days") ||
                    !strcmp(key, "logging_mode") || !strcmp(key, "max_size_mb"),
                    "unknown database key (use path, retention_days, logging_mode, max_size_mb)");
        }
        REQUIRE(json_object_object_get_ex(database, "path", &path) &&
                json_object_is_type(path, json_type_string), "database.path must be a nonempty string");
        const char *path_text = json_object_get_string(path);
        size_t length = (size_t)json_object_get_string_len(path);
        REQUIRE(length > 0 && length < sizeof(config->database.path) && strlen(path_text) == length,
                "database.path must contain 1 to 4095 bytes without NUL bytes");
        REQUIRE(strcmp(path_text, ":memory:") && strncmp(path_text, "file:", 5),
                "database.path must be a file path, not a SQLite URI or :memory:");
        memcpy(config->database.path, path_text, length + 1);
        if (json_object_object_get_ex(database, "retention_days", &days)) {
            REQUIRE(json_object_is_type(days, json_type_int) &&
                    json_object_get_int64(days) >= 1 && json_object_get_int64(days) <= INT_MAX,
                    "database.retention_days must be an integer from 1 to 2147483647, or omitted");
            config->database.retention_days = (unsigned)json_object_get_int64(days);
        }
        if (json_object_object_get_ex(database, "max_size_mb", &size)) {
            REQUIRE(json_object_is_type(size, json_type_int) &&
                    json_object_get_int64(size) >= 1 && json_object_get_int64(size) <= INT_MAX,
                    "database.max_size_mb must be an integer from 1 to 2147483647, or omitted");
            config->database.max_size_mb = (unsigned)json_object_get_int64(size);
        }
        if (json_object_object_get_ex(database, "logging_mode", &mode)) {
            REQUIRE(json_object_is_type(mode, json_type_string), "database.logging_mode must be a string");
            const char *mode_text = json_object_get_string(mode);
            REQUIRE(strlen(mode_text) == (size_t)json_object_get_string_len(mode) &&
                    (!strcmp(mode_text, "per_report") || !strcmp(mode_text, "every_packet")),
                    "database.logging_mode must be per_report or every_packet");
            config->database.logging_mode = !strcmp(mode_text, "every_packet") ?
                                           DATABASE_EVERY_PACKET : DATABASE_PER_REPORT;
        }
    }
    config->mqtt.tls = true;
    config->mqtt.port = 8883;
    config->mqtt.max_pending_sensors = 256;
    if (json_object_object_get_ex(root, "mqtt", &mqtt)) {
        json_object *value;
        REQUIRE(json_object_is_type(mqtt, json_type_object), "mqtt must be an object");
        json_object_object_foreach(mqtt, key, item) {
            (void)item;
            REQUIRE(!strcmp(key, "enabled") || !strcmp(key, "host") || !strcmp(key, "port") ||
                    !strcmp(key, "tls") || !strcmp(key, "access_token") || !strcmp(key, "ca_file") ||
                    !strcmp(key, "max_pending_sensors"), "unknown mqtt config key");
        }
        config->mqtt.enabled = true;
        if (json_object_object_get_ex(mqtt, "enabled", &value)) {
            REQUIRE(json_object_is_type(value, json_type_boolean), "mqtt.enabled must be true or false");
            config->mqtt.enabled = json_object_get_boolean(value);
        }
        if (json_object_object_get_ex(mqtt, "tls", &value)) {
            REQUIRE(json_object_is_type(value, json_type_boolean), "mqtt.tls must be true or false");
            config->mqtt.tls = json_object_get_boolean(value);
        }
        config->mqtt.port = config->mqtt.tls ? 8883 : 1883;
        if (json_object_object_get_ex(mqtt, "port", &value)) {
            REQUIRE(json_object_is_type(value, json_type_int) && json_object_get_int64(value) >= 1 &&
                    json_object_get_int64(value) <= 65535, "mqtt.port must be an integer from 1 to 65535");
            config->mqtt.port = (unsigned)json_object_get_int64(value);
        }
        if (json_object_object_get_ex(mqtt, "max_pending_sensors", &value)) {
            REQUIRE(json_object_is_type(value, json_type_int) && json_object_get_int64(value) >= 1 &&
                    json_object_get_int64(value) <= 10000,
                    "mqtt.max_pending_sensors must be an integer from 1 to 10000");
            config->mqtt.max_pending_sensors = (unsigned)json_object_get_int64(value);
        }
        REQUIRE(config_string(mqtt, "host", config->mqtt.host, sizeof(config->mqtt.host)),
                "mqtt.host must be a string of at most 255 bytes without NUL bytes");
        REQUIRE(config_string(mqtt, "access_token", config->mqtt.access_token, sizeof(config->mqtt.access_token)),
                "mqtt.access_token must be a string of at most 1023 bytes without NUL bytes");
        REQUIRE(config_string(mqtt, "ca_file", config->mqtt.ca_file, sizeof(config->mqtt.ca_file)),
                "mqtt.ca_file must be a string of at most 4095 bytes without NUL bytes");
        REQUIRE(!strstr(config->mqtt.host, "://"), "mqtt.host must be a hostname or IP address without a URL scheme");
        for (const char *p = config->mqtt.host; *p; p++)
            REQUIRE(!isspace((unsigned char)*p) && *p != '/', "mqtt.host must not contain spaces or slashes");
        REQUIRE(!config->mqtt.enabled || (config->mqtt.host[0] && config->mqtt.access_token[0]),
                "enabled MQTT needs mqtt.host and mqtt.access_token");
        REQUIRE(config->mqtt.tls || !config->mqtt.ca_file[0], "mqtt.ca_file requires mqtt.tls=true");
    }
    return true;
fail:
    fprintf(stderr, "Config error: %s\n", error);
    memset(config, 0, sizeof(*config));
    return false;
#undef REQUIRE
}

bool app_config_load(const char *path, AppConfig *config)
{
    json_object *root = json_object_from_file(path);
    if (!root) {
        fprintf(stderr, "Cannot read config %s: %s\n", path, json_util_get_last_err());
        return false;
    }
    bool ok = app_config_parse(root, config);
    json_object_put(root);
    return ok;
}
