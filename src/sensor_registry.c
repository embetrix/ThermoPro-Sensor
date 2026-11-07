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

#define _POSIX_C_SOURCE 200809L
#include "sensor_registry.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sensor_registry_clear(SensorRegistry *registry)
{
    for (size_t i = 0; i < registry->count; i++)
        free(registry->sensors[i].name);
    free(registry->sensors);
    memset(registry, 0, sizeof(*registry));
}

static bool normalize_address(const char *input, char output[18])
{
    if (strlen(input) != 17)
        return false;
    for (size_t i = 0; i < 17; i++) {
        unsigned char c = (unsigned char)input[i];
        if (i % 3 == 2 ? c != ':' : !isxdigit(c))
            return false;
        output[i] = (char)toupper(c);
    }
    output[17] = '\0';
    return true;
}

static Sensor *find_sensor(SensorRegistry *registry, const char *path)
{
    for (size_t i = 0; i < registry->count; i++)
        if (!strcmp(registry->sensors[i].path, path))
            return &registry->sensors[i];
    return NULL;
}

/* Add a new device only after decoding a matching packet */
Sensor *sensor_registry_resolve(SensorRegistry *registry, const char *adapter_path,
                                const char *path, Sensor *candidate)
{
    Sensor *sensor = find_sensor(registry, path);
    if (sensor)
        return sensor;
    size_t prefix_length = strlen(adapter_path);
    if (strncmp(path, adapter_path, prefix_length) ||
        strncmp(path + prefix_length, "/dev_", 5))
        return NULL;
    const char *suffix = path + prefix_length + 5;
    if (strlen(suffix) != 17)
        return NULL;
    char address[18];
    memcpy(address, suffix, sizeof(address));
    for (size_t i = 2; i < 17; i += 3) {
        if (address[i] != '_')
            return NULL;
        address[i] = ':';
    }
    if (!normalize_address(address, candidate->address))
        return NULL;
    snprintf(candidate->path, sizeof(candidate->path), "%s", path);
    return candidate;
}

int sensor_registry_remember(SensorRegistry *registry, const Sensor *candidate)
{
    if (candidate->name || !candidate->fresh)
        return 0;
    if (registry->count >= SIZE_MAX / sizeof(*registry->sensors) - 1)
        return -ENOMEM;
    char *name = strdup(candidate->address);
    if (!name)
        return -ENOMEM;
    Sensor *sensors = realloc(registry->sensors, (registry->count + 1) * sizeof(*sensors));
    if (!sensors) {
        free(name);
        return -ENOMEM;
    }
    registry->sensors = sensors;
    registry->sensors[registry->count] = *candidate;
    registry->sensors[registry->count].name = name;
    registry->count++;
    fprintf(stderr, "Discovered compatible sensor: %s\n", candidate->address);
    return 0;
}
