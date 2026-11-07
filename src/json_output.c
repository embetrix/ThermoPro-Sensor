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

#include "json_output.h"

#include <stdio.h>

json_object *sensor_to_json(const Sensor *sensor)
{
    json_object *output = json_object_new_object();
    json_object_object_add(output, "sensor", json_object_new_string(sensor->name));
    json_object *reading = NULL;
    if (sensor->fresh) {
        reading = json_object_new_object();
        json_object_object_add(reading, "received_at", json_object_new_string(sensor->received_at));
        json_object *temperature = json_object_new_double(sensor->temperature_tenths / 10.0);
        json_object_set_serializer(temperature, json_object_double_to_json_string, "%.1f", NULL);
        json_object_object_add(reading, "temperature_c", temperature);
        json_object_object_add(reading, "humidity_pct", json_object_new_int((int)sensor->humidity));
        json_object_object_add(reading, "battery_low", json_object_new_boolean(sensor->battery_low));
        json_object_object_add(reading, "rssi_dbm", sensor->has_rssi ? json_object_new_int(sensor->rssi) : NULL);
    }
    json_object_object_add(output, "reading", reading);
    return output;
}

bool json_output_report(const Sensor *sensors, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        json_object *output = sensor_to_json(&sensors[i]);
        int r = puts(json_object_to_json_string_ext(output, JSON_C_TO_STRING_PLAIN));
        json_object_put(output);
        if (r == EOF)
            return false;
    }
    return fflush(stdout) == 0;
}
