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
#include "thermopro_decoder.h"

#include <stdio.h>
#include <time.h>

_Static_assert(THERMOPRO_PAYLOAD_LENGTH >= 3 && THERMOPRO_PAYLOAD_LENGTH <= 65535,
               "THERMOPRO_PAYLOAD_LENGTH must be between 3 and 65535");
_Static_assert(THERMOPRO_PACKET_MARKER >= 0 && THERMOPRO_PACKET_MARKER <= 255,
               "THERMOPRO_PACKET_MARKER must fit in one byte");
_Static_assert(THERMOPRO_MAX_HUMIDITY >= 0 && THERMOPRO_MAX_HUMIDITY <= 100,
               "THERMOPRO_MAX_HUMIDITY must be between 0 and 100");

/* BlueZ puts the first two manufacturer bytes in a little-endian key */
bool thermopro_decode(Sensor *sensor, uint16_t manufacturer, const uint8_t *data, size_t length)
{
    if (length != THERMOPRO_PAYLOAD_LENGTH ||
        (manufacturer & 0xff) != THERMOPRO_PACKET_MARKER ||
        data[1] > THERMOPRO_MAX_HUMIDITY)
        return false;
    unsigned raw = (manufacturer >> 8) | ((unsigned)data[0] << 8);
    sensor->temperature_tenths = raw >= 0x8000 ? (int)raw - 0x10000 : (int)raw;
    sensor->humidity = data[1];
    sensor->battery_low = !(data[2] & 0x02);
    struct timespec now;
    struct tm utc;
    char date[20];
    clock_gettime(CLOCK_REALTIME, &now);
    gmtime_r(&now.tv_sec, &utc);
    strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
    snprintf(sensor->received_at, sizeof(sensor->received_at), "%s.%06u+00:00", date, (unsigned)(now.tv_nsec / 1000));
    sensor->fresh = true;
    return true;
}
