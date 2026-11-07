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
#include "sqlite_store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#undef NDEBUG
#include <assert.h>

static int row_count(sqlite3 *db)
{
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM readings", -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW);
    int count = sqlite3_column_int(query, 0);
    sqlite3_finalize(query);
    return count;
}

static void test_database_config(void)
{
    const char *valid[] = {
        "{\"path\":\"readings.db\"}",
        "{\"path\":\"/tmp/readings.db\",\"retention_days\":30,\"logging_mode\":\"every_packet\"}",
        "{\"path\":\"readings.db\",\"logging_mode\":\"per_report\"}",
        "{\"path\":\"readings.db\",\"retention_days\":2147483647}",
    };
    const char *invalid[] = {
        "null", "true", "[]", "{}", "{\"path\":null}", "{\"path\":42}",
        "{\"path\":\"\"}", "{\"path\":\"x\\u0000y\"}",
        "{\"path\":\":memory:\"}", "{\"path\":\"file:readings.db\"}",
        "{\"path\":\"x\",\"retention_days\":0}",
        "{\"path\":\"x\",\"retention_days\":-1}",
        "{\"path\":\"x\",\"retention_days\":1.5}",
        "{\"path\":\"x\",\"retention_days\":\"30\"}",
        "{\"path\":\"x\",\"retention_days\":true}",
        "{\"path\":\"x\",\"retention_days\":null}",
        "{\"path\":\"x\",\"retention_days\":2147483648}",
        "{\"path\":\"x\",\"logging_mode\":\"invalid\"}",
        "{\"path\":\"x\",\"logging_mode\":\"per_report\\u0000\"}",
        "{\"path\":\"x\",\"logging_mode\":true}",
        "{\"path\":\"x\",\"retentions_days\":30}",
    };
    json_object *root = json_tokener_parse("{\"polling_period_seconds\":10}");
    AppConfig config;
    assert(app_config_parse(root, &config));
    assert(!config.database.path[0]);
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        json_object_object_add(root, "database", json_tokener_parse(valid[i]));
        assert(app_config_parse(root, &config));
        if (i == 0)
            assert(config.database.retention_days == 0 && config.database.logging_mode == DATABASE_PER_REPORT);
        if (i == 1)
            assert(config.database.retention_days == 30 && config.database.logging_mode == DATABASE_EVERY_PACKET);
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        json_object_object_add(root, "database", json_tokener_parse(invalid[i]));
        assert(!app_config_parse(root, &config));
        assert(!config.database.path[0]);
    }
    const char *sizes[] = {"0", "-1", "0.5", "true", "null", "\"10\"", "2147483648"};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char input[128];
        snprintf(input, sizeof(input), "{\"path\":\"x\",\"max_size_mb\":%s}", sizes[i]);
        json_object_object_add(root, "database", json_tokener_parse(input));
        assert(!app_config_parse(root, &config));
    }
    json_object_object_add(root, "database", json_tokener_parse("{\"path\":\"x\",\"max_size_mb\":10}"));
    assert(app_config_parse(root, &config) && config.database.max_size_mb == 10);
    json_object_object_add(root, "database", json_tokener_parse("{\"path\":\"x\"}"));
    assert(app_config_parse(root, &config) && config.database.max_size_mb == 0);
    char too_long[4097];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    json_object *database = json_object_new_object();
    json_object_object_add(database, "path", json_object_new_string(too_long));
    json_object_object_add(root, "database", database);
    assert(!app_config_parse(root, &config));
    json_object_put(root);
}

static void test_storage(void)
{
    DatabaseConfig config = {0};
    SqliteStore store;
    assert(sqlite_store_open(&store, &config));
    assert(!store.db && sqlite_store_write(&store, NULL, 0));
    assert(sqlite_store_prune(&store, (time_t)-1));
    sqlite_store_close(&store);

    char directory[] = "/tmp/thermopro-sqlite-XXXXXX";
    assert(mkdtemp(directory));
    snprintf(config.path, sizeof(config.path), "%s/readings.db", directory);
    assert(sqlite_store_open(&store, &config));
    Sensor readings[] = {
        {.address = "EC:20:1E:58:25:10", .fresh = true,
         .received_at = "2024-01-01T23:59:59.999999+00:00",
         .temperature_tenths = -133, .humidity = 48, .battery_low = true, .has_rssi = true, .rssi = -67},
        {.address = "FA:5B:AB:FB:56:D6", .fresh = true,
         .received_at = "2024-01-02T00:00:00.000000+00:00", .temperature_tenths = 240, .humidity = 50},
        {.address = "EC:20:1E:58:25:10", .fresh = false},
    };
    assert(sqlite_store_write(&store, readings, 3));
    assert(readings[0].fresh && readings[1].fresh);
    assert(row_count(store.db) == 2);
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(store.db,
        "SELECT sensor_address, received_at, temperature_tenths, humidity_pct, battery_low, rssi_dbm FROM readings ORDER BY id",
        -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(query, 0), readings[0].address));
    assert(!strcmp((const char *)sqlite3_column_text(query, 1), readings[0].received_at));
    assert(sqlite3_column_type(query, 2) == SQLITE_INTEGER && sqlite3_column_int(query, 2) == -133);
    assert(sqlite3_column_int(query, 3) == 48 && sqlite3_column_int(query, 4) == 1);
    assert(sqlite3_column_int(query, 5) == -67);
    assert(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_type(query, 5) == SQLITE_NULL);
    sqlite3_finalize(query);

    /* A failed row must undo the whole batch */
    readings[1].humidity = 101;
    assert(!sqlite_store_write(&store, readings, 3));
    assert(row_count(store.db) == 2);
    readings[1].humidity = 50;
    assert(sqlite_store_write(&store, readings, 3));
    assert(row_count(store.db) == 4);

    /* Keep old readings after restart when retention_days is omitted */
    sqlite_store_close(&store);
    assert(sqlite_store_open(&store, &config));
    assert(row_count(store.db) == 4);
    time_t jan_3_2024 = 1704240000;
    assert(sqlite_store_prune(&store, jan_3_2024) && row_count(store.db) == 4);
    store.retention_days = 1;
    assert(sqlite_store_prune(&store, jan_3_2024));
    assert(row_count(store.db) == 2);
    assert(sqlite_store_prune(&store, jan_3_2024) && row_count(store.db) == 2);
    assert(sqlite_store_prune(&store, jan_3_2024 + 1) && row_count(store.db) == 0);
    assert(sqlite_store_write(&store, readings, 3));
    store.retention_days = 2147483647;
    assert(sqlite_store_prune(&store, jan_3_2024) && row_count(store.db) == 2);
    sqlite_store_close(&store);

    /* Delete expired readings at startup */
    config.retention_days = 1;
    assert(sqlite_store_open(&store, &config));
    assert(row_count(store.db) == 0);
    sqlite_store_close(&store);
    assert(unlink(config.path) == 0);
    assert(rmdir(directory) == 0);
    assert(!sqlite_store_open(&store, &config));
    sqlite_store_close(&store);
}

static sqlite3_int64 scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW);
    sqlite3_int64 result = sqlite3_column_int64(query, 0);
    sqlite3_finalize(query);
    return result;
}

static void test_size_limit(void)
{
    char directory[] = "/tmp/thermopro-size-XXXXXX";
    assert(mkdtemp(directory));
    DatabaseConfig config = {.max_size_mb = 1};
    snprintf(config.path, sizeof(config.path), "%s/readings.db", directory);
    SqliteStore store;
    assert(sqlite_store_open(&store, &config));
    sqlite3_int64 page_size = scalar(store.db, "PRAGMA page_size");
    assert(store.max_pages == 1000000 / page_size);
    Sensor future = {.address = "EC:20:1E:58:25:10", .fresh = true,
                     .received_at = "2099-01-01T00:00:00.000000+00:00", .humidity = 50};
    assert(sqlite_store_write(&store, &future, 1));
    Sensor batch[256];
    for (unsigned round = 0; round < 80; round++) {
        for (unsigned i = 0; i < 256; i++) {
            batch[i] = future;
            batch[i].temperature_tenths = (int)(round * 256 + i);
            snprintf(batch[i].received_at, sizeof(batch[i].received_at),
                     "2026-01-01T00:00:00.%06u+00:00", round * 256 + i);
        }
        assert(sqlite_store_write(&store, batch, 256));
        assert(scalar(store.db, "PRAGMA page_count") * page_size <= 1000000);
    }
    assert(row_count(store.db) < 80 * 256);
    /* Delete by reading time, even when rows arrive out of order */
    assert(scalar(store.db, "SELECT count(*) FROM readings WHERE received_at LIKE '2099-%'") == 1);
    assert(scalar(store.db, "SELECT count(*) FROM readings WHERE received_at='2026-01-01T00:00:00.000000+00:00'") == 0);
    assert(scalar(store.db, "SELECT count(*) FROM readings WHERE temperature_tenths >= 79 * 256") == 256);
    assert(sqlite3_wal_checkpoint_v2(store.db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL) == SQLITE_OK);
    struct stat info;
    assert(stat(config.path, &info) == 0 && info.st_size <= 1000000);
    int saved_rows = row_count(store.db);
    sqlite_store_close(&store);
    assert(sqlite_store_open(&store, &config));
    assert(row_count(store.db) == saved_rows && store.max_pages == 1000000 / page_size);
    sqlite_store_close(&store);

    /* Allow growth without a limit and shrink to a lower limit at startup */
    config.max_size_mb = 0;
    assert(sqlite_store_open(&store, &config));
    assert(store.max_pages == 0 && scalar(store.db, "PRAGMA max_page_count") > 1000000 / page_size);
    assert(sqlite3_exec(store.db,
        "WITH RECURSIVE numbers(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM numbers WHERE n<12000) "
        "INSERT INTO readings(sensor_address, received_at, temperature_tenths, humidity_pct, battery_low) "
        "SELECT 'EC:20:1E:58:25:10', '2099-01-01T00:00:00.000000+00:00', n, 50, 0 FROM numbers",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(scalar(store.db, "PRAGMA page_count") * page_size > 1000000);
    saved_rows = row_count(store.db);
    sqlite_store_close(&store);
    config.max_size_mb = 1;
    assert(sqlite_store_open(&store, &config));
    assert(row_count(store.db) > 0 && row_count(store.db) < saved_rows);
    assert(scalar(store.db, "PRAGMA page_count") * page_size <= 1000000);
    assert(stat(config.path, &info) == 0 && info.st_size <= 1000000);
    sqlite_store_close(&store);

    /* Finish a file shrink saved in the WAL at startup */
    config.max_size_mb = 0;
    assert(sqlite_store_open(&store, &config));
    assert(sqlite3_exec(store.db, "PRAGMA wal_autocheckpoint=0;"
        "INSERT INTO readings(sensor_address, received_at, temperature_tenths, humidity_pct, battery_low) "
        "SELECT sensor_address, received_at, temperature_tenths, humidity_pct, battery_low FROM readings;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_wal_checkpoint_v2(store.db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL) == SQLITE_OK);
    assert(stat(config.path, &info) == 0 && info.st_size > 1000000);
    assert(sqlite3_exec(store.db, "DELETE FROM readings WHERE id != (SELECT max(id) FROM readings); VACUUM;",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(stat(config.path, &info) == 0 && info.st_size > 1000000);
    assert(scalar(store.db, "PRAGMA page_count") * page_size <= 1000000);
    SqliteStore reopened;
    config.max_size_mb = 1;
    assert(sqlite_store_open(&reopened, &config));
    assert(row_count(reopened.db) == 1);
    assert(stat(config.path, &info) == 0 && info.st_size <= 1000000);
    sqlite_store_close(&reopened);
    sqlite_store_close(&store);
    assert(unlink(config.path) == 0);
    assert(rmdir(directory) == 0);
}

int main(void)
{
    test_database_config();
    test_storage();
    test_size_limit();
    puts("Database config, persistence, transactions, retention, and size limit tests passed");
    return 0;
}
