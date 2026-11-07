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
#include "thingsboard_mqtt.h"

#include <errno.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define THINGSBOARD_CONNECT_TOPIC "v1/gateway/connect"
#define THINGSBOARD_TELEMETRY_TOPIC "v1/gateway/telemetry"
#define MQTT_STATUS_INTERVAL_SECONDS 30

/* Each slot holds one waiting reading, plus at most one message being sent */
typedef struct {
    Sensor reading;
    bool used;
    bool pending;
} PendingSensor;

struct ThingsBoardMqtt {
    struct mosquitto *client;
    pthread_mutex_t mutex;
    PendingSensor *sensors;
    size_t capacity;
    size_t next;
    size_t current;
    int message_id;
    bool sending_reading;
    bool connected;
    bool stopping;
    bool thread_started;
    bool network_stopped;
    time_t last_status;
    pthread_t thread;
    char host[256];
    int port;
};

static time_t monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

static void log_network_error(ThingsBoardMqtt *output, const char *action, int error, int system_error)
{
    char reason[256];
    const char *detail = mosquitto_strerror(error);
    if (error == MOSQ_ERR_ERRNO && strerror_r(system_error, reason, sizeof(reason)) == 0)
        detail = reason;
    fprintf(stderr, "MQTT %s (%s:%d): %s\n", action, output->host, output->port, detail);
}

json_object *thingsboard_mqtt_reading_payload(const Sensor *sensor)
{
    json_object *root = json_object_new_object();
    json_object *entries = json_object_new_array();
    json_object *entry = json_object_new_object();
    json_object *values = json_object_new_object();
    json_object *temperature = json_object_new_double(sensor->temperature_tenths / 10.0);
    json_object_set_serializer(temperature, json_object_double_to_json_string, "%.1f", NULL);
    json_object_object_add(values, "temperature_c", temperature);
    json_object_object_add(values, "humidity_pct", json_object_new_int((int)sensor->humidity));
    json_object_object_add(values, "battery_low", json_object_new_boolean(sensor->battery_low));
    if (sensor->has_rssi)
        json_object_object_add(values, "rssi_dbm", json_object_new_int(sensor->rssi));
    json_object_object_add(entry, "ts", json_object_new_int64(sensor->received_at_ms));
    json_object_object_add(entry, "values", values);
    json_object_array_add(entries, entry);
    json_object_object_add(root, sensor->address, entries);
    return root;
}

/* The caller holds the mutex */
static bool publish_json(ThingsBoardMqtt *output, const char *topic, json_object *payload)
{
    const char *text = json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN);
    int result = mosquitto_publish(output->client, &output->message_id, topic,
                                  (int)strlen(text), text, 1, false);
    json_object_put(payload);
    if (result != MOSQ_ERR_SUCCESS) {
        output->message_id = 0;
        fprintf(stderr, "MQTT publish failed: %s\n", mosquitto_strerror(result));
        return false;
    }
    return true;
}

static void send_next(ThingsBoardMqtt *output)
{
    if (!output->connected || output->stopping || output->message_id)
        return;
    for (size_t offset = 0; offset < output->capacity; offset++) {
        size_t index = (output->next + offset) % output->capacity;
        PendingSensor *slot = &output->sensors[index];
        if (!slot->used || !slot->pending)
            continue;
        output->current = index;
        output->sending_reading = false;
        json_object *message = json_object_new_object();
        json_object_object_add(message, "device", json_object_new_string(slot->reading.address));
        /* Wait for the gateway connect acknowledgement before sending data */
        publish_json(output, THINGSBOARD_CONNECT_TOPIC, message);
        return;
    }
}

static void on_connect(struct mosquitto *client, void *userdata, int result)
{
    (void)client;
    ThingsBoardMqtt *output = userdata;
    pthread_mutex_lock(&output->mutex);
    output->connected = result == 0;
    if (output->stopping) {
        pthread_mutex_unlock(&output->mutex);
        return;
    }
    if (result == 0) {
        fprintf(stderr, "MQTT connected (%s:%d)\n", output->host, output->port);
        send_next(output);
    } else {
        fprintf(stderr, "MQTT connection refused: %s\n", mosquitto_connack_string(result));
    }
    pthread_mutex_unlock(&output->mutex);
}

static void on_disconnect(struct mosquitto *client, void *userdata, int result)
{
    int system_error = errno;
    (void)client;
    ThingsBoardMqtt *output = userdata;
    pthread_mutex_lock(&output->mutex);
    output->connected = false;
    if (!output->stopping) {
        output->last_status = monotonic_seconds();
        log_network_error(output, "disconnected", result, system_error);
    }
    pthread_mutex_unlock(&output->mutex);
}

static void on_publish(struct mosquitto *client, void *userdata, int message_id)
{
    (void)client;
    ThingsBoardMqtt *output = userdata;
    pthread_mutex_lock(&output->mutex);
    if (!output->stopping && output->message_id == message_id) {
        PendingSensor *slot = &output->sensors[output->current];
        output->message_id = 0;
        if (!output->sending_reading) {
            output->sending_reading = true;
            if (publish_json(output, THINGSBOARD_TELEMETRY_TOPIC, thingsboard_mqtt_reading_payload(&slot->reading)))
                slot->pending = false;
        } else {
            if (!slot->pending)
                slot->used = false;
            output->next = (output->current + 1) % output->capacity;
            send_next(output);
        }
    }
    pthread_mutex_unlock(&output->mutex);
}

static void *network_loop(void *userdata)
{
    ThingsBoardMqtt *output = userdata;
    fprintf(stderr, "MQTT connecting to %s:%d\n", output->host, output->port);
    int error = mosquitto_connect(output->client, output->host, output->port, 60);
    if (error != MOSQ_ERR_SUCCESS)
        log_network_error(output, "connection failed, retrying", error, errno);
    error = mosquitto_loop_forever(output->client, 1000, 1);
    int system_error = errno;
    pthread_mutex_lock(&output->mutex);
    output->network_stopped = true;
    output->connected = false;
    if (!output->stopping)
        log_network_error(output, "network stopped, check settings and restart", error, system_error);
    pthread_mutex_unlock(&output->mutex);
    return NULL;
}

bool thingsboard_mqtt_open(ThingsBoardMqtt **result, const MqttConfig *config)
{
    *result = NULL;
    if (!config->enabled) {
        fprintf(stderr, "MQTT disabled in config\n");
        return true;
    }
    if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS)
        return false;
    ThingsBoardMqtt *output = calloc(1, sizeof(*output));
    if (!output) {
        mosquitto_lib_cleanup();
        return false;
    }
    if (pthread_mutex_init(&output->mutex, NULL) != 0) {
        free(output);
        mosquitto_lib_cleanup();
        return false;
    }
    output->capacity = config->max_pending_sensors;
    output->sensors = calloc(output->capacity, sizeof(*output->sensors));
    output->client = mosquitto_new(NULL, true, output);
    if (!output->sensors || !output->client)
        goto fail;
    mosquitto_connect_callback_set(output->client, on_connect);
    mosquitto_disconnect_callback_set(output->client, on_disconnect);
    mosquitto_publish_callback_set(output->client, on_publish);
    int error = mosquitto_username_pw_set(output->client, config->access_token, NULL);
    if (error == MOSQ_ERR_SUCCESS)
        error = mosquitto_reconnect_delay_set(output->client, 1, 30, true);
    if (error == MOSQ_ERR_SUCCESS && config->tls) {
        if (config->ca_file[0])
            error = mosquitto_tls_set(output->client, config->ca_file, NULL, NULL, NULL, NULL);
        else
            error = mosquitto_int_option(output->client, MOSQ_OPT_TLS_USE_OS_CERTS, 1);
    }
    if (error != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT setup failed: %s\n", mosquitto_strerror(error));
        goto fail;
    }
    memcpy(output->host, config->host, sizeof(output->host));
    output->port = (int)config->port;
    output->last_status = monotonic_seconds();
    error = mosquitto_threaded_set(output->client, true);
    if (error != MOSQ_ERR_SUCCESS || pthread_create(&output->thread, NULL, network_loop, output) != 0) {
        fprintf(stderr, "Cannot start MQTT network thread\n");
        goto fail;
    }
    output->thread_started = true;
    *result = output;
    return true;
fail:
    fprintf(stderr, "Cannot start MQTT output\n");
    thingsboard_mqtt_close(output);
    return false;
}

void thingsboard_mqtt_report(ThingsBoardMqtt *output, const Sensor *sensors, size_t count)
{
    if (!output)
        return;
    pthread_mutex_lock(&output->mutex);
    size_t dropped = 0;
    for (size_t i = 0; i < count; i++) {
        if (!sensors[i].fresh)
            continue;
        size_t available = output->capacity;
        size_t index;
        for (index = 0; index < output->capacity; index++) {
            if (output->sensors[index].used &&
                !strcmp(output->sensors[index].reading.address, sensors[i].address))
                break;
            if (!output->sensors[index].used)
                available = index;
        }
        if (index == output->capacity)
            index = available;
        if (index == output->capacity) {
            dropped++;
            continue;
        }
        output->sensors[index].reading = sensors[i];
        output->sensors[index].reading.name = NULL;
        output->sensors[index].used = true;
        output->sensors[index].pending = true;
    }
    if (dropped)
        fprintf(stderr, "MQTT queue full: skipped %zu readings\n", dropped);
    time_t now = monotonic_seconds();
    if (!output->connected && !output->stopping && now - output->last_status >= MQTT_STATUS_INTERVAL_SECONDS) {
        size_t pending = 0;
        for (size_t i = 0; i < output->capacity; i++)
            if (output->sensors[i].used)
                pending++;
        fprintf(stderr, "MQTT offline (%s:%d): %s, %zu sensors waiting\n",
                output->host, output->port,
                output->network_stopped ? "network stopped, check settings and restart" : "waiting for connection or retry",
                pending);
        output->last_status = now;
    }
    send_next(output);
    pthread_mutex_unlock(&output->mutex);
}

void thingsboard_mqtt_close(ThingsBoardMqtt *output)
{
    if (!output)
        return;
    pthread_mutex_lock(&output->mutex);
    output->stopping = true;
    pthread_mutex_unlock(&output->mutex);
    if (output->client) {
        mosquitto_disconnect(output->client);
        if (output->thread_started) {
            pthread_cancel(output->thread);
            pthread_join(output->thread, NULL);
        }
        mosquitto_destroy(output->client);
    }
    pthread_mutex_destroy(&output->mutex);
    free(output->sensors);
    free(output);
    mosquitto_lib_cleanup();
}
