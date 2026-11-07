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
#include "app_config.h"
#include "bluez_scanner.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stopping;

static void stop_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

int main(int argc, char **argv)
{
    bool check_only = argc > 1 && !strcmp(argv[1], "--check-config");
    if ((argc > 1 && !strcmp(argv[1], "--help")) || argc > (check_only ? 3 : 2)) {
        fprintf(argc > 2 && !check_only ? stderr : stdout,
                "Usage: %s [--check-config] [config/thermopro-sensor.json]\n", argv[0]);
        return argc > (check_only ? 3 : 2) ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    const char *path = argc > (check_only ? 2 : 1) ? argv[check_only ? 2 : 1] : "config/thermopro-sensor.json";
    AppConfig config;
    if (!app_config_load(path, &config))
        return EXIT_FAILURE;
    if (check_only) {
        printf("Config OK: automatic discovery, period=%u seconds, DuplicateData=%s, adapter=%s\n",
               config.polling_period_seconds, config.duplicate_data ? "true" : "false", config.adapter_path);
        return EXIT_SUCCESS;
    }
    struct sigaction action = {0};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    return bluez_scanner_run(&config, &stopping);
}
