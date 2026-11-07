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
#include "bluez_scanner.h"
#include "json_output.h"
#include "sensor_registry.h"
#include "thermopro_decoder.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <systemd/sd-bus.h>

#define BLUEZ "org.bluez"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE "org.bluez.Device1"
#define PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define OBJECT_MANAGER_IFACE "org.freedesktop.DBus.ObjectManager"
#define USEC_PER_SEC UINT64_C(1000000)

typedef struct {
    const AppConfig *config;
    SensorRegistry registry;
} ScannerContext;

/* Return D-Bus parsing errors to the caller */
#define BUS_TRY(expression) do { int result_ = (expression); if (result_ < 0) return result_; } while (0)

static int read_manufacturer_data(sd_bus_message *message, Sensor *sensor)
{
    BUS_TRY(sd_bus_message_enter_container(message, 'a', "{qv}"));
    int r;
    while ((r = sd_bus_message_enter_container(message, 'e', "qv")) > 0) {
        uint16_t manufacturer;
        const void *bytes;
        size_t length;
        BUS_TRY(sd_bus_message_read(message, "q", &manufacturer));
        BUS_TRY(sd_bus_message_enter_container(message, 'v', "ay"));
        BUS_TRY(sd_bus_message_read_array(message, 'y', &bytes, &length));
        thermopro_decode(sensor, manufacturer, bytes, length);
        BUS_TRY(sd_bus_message_exit_container(message));
        BUS_TRY(sd_bus_message_exit_container(message));
    }
    if (r < 0)
        return r;
    return sd_bus_message_exit_container(message);
}

static int read_properties(sd_bus_message *message, Sensor *sensor)
{
    BUS_TRY(sd_bus_message_enter_container(message, 'a', "{sv}"));
    int r;
    while ((r = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        const char *key;
        BUS_TRY(sd_bus_message_read(message, "s", &key));
        if (!strcmp(key, "RSSI")) {
            BUS_TRY(sd_bus_message_enter_container(message, 'v', "n"));
            BUS_TRY(sd_bus_message_read(message, "n", &sensor->rssi));
            sensor->has_rssi = true;
            BUS_TRY(sd_bus_message_exit_container(message));
        } else if (!strcmp(key, "ManufacturerData")) {
            BUS_TRY(sd_bus_message_enter_container(message, 'v', "a{qv}"));
            BUS_TRY(read_manufacturer_data(message, sensor));
            BUS_TRY(sd_bus_message_exit_container(message));
        } else {
            BUS_TRY(sd_bus_message_skip(message, "v"));
        }
        BUS_TRY(sd_bus_message_exit_container(message));
    }
    if (r < 0)
        return r;
    return sd_bus_message_exit_container(message);
}

static int properties_changed(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    ScannerContext *context = userdata;
    Sensor candidate = {0};
    Sensor *sensor = sensor_registry_resolve(&context->registry, context->config->adapter_path,
                                             sd_bus_message_get_path(message), &candidate);
    if (!sensor)
        return 0;
    const char *interface;
    BUS_TRY(sd_bus_message_read(message, "s", &interface));
    if (strcmp(interface, DEVICE_IFACE))
        return 0;
    BUS_TRY(read_properties(message, sensor));
    /* Clear the saved RSSI if BlueZ marks it invalid */
    BUS_TRY(sd_bus_message_enter_container(message, 'a', "s"));
    const char *key;
    int r;
    while ((r = sd_bus_message_read(message, "s", &key)) > 0)
        if (!strcmp(key, "RSSI"))
            sensor->has_rssi = false;
    return r < 0 ? r : sensor_registry_remember(&context->registry, sensor);
}

static int interfaces_added(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    (void)error;
    ScannerContext *context = userdata;
    const char *path;
    BUS_TRY(sd_bus_message_read(message, "o", &path));
    Sensor candidate = {0};
    Sensor *sensor = sensor_registry_resolve(&context->registry, context->config->adapter_path,
                                             path, &candidate);
    if (!sensor)
        return 0;
    BUS_TRY(sd_bus_message_enter_container(message, 'a', "{sa{sv}}"));
    int r;
    while ((r = sd_bus_message_enter_container(message, 'e', "sa{sv}")) > 0) {
        const char *interface;
        BUS_TRY(sd_bus_message_read(message, "s", &interface));
        if (!strcmp(interface, DEVICE_IFACE)) {
            sensor->has_rssi = false;
            BUS_TRY(read_properties(message, sensor));
        } else {
            BUS_TRY(sd_bus_message_skip(message, "a{sv}"));
        }
        BUS_TRY(sd_bus_message_exit_container(message));
    }
    return r < 0 ? r : sensor_registry_remember(&context->registry, sensor);
}

static int set_discovery_filter(sd_bus *bus, const AppConfig *config, sd_bus_error *error)
{
    sd_bus_message *message = NULL;
    int r = sd_bus_message_new_method_call(bus, &message, BLUEZ, config->adapter_path,
                                         ADAPTER_IFACE, "SetDiscoveryFilter");
    if (r >= 0)
        r = sd_bus_message_append(message, "a{sv}", 2,
                                  "Transport", "s", "le",
                                  "DuplicateData", "b", (int)config->duplicate_data);
    if (r >= 0)
        r = sd_bus_call(bus, message, 0, error, NULL);
    sd_bus_message_unref(message);
    return r;
}

static uint64_t monotonic_usec(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * USEC_PER_SEC + (uint64_t)now.tv_nsec / 1000;
}

int bluez_scanner_run(const AppConfig *config, const volatile sig_atomic_t *stopping)
{
    ScannerContext context = {.config = config};
    sd_bus *bus = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    bool discovery_started = false;
    int r = sd_bus_open_system(&bus);
    if (r < 0)
        goto done;
    r = sd_bus_match_signal(bus, NULL, BLUEZ, NULL, PROPERTIES_IFACE,
                            "PropertiesChanged", properties_changed, &context);
    if (r < 0)
        goto done;
    r = sd_bus_match_signal(bus, NULL, BLUEZ, NULL, OBJECT_MANAGER_IFACE,
                            "InterfacesAdded", interfaces_added, &context);
    if (r < 0)
        goto done;
    r = set_discovery_filter(bus, config, &error);
    if (r < 0)
        goto done;
    r = sd_bus_call_method(bus, BLUEZ, config->adapter_path, ADAPTER_IFACE,
                           "StartDiscovery", &error, NULL, "");
    if (r < 0)
        goto done;
    discovery_started = true;
    fprintf(stderr, "Scanning %s, report every %u seconds, DuplicateData=%s\n",
            config->adapter_path, config->polling_period_seconds, config->duplicate_data ? "true" : "false");
    uint64_t interval = config->polling_period_seconds * USEC_PER_SEC;
    uint64_t next_report = monotonic_usec() + interval;
    while (!*stopping) {
        /* Limit each batch so incoming messages do not delay reports */
        for (unsigned i = 0; i < 64 && !*stopping; i++) {
            r = sd_bus_process(bus, NULL);
            if (r == -EINTR)
                break;
            if (r < 0)
                goto done;
            if (r == 0)
                break;
        }
        if (*stopping)
            break;
        uint64_t now = monotonic_usec();
        if (now >= next_report) {
            if (!json_output_report(context.registry.sensors, context.registry.count)) {
                r = -EIO;
                goto done;
            }
            next_report = now + interval;
        }
        /* Keep processing while messages are waiting */
        if (r > 0)
            continue;
        now = monotonic_usec();
        r = sd_bus_wait(bus, now < next_report ? next_report - now : 0);
        if (r < 0 && r != -EINTR)
            goto done;
    }
    r = 0;
done:
    if (r < 0)
        fprintf(stderr, "Bluetooth error: %s\nCheck that BlueZ is running, the adapter is on, and your user has Bluetooth access\n",
                error.message ? error.message : strerror(-r));
    if (discovery_started)
        sd_bus_call_method(bus, BLUEZ, config->adapter_path, ADAPTER_IFACE,
                           "StopDiscovery", NULL, NULL, "");
    sd_bus_error_free(&error);
    sd_bus_flush_close_unref(bus);
    sensor_registry_clear(&context.registry);
    return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
