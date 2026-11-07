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

#ifndef THERMOPRO_SQLITE_STORE_H
#define THERMOPRO_SQLITE_STORE_H

#include "app_config.h"
#include "sensor.h"

#include <stddef.h>
#include <time.h>
#include <sqlite3.h>

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *insert;
    sqlite3_stmt *prune;
    unsigned retention_days;
    sqlite3_int64 max_pages;
} SqliteStore;

/* No database config means no database logging */
bool sqlite_store_open(SqliteStore *store, const DatabaseConfig *config);
/* Save fresh readings together and delete old ones if space is needed */
/* Fresh flags are unchanged */
bool sqlite_store_write(SqliteStore *store, const Sensor *sensors, size_t count);
bool sqlite_store_prune(SqliteStore *store, time_t now);
void sqlite_store_close(SqliteStore *store);

#endif
