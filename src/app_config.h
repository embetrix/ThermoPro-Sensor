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

#ifndef THERMOPRO_APP_CONFIG_H
#define THERMOPRO_APP_CONFIG_H

#include <stdbool.h>
#include <json-c/json.h>

typedef enum {
    DATABASE_PER_REPORT,
    DATABASE_EVERY_PACKET
} DatabaseLoggingMode;

typedef struct {
    /* An empty path disables logging, zero days disables age cleanup */
    char path[4096];
    unsigned retention_days;
    /* Zero means no size limit, MB uses 1,000,000 bytes */
    unsigned max_size_mb;
    DatabaseLoggingMode logging_mode;
} DatabaseConfig;

typedef struct {
    bool enabled;
    bool tls;
    unsigned port;
    unsigned max_pending_sensors;
    char host[256];
    char access_token[1024];
    char ca_file[4096];
} MqttConfig;

typedef struct {
    unsigned polling_period_seconds;
    bool duplicate_data;
    char adapter_path[64];
    DatabaseConfig database;
    MqttConfig mqtt;
} AppConfig;

/* Check discovery settings and fill the config */
bool app_config_parse(json_object *root, AppConfig *config);
bool app_config_load(const char *path, AppConfig *config);

#endif
