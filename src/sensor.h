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

#ifndef THERMOPRO_SENSOR_H
#define THERMOPRO_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char address[18];
    char path[128];
    char *name;
    bool fresh;
    bool has_rssi;
    int16_t rssi;
    int temperature_tenths;
    unsigned humidity;
    bool battery_low;
    char received_at[40];
} Sensor;

#endif
