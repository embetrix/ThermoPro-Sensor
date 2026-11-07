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

/* Check the config without using Bluetooth */
bool app_config_parse(json_object *root, AppConfig *config)
{
    json_object *period, *duplicates, *adapter, *discovery;
    const char *adapter_name = "hci0";
    const char *error = NULL;

#define REQUIRE(condition, message) \
    do { if (!(condition)) { error = (message); goto fail; } } while (0)

    memset(config, 0, sizeof(*config));
    REQUIRE(json_object_is_type(root, json_type_object), "config must be a JSON object");
    json_object_object_foreach(root, key, value) {
        (void)value;
        REQUIRE(!strcmp(key, "polling_period_seconds") ||
                !strcmp(key, "DuplicateData") || !strcmp(key, "adapter") || !strcmp(key, "discover_sensors"),
                "unknown config key (use polling_period_seconds, DuplicateData, adapter, discover_sensors)");
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
