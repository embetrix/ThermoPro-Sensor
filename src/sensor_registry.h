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

#ifndef THERMOPRO_SENSOR_REGISTRY_H
#define THERMOPRO_SENSOR_REGISTRY_H

#include "sensor.h"
#include <stddef.h>

typedef struct {
    Sensor *sensors;
    size_t count;
} SensorRegistry;

/* Start registries and candidates with {0}
 * Adding a sensor may move the array and invalidate earlier pointers */
Sensor *sensor_registry_resolve(SensorRegistry *registry, const char *adapter_path,
                                const char *device_path, Sensor *candidate);
/* Save a candidate only after a valid reading
 * Return 0 on success or -errno on failure */
int sensor_registry_remember(SensorRegistry *registry, const Sensor *candidate);
void sensor_registry_clear(SensorRegistry *registry);

#endif
