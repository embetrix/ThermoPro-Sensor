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

typedef struct {
    unsigned polling_period_seconds;
    bool duplicate_data;
    char adapter_path[64];
} AppConfig;

/* Check discovery settings and fill the config */
bool app_config_parse(json_object *root, AppConfig *config);
bool app_config_load(const char *path, AppConfig *config);

#endif
