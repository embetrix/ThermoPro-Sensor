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

#ifndef THERMOPRO_DECODER_H
#define THERMOPRO_DECODER_H

#include "sensor.h"
#include <stddef.h>

/* Default packet checks, overridable through CMake
 * Payload length does not include the two-byte manufacturer key */
#ifndef THERMOPRO_PAYLOAD_LENGTH
#define THERMOPRO_PAYLOAD_LENGTH 5
#endif
#ifndef THERMOPRO_PACKET_MARKER
#define THERMOPRO_PACKET_MARKER 0xC2
#endif
#ifndef THERMOPRO_MAX_HUMIDITY
#define THERMOPRO_MAX_HUMIDITY 100
#endif

/* Update the reading and UTC receive time only for a matching packet */
bool thermopro_decode(Sensor *sensor, uint16_t manufacturer, const uint8_t *data, size_t length);

#endif
