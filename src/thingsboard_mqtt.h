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

#ifndef THERMOPRO_THINGSBOARD_MQTT_H
#define THERMOPRO_THINGSBOARD_MQTT_H

#include "app_config.h"
#include "sensor.h"
#include <stddef.h>

typedef struct ThingsBoardMqtt ThingsBoardMqtt;

/* The caller frees the payload with json_object_put() */
json_object *thingsboard_mqtt_reading_payload(const Sensor *sensor);
bool thingsboard_mqtt_open(ThingsBoardMqtt **output, const MqttConfig *config);
/* Keep the latest fresh reading per sensor while waiting to send */
void thingsboard_mqtt_report(ThingsBoardMqtt *output, const Sensor *sensors, size_t count);
void thingsboard_mqtt_close(ThingsBoardMqtt *output);

#endif
