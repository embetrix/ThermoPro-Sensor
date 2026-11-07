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

#include "sqlite_store.h"

#include <stdio.h>
#include <string.h>

#define DATABASE_BUSY_TIMEOUT_MS 1000

static bool database_error(SqliteStore *store, const char *operation)
{
    fprintf(stderr, "Database error (%s): %s\n", operation, sqlite3_errmsg(store->db));
    return false;
}

static bool execute(SqliteStore *store, const char *sql, const char *operation)
{
    if (sqlite3_exec(store->db, sql, NULL, NULL, NULL) != SQLITE_OK)
        return database_error(store, operation);
    return true;
}

static bool query_integer(SqliteStore *store, const char *sql, sqlite3_int64 *value)
{
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL);
    if (result == SQLITE_OK)
        result = sqlite3_step(statement);
    if (result == SQLITE_ROW)
        *value = sqlite3_column_int64(statement, 0);
    else
        database_error(store, "query size limit");
    sqlite3_finalize(statement);
    return result == SQLITE_ROW;
}

static bool delete_oldest(SqliteStore *store)
{
    /* Delete a few old readings to make room for new ones */
    if (!execute(store,
            "DELETE FROM readings WHERE id IN ("
            "SELECT id FROM readings ORDER BY received_at, id "
            "LIMIT max(128, (SELECT count(*) / 20 FROM readings)))", "delete oldest readings"))
        return false;
    int deleted = sqlite3_changes(store->db);
    if (!deleted) {
        fprintf(stderr, "Database size limit cannot fit the incoming batch or database schema\n");
        return false;
    }
    fprintf(stderr, "Database size limit: removed %d oldest readings\n", deleted);
    return true;
}

static bool configure_size_limit(SqliteStore *store, unsigned max_size_mb)
{
    if (!max_size_mb)
        return true;
    sqlite3_int64 page_size, pages;
    if (!query_integer(store, "PRAGMA page_size", &page_size) ||
        !query_integer(store, "PRAGMA page_count", &pages))
        return false;
    sqlite3_int64 limit = (sqlite3_int64)max_size_mb * 1000000 / page_size;
    /* Stay within SQLite's page limit */
    if (limit > 4294967294LL)
        limit = 4294967294LL;
    if (pages > limit) {
        /* Shrink unused space before deleting readings */
        if (!execute(store, "VACUUM", "compact oversized database") ||
            !query_integer(store, "PRAGMA page_count", &pages))
            return false;
        while (pages > limit) {
            if (!delete_oldest(store) ||
                !execute(store, "VACUUM", "compact oversized database") ||
                !query_integer(store, "PRAGMA page_count", &pages))
                return false;
        }
    }
    /* Finish a file shrink left in the WAL after a crash */
    if (sqlite3_wal_checkpoint_v2(store->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL) != SQLITE_OK)
        return database_error(store, "checkpoint size-limited database");
    char sql[80];
    snprintf(sql, sizeof(sql), "PRAGMA max_page_count=%lld", (long long)limit);
    if (!query_integer(store, sql, &store->max_pages))
        return false;
    if (store->max_pages > limit) {
        fprintf(stderr, "Database error: cannot apply configured size limit\n");
        return false;
    }
    return true;
}

void sqlite_store_close(SqliteStore *store)
{
    sqlite3_finalize(store->insert);
    sqlite3_finalize(store->prune);
    sqlite3_close(store->db);
    memset(store, 0, sizeof(*store));
}

bool sqlite_store_open(SqliteStore *store, const DatabaseConfig *config)
{
    memset(store, 0, sizeof(*store));
    if (!config->path[0])
        return true;
    store->retention_days = config->retention_days;
    if (sqlite3_open_v2(config->path, &store->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database %s: %s\n", config->path, sqlite3_errmsg(store->db));
        goto fail;
    }
    if (sqlite3_busy_timeout(store->db, DATABASE_BUSY_TIMEOUT_MS) != SQLITE_OK) {
        database_error(store, "busy timeout");
        goto fail;
    }
    sqlite3_stmt *journal = NULL;
    int result = sqlite3_prepare_v2(store->db, "PRAGMA journal_mode=WAL", -1, &journal, NULL);
    if (result == SQLITE_OK)
        result = sqlite3_step(journal);
    bool wal = result == SQLITE_ROW &&
               !strcmp((const char *)sqlite3_column_text(journal, 0), "wal");
    sqlite3_finalize(journal);
    if (!wal) {
        fprintf(stderr, "Database error: cannot enable WAL for %s\n", config->path);
        goto fail;
    }
    if (!execute(store,
                 "PRAGMA synchronous=FULL;"
                 "BEGIN IMMEDIATE;"
                 "CREATE TABLE IF NOT EXISTS readings ("
                 "id INTEGER PRIMARY KEY,"
                 "sensor_address TEXT NOT NULL,"
                 "received_at TEXT NOT NULL,"
                 "temperature_tenths INTEGER NOT NULL,"
                 "humidity_pct INTEGER NOT NULL CHECK(humidity_pct BETWEEN 0 AND 100),"
                 "battery_low INTEGER NOT NULL CHECK(battery_low IN (0, 1)),"
                 "rssi_dbm INTEGER);"
                 "CREATE INDEX IF NOT EXISTS readings_sensor_time ON readings(sensor_address, received_at);"
                 "CREATE INDEX IF NOT EXISTS readings_time ON readings(received_at);"
                 "COMMIT;", "initialize schema"))
        goto fail;
    if (sqlite3_prepare_v2(store->db,
            "INSERT INTO readings(sensor_address, received_at, temperature_tenths, humidity_pct, battery_low, rssi_dbm) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6)", -1, &store->insert, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(store->db,
            "DELETE FROM readings WHERE received_at < "
            "strftime('%Y-%m-%dT%H:%M:%f000+00:00', ?1 - ?2 * 86400, 'unixepoch')",
            -1, &store->prune, NULL) != SQLITE_OK) {
        database_error(store, "prepare statements");
        goto fail;
    }
    if (!sqlite_store_prune(store, time(NULL)) ||
        !configure_size_limit(store, config->max_size_mb))
        goto fail;
    return true;
fail:
    sqlite_store_close(store);
    return false;
}

static bool write_batch(SqliteStore *store, const Sensor *sensors, size_t count, bool *at_capacity)
{
    *at_capacity = false;
    if (!execute(store, "BEGIN IMMEDIATE", "begin readings"))
        return false;
    const char *operation = "insert reading";
    sqlite3_stmt *insert = store->insert;
    for (size_t i = 0; i < count; i++) {
        const Sensor *sensor = &sensors[i];
        if (!sensor->fresh)
            continue;
        if (sqlite3_bind_text(insert, 1, sensor->address, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(insert, 2, sensor->received_at, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int(insert, 3, sensor->temperature_tenths) != SQLITE_OK ||
            sqlite3_bind_int(insert, 4, (int)sensor->humidity) != SQLITE_OK ||
            sqlite3_bind_int(insert, 5, sensor->battery_low) != SQLITE_OK ||
            (sensor->has_rssi ? sqlite3_bind_int(insert, 6, sensor->rssi) :
                                sqlite3_bind_null(insert, 6)) != SQLITE_OK ||
            sqlite3_step(insert) != SQLITE_DONE)
            goto failed;
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
    }
    operation = "commit readings";
    if (sqlite3_exec(store->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
        return true;
failed:
    *at_capacity = store->max_pages && sqlite3_errcode(store->db) == SQLITE_FULL &&
                   sqlite3_system_errno(store->db) == 0;
    if (!*at_capacity)
        database_error(store, operation);
    sqlite3_reset(insert);
    sqlite3_clear_bindings(insert);
    /* SQLITE_FULL can roll back the transaction itself */
    if (!sqlite3_get_autocommit(store->db) && !execute(store, "ROLLBACK", "rollback readings"))
        *at_capacity = false;
    return false;
}

bool sqlite_store_write(SqliteStore *store, const Sensor *sensors, size_t count)
{
    if (!store->db)
        return true;
    size_t first = 0;
    while (first < count && !sensors[first].fresh)
        first++;
    if (first == count)
        return true;
    bool at_capacity;
    while (!write_batch(store, sensors + first, count - first, &at_capacity)) {
        if (!at_capacity || !delete_oldest(store))
            return false;
    }
    return true;
}

bool sqlite_store_prune(SqliteStore *store, time_t now)
{
    if (!store->db || !store->retention_days)
        return true;
    if (now == (time_t)-1) {
        fprintf(stderr, "Database error: cannot read clock for retention cleanup\n");
        return false;
    }
    bool ok = sqlite3_bind_int64(store->prune, 1, (sqlite3_int64)now) == SQLITE_OK &&
              sqlite3_bind_int64(store->prune, 2, store->retention_days) == SQLITE_OK &&
              sqlite3_step(store->prune) == SQLITE_DONE;
    if (!ok)
        database_error(store, "retention cleanup");
    sqlite3_reset(store->prune);
    sqlite3_clear_bindings(store->prune);
    return ok;
}
