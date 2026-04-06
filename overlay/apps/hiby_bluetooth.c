/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/_ \ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"

#ifdef HIBY_LINUX

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/stat.h>

#include "kernel.h"
#include "audio.h"
#include "menu.h"
#include "splash.h"
#include "gui/list.h"
#include "pcm-alsa.h"

/* HiBy hosted build provides dynamic output routing helper in its
 * target-specific PCM implementation. */
int pcm_alsa_switch_playback_device(const char *device);
void hiby_pcm_set_bt_mac(const char *mac);

#define BT_MAX_DEVICES 32
#define BT_NAME_LEN 80
#define BT_LOCAL_PLAYBACK_DEVICE "plughw:0,0"
#define BT_SYS_SOCKET "/var/run/sys_server"
#define BT_LIST_FILE "/data/bt_list.txt"
#define BT_SCAN_FILE "/data/bt_scan.txt"
#define BT_SYS_REPLY_MAX 128
#define BT_DEVICE_PICK_CANCEL (-1)
#define BT_DEVICE_PICK_SCAN (-2)
#define BT_SCAN_MENU_LABEL "Scan for new devices"

struct bt_device
{
    char mac[18];
    char name[BT_NAME_LEN];
    bool paired;
};

struct bt_device_menu_data
{
    struct bt_device *devices;
    int count;
    bool include_scan_item;
};

static char bt_selected_mac[18];
static const char *bt_playback_dev = BT_LOCAL_PLAYBACK_DEVICE;
static char bt_bt_playback_dev[2][96];
static unsigned int bt_bt_playback_dev_next = 0;
static char bt_active_codec[16];

static const char *bt_codec_preference[] = {
    "aptX", "SBC", NULL
};

static bool bt_wait_for_bluealsa_pcm(const char *mac, int timeout_ticks);
static void bt_select_best_codec(const char *mac);
static bool bt_bluealsa_pcm_ready(const char *mac);
static bool bt_is_connected(const char *mac);
static bool bt_prepare_stack(void);
static void bt_connect_device(const struct bt_device *device);

static const char *bt_make_bt_playback_dev(const char *mac)
{
    char *route = bt_bt_playback_dev[bt_bt_playback_dev_next];

    bt_bt_playback_dev_next ^= 1;
    snprintf(route, sizeof(bt_bt_playback_dev[0]),
             "bluealsa:DEV=%s,PROFILE=a2dp", mac);
    return route;
}

static int bt_simplelist_ok_cancel(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    return action;
}

static const char *bt_action_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    const char **items = data;
    if (selected_item < 0 || selected_item >= 3)
    {
        buffer[0] = '\0';
        return buffer;
    }
    snprintf(buffer, buffer_len, "%s", items[selected_item]);
    return buffer;
}

static const char *bt_device_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    struct bt_device_menu_data *ctx = data;

    if (ctx->include_scan_item)
    {
        if (selected_item == 0)
        {
            snprintf(buffer, buffer_len, "%s", BT_SCAN_MENU_LABEL);
            return buffer;
        }
        selected_item--;
    }

    if (selected_item < 0 || selected_item >= ctx->count)
    {
        buffer[0] = '\0';
        return buffer;
    }

    snprintf(buffer, buffer_len, "%s (%s)%s",
        ctx->devices[selected_item].name,
        ctx->devices[selected_item].mac,
        ctx->devices[selected_item].paired ? " [Paired]" : "");
    return buffer;
}

static void bt_trim(char *s)
{
    size_t len;

    if (!s)
        return;

    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || isspace((unsigned char)s[len - 1])))
        s[--len] = '\0';
}

static bool bt_has_mac_pattern(const char *p, char sep)
{
    int i;

    for (i = 0; i < 17; i++)
    {
        if ((i % 3) == 2)
        {
            if (p[i] != sep)
                return false;
        }
        else if (!isxdigit((unsigned char)p[i]))
        {
            return false;
        }
    }

    return true;
}

static bool bt_extract_mac_from_line(const char *line, char *mac_out, size_t mac_out_len)
{
    size_t i, len;

    if (!line || mac_out_len < 18)
        return false;

    len = strlen(line);
    if (len < 17)
        return false;

    for (i = 0; i + 17 <= len; i++)
    {
        char sep = line[i + 2];
        if (sep != ':' && sep != '_')
            continue;
        if (!bt_has_mac_pattern(&line[i], sep))
            continue;

        snprintf(mac_out, mac_out_len, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
            toupper((unsigned char)line[i + 0]), toupper((unsigned char)line[i + 1]),
            toupper((unsigned char)line[i + 3]), toupper((unsigned char)line[i + 4]),
            toupper((unsigned char)line[i + 6]), toupper((unsigned char)line[i + 7]),
            toupper((unsigned char)line[i + 9]), toupper((unsigned char)line[i + 10]),
            toupper((unsigned char)line[i + 12]), toupper((unsigned char)line[i + 13]),
            toupper((unsigned char)line[i + 15]), toupper((unsigned char)line[i + 16]));
        return true;
    }

    return false;
}

static void bt_mac_to_underscore(const char *mac, char *out, size_t out_len)
{
    size_t i, n = 0;

    for (i = 0; mac[i] != '\0' && n + 1 < out_len; i++)
    {
        out[n++] = (mac[i] == ':') ? '_' : mac[i];
    }
    out[n] = '\0';
}

static int bt_add_device_unique_ex(struct bt_device *devices, int count, int max_devices,
    const char *mac, const char *name, bool paired)
{
    int i;

    if (!mac || !mac[0] || count >= max_devices)
        return count;

    for (i = 0; i < count; i++)
    {
        if (!strcasecmp(devices[i].mac, mac))
        {
            if (paired)
                devices[i].paired = true;
            if (name && name[0] &&
                (!devices[i].name[0] || !strcasecmp(devices[i].name, devices[i].mac)))
            {
                snprintf(devices[i].name, sizeof(devices[i].name), "%s", name);
            }
            return count;
        }
    }

    snprintf(devices[count].mac, sizeof(devices[count].mac), "%s", mac);
    if (name && name[0])
        snprintf(devices[count].name, sizeof(devices[count].name), "%s", name);
    else
        snprintf(devices[count].name, sizeof(devices[count].name), "%s", mac);
    devices[count].paired = paired;

    return count + 1;
}

static int bt_device_sort_cmp(const void *a, const void *b)
{
    const struct bt_device *da = a;
    const struct bt_device *db = b;

    if (da->paired != db->paired)
        return da->paired ? -1 : 1;

    return strcasecmp(da->name, db->name);
}

static int bt_sys_command(const char *command, char *reply, size_t reply_size)
{
    struct sockaddr_un addr;
    struct timeval tv_send = { .tv_sec = 1, .tv_usec = 0 };
    struct timeval tv_recv = { .tv_sec = 2, .tv_usec = 0 };
    int fd = -1;
    ssize_t n;

    if (!command || !command[0])
        return -1;

    if (reply && reply_size > 0)
        reply[0] = '\0';

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", BT_SYS_SOCKET);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    if (send(fd, command, strlen(command), 0) < 0)
    {
        close(fd);
        return -1;
    }

    if (reply && reply_size > 1)
    {
        n = recv(fd, reply, reply_size - 1, 0);
        if (n < 0)
        {
            close(fd);
            return -1;
        }

        reply[n] = '\0';
        bt_trim(reply);
    }

    close(fd);
    return 0;
}

static bool bt_sys_reply_ok(const char *reply, const char *command_prefix)
{
    char ok_reply[48];
    char wait_reply[48];

    if (!reply || !reply[0])
        return false;

    if (!strcasecmp(reply, "OK") || !strcasecmp(reply, "WAITINIT"))
        return true;

    if (strstr(reply, "FAIL"))
        return false;

    if (!command_prefix || !command_prefix[0])
        return false;

    snprintf(ok_reply, sizeof(ok_reply), "%s:OK", command_prefix);
    snprintf(wait_reply, sizeof(wait_reply), "%s:WAITINIT", command_prefix);

    if (strstr(reply, ok_reply))
        return true;
    if (strstr(reply, wait_reply))
        return true;

    return false;
}

static bool bt_json_get_string_value(const char *line, const char *key,
                                     char *out, size_t out_len)
{
    const char *start;
    const char *colon;
    const char *q1;
    const char *q2;
    size_t len;

    if (!line || !key || !out || out_len == 0)
        return false;

    start = strstr(line, key);
    if (!start)
        return false;

    colon = strchr(start, ':');
    if (!colon)
        return false;

    q1 = strchr(colon, '"');
    if (!q1)
        return false;
    q1++;

    q2 = strchr(q1, '"');
    if (!q2)
        return false;

    len = (size_t)(q2 - q1);
    if (len >= out_len)
        len = out_len - 1;
    memcpy(out, q1, len);
    out[len] = '\0';
    return true;
}

static bool bt_json_get_int_value(const char *line, const char *key, int *value)
{
    const char *start;
    const char *colon;
    const char *p;

    if (!line || !key || !value)
        return false;

    start = strstr(line, key);
    if (!start)
        return false;

    colon = strchr(start, ':');
    if (!colon)
        return false;

    p = colon + 1;
    while (*p == ' ' || *p == '\t')
        p++;

    if (!isdigit((unsigned char)*p) && *p != '-')
        return false;

    *value = atoi(p);
    return true;
}

static bool bt_get_file_stamp(const char *path, long *mtime, long *size)
{
    struct stat st;

    if (!path || !mtime || !size)
        return false;

    if (stat(path, &st) < 0)
        return false;

    *mtime = (long)st.st_mtime;
    *size = (long)st.st_size;
    return true;
}

static int bt_load_devices_from_json_file(const char *path,
                                          struct bt_device *devices,
                                          int max_devices,
                                          bool *ready)
{
    FILE *fp;
    char line[512];
    char mac[18] = "";
    char name[BT_NAME_LEN] = "";
    int paired = 0;
    int count = 0;
    bool has_device_key = false;

    if (ready)
        *ready = false;

    fp = fopen(path, "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "\"DEVICE\""))
            has_device_key = true;

        if (strstr(line, "\"MAC\""))
            bt_extract_mac_from_line(line, mac, sizeof(mac));

        if (bt_json_get_string_value(line, "\"Name\"", name, sizeof(name)))
            bt_trim(name);

        bt_json_get_int_value(line, "\"Paired\"", &paired);

        if (mac[0] && strchr(line, '}'))
        {
            count = bt_add_device_unique_ex(devices, count, max_devices,
                                            mac, name, paired != 0);
            mac[0] = '\0';
            name[0] = '\0';
            paired = 0;
        }
    }

    fclose(fp);

    if (ready)
        *ready = has_device_key;

    return count;
}

static int bt_load_devices_from_bt_list_file(struct bt_device *devices, int max_devices,
                                             bool *ready)
{
    return bt_load_devices_from_json_file(BT_LIST_FILE, devices, max_devices, ready);
}

static int bt_merge_devices_from_bt_scan_file(struct bt_device *devices, int count,
                                              int max_devices, bool *ready)
{
    static struct bt_device scanned[BT_MAX_DEVICES];
    int scanned_count;
    int i;

    scanned_count = bt_load_devices_from_json_file(BT_SCAN_FILE, scanned,
                                                   BT_MAX_DEVICES, ready);
    for (i = 0; i < scanned_count; i++)
    {
        count = bt_add_device_unique_ex(devices, count, max_devices,
                                        scanned[i].mac, scanned[i].name,
                                        scanned[i].paired);
    }

    return count;
}

static int bt_load_devices_via_sys_list(struct bt_device *devices, int max_devices)
{
    char reply[BT_SYS_REPLY_MAX];
    long old_mtime = 0;
    long old_size = 0;
    bool had_old_stamp = false;
    bool ready = false;
    bool changed = false;
    int count = 0;
    int waited = 0;

    had_old_stamp = bt_get_file_stamp(BT_LIST_FILE, &old_mtime, &old_size);
    if (bt_sys_command("BT:LIST", reply, sizeof(reply)) < 0)
        return 0;

    while (waited < HZ * 3)
    {
        long mtime = 0;
        long size = 0;

        count = bt_load_devices_from_bt_list_file(devices, max_devices, &ready);

        if (bt_get_file_stamp(BT_LIST_FILE, &mtime, &size))
        {
            if (!had_old_stamp || mtime != old_mtime || size != old_size)
                changed = true;
        }

        if (ready && (changed || waited >= HZ))
            break;

        sleep(HZ / 5);
        waited += HZ / 5;
    }

    if (count > 1)
        qsort(devices, count, sizeof(devices[0]), bt_device_sort_cmp);
    return count;
}

static int bt_scan_and_merge_devices(struct bt_device *devices, int count, int max_devices)
{
    char reply[BT_SYS_REPLY_MAX];
    long old_mtime = 0;
    long old_size = 0;
    bool had_old_stamp;
    bool changed = false;
    bool ready = false;
    int waited = 0;
    int post_waited = 0;

    had_old_stamp = bt_get_file_stamp(BT_SCAN_FILE, &old_mtime, &old_size);

    if (bt_sys_command("BT:SCAN", reply, sizeof(reply)) < 0 ||
        !bt_sys_reply_ok(reply, "BT:SCAN"))
        return count;

    while (waited < HZ * 8)
    {
        long mtime = 0;
        long size = 0;

        if (bt_get_file_stamp(BT_SCAN_FILE, &mtime, &size))
        {
            if (!had_old_stamp || mtime != old_mtime || size != old_size)
            {
                changed = true;
                break;
            }
        }

        sleep(HZ / 5);
        waited += HZ / 5;
    }

    bt_sys_command("BT:CANCEL_SCAN", reply, sizeof(reply));

    while (post_waited < HZ * 3)
    {
        long mtime = 0;
        long size = 0;

        if (bt_get_file_stamp(BT_SCAN_FILE, &mtime, &size))
        {
            if (!had_old_stamp || mtime != old_mtime || size != old_size)
                changed = true;
            if (changed)
            {
                count = bt_merge_devices_from_bt_scan_file(devices, count,
                                                           max_devices, &ready);
                if (ready)
                    break;
            }
        }

        sleep(HZ / 5);
        post_waited += HZ / 5;
    }

    if (!changed)
        count = bt_merge_devices_from_bt_scan_file(devices, count, max_devices, NULL);

    if (count > 1)
        qsort(devices, count, sizeof(devices[0]), bt_device_sort_cmp);
    return count;
}

static int bt_choose_device(const char *title, struct bt_device *devices, int count,
                            bool include_scan_item)
{
    struct bt_device_menu_data data;
    struct simplelist_info info;
    int total_count = count + (include_scan_item ? 1 : 0);

    if (total_count <= 0)
    {
        splash(HZ, "No devices");
        return BT_DEVICE_PICK_CANCEL;
    }

    data.devices = devices;
    data.count = count;
    data.include_scan_item = include_scan_item;

    simplelist_info_init(&info, (char *)title, total_count, &data);
    info.get_name = bt_device_name_cb;
    info.action_callback = bt_simplelist_ok_cancel;
    info.selection = -1;
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);
    if (info.selection < 0 || info.selection >= total_count)
        return BT_DEVICE_PICK_CANCEL;

    if (include_scan_item && info.selection == 0)
        return BT_DEVICE_PICK_SCAN;

    return include_scan_item ? info.selection - 1 : info.selection;
}

static void bt_set_selected_mac(const char *mac)
{
    if (mac && mac[0])
        snprintf(bt_selected_mac, sizeof(bt_selected_mac), "%s", mac);
    else
        bt_selected_mac[0] = '\0';
}

static void bt_kick_audio_if_playing(void)
{
    int status = audio_status();
    if ((status & AUDIO_STATUS_PLAY) && !(status & AUDIO_STATUS_PAUSE))
    {
        audio_pause();
        sleep(HZ / 4);
        audio_resume();
    }
}

static void bt_route_to_local(bool show_message)
{
    bt_playback_dev = BT_LOCAL_PLAYBACK_DEVICE;
    bt_active_codec[0] = '\0';
    hiby_pcm_set_bt_mac(NULL);
    pcm_alsa_switch_playback_device(bt_playback_dev);
    bt_kick_audio_if_playing();
    if (show_message)
        splash(HZ, "Output: Local");
}

static bool bt_route_to_bluetooth(const char *mac)
{
    int rc;

    if (!mac || !mac[0])
        return false;

    bt_playback_dev = bt_make_bt_playback_dev(mac);

    if (!bt_wait_for_bluealsa_pcm(mac, HZ * 6))
    {
        bt_route_to_local(false);
        return false;
    }

    bt_select_best_codec(mac);
    if (!bt_wait_for_bluealsa_pcm(mac, HZ * 3))
    {
        bt_route_to_local(false);
        return false;
    }

    rc = pcm_alsa_switch_playback_device(bt_playback_dev);
    if (rc == 0)
    {
        hiby_pcm_set_bt_mac(mac);
        bt_kick_audio_if_playing();
        return true;
    }

    bt_route_to_local(false);
    return false;
}

static bool bt_is_connected(const char *mac)
{
    if (!mac || !*mac)
        return false;

    return bt_bluealsa_pcm_ready(mac);
}

static bool bt_bluealsa_pcm_ready(const char *mac)
{
    char line[256];
    char mac_u[18];
    FILE *fp;

    if (!mac || !*mac)
        return false;

    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    fp = popen("bluealsa-cli list-pcms 2>/dev/null", "r");
    if (!fp)
        return false;

    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, mac_u) && strstr(line, "/a2dpsrc/sink"))
        {
            pclose(fp);
            return true;
        }
    }

    pclose(fp);
    return false;
}

static bool bt_get_active_mac(char *mac_out, size_t mac_out_len)
{
    FILE *fp;
    char line[256];

    if (!mac_out || mac_out_len < 18)
        return false;

    mac_out[0] = '\0';
    fp = popen("bluealsa-cli list-pcms 2>/dev/null", "r");
    if (fp)
    {
        while (fgets(line, sizeof(line), fp))
        {
            if (!strstr(line, "/a2dpsrc/sink"))
                continue;
            if (bt_extract_mac_from_line(line, mac_out, mac_out_len))
            {
                pclose(fp);
                return true;
            }
        }
        pclose(fp);
    }

    if (bt_selected_mac[0] && bt_is_connected(bt_selected_mac))
    {
        snprintf(mac_out, mac_out_len, "%s", bt_selected_mac);
        return true;
    }

    return false;
}

static bool bt_wait_for_bluealsa_pcm(const char *mac, int timeout_ticks)
{
    int ticks = 0;

    if (timeout_ticks < HZ / 2)
        timeout_ticks = HZ / 2;

    while (ticks < timeout_ticks)
    {
        if (bt_bluealsa_pcm_ready(mac))
            return true;

        sleep(HZ / 5);
        ticks += HZ / 5;
    }

    return false;
}

static bool bt_try_set_codec(const char *pcm_path, const char *codec)
{
    char cmd[256];
    int rc;

    snprintf(cmd, sizeof(cmd),
             "bluealsa-cli codec '%s' %s >/tmp/rb_bt_codec.log 2>&1",
             pcm_path, codec);
    rc = system(cmd);
    return (rc == 0);
}

static void bt_select_best_codec(const char *mac)
{
    char mac_u[18];
    char pcm_path[96];
    int i;

    if (!mac || !*mac)
        return;

    bt_active_codec[0] = '\0';
    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    snprintf(pcm_path, sizeof(pcm_path),
             "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", mac_u);

    for (i = 0; bt_codec_preference[i] != NULL; i++)
    {
        if (bt_try_set_codec(pcm_path, bt_codec_preference[i]))
        {
            snprintf(bt_active_codec, sizeof(bt_active_codec), "%s",
                     bt_codec_preference[i]);
            return;
        }
    }
}

static bool bt_prepare_stack(void)
{
    char reply[BT_SYS_REPLY_MAX];
    int i;

    system("/usr/bin/bt_enable >/tmp/rb_bt_enable.log 2>&1");

    for (i = 0; i < 12; i++)
    {
        reply[0] = '\0';
        if (bt_sys_command("BT:LIST", reply, sizeof(reply)) == 0 &&
            bt_sys_reply_ok(reply, "BT:LIST"))
            return true;

        reply[0] = '\0';
        bt_sys_command("BT:ON", reply, sizeof(reply));

        sleep(HZ / 5);
    }

    return false;
}

static void bt_show_devices(void)
{
    static struct bt_device devices[BT_MAX_DEVICES];
    int count;
    int idx;

    if (!bt_prepare_stack())
    {
        splash(HZ * 2, "BT unavailable");
        return;
    }

    splash(0, "Loading devices...");
    count = bt_load_devices_via_sys_list(devices, BT_MAX_DEVICES);
    if (count <= 0)
    {
        splash(0, "Scanning...");
        count = bt_scan_and_merge_devices(devices, count, BT_MAX_DEVICES);
    }

    while (1)
    {
        idx = bt_choose_device("Devices", devices, count, true);
        if (idx == BT_DEVICE_PICK_SCAN)
        {
            splash(0, "Scanning...");
            count = bt_scan_and_merge_devices(devices, count, BT_MAX_DEVICES);
            if (count <= 0)
                splash(HZ, "No devices");
            continue;
        }

        if (idx >= 0 && idx < count)
            bt_connect_device(&devices[idx]);
        return;
    }
}

static void bt_connect_device(const struct bt_device *device)
{
    const char *mac;
    char cmd[96];
    char reply[BT_SYS_REPLY_MAX];
    int ctl_rc;
    bool routed;
    bool connect_reply_ok;
    bool pair_reply_ok = true;

    if (!device || !device->mac[0])
        return;

    mac = device->mac;
    splash(0, "Connecting...");

    if (!bt_prepare_stack())
    {
        splash(HZ * 2, "BT unavailable");
        return;
    }

    if (!device->paired)
    {
        snprintf(cmd, sizeof(cmd), "BT:PAIR:%s", mac);
        ctl_rc = bt_sys_command(cmd, reply, sizeof(reply));
        pair_reply_ok = (ctl_rc == 0) && bt_sys_reply_ok(reply, "BT:PAIR");
        if (pair_reply_ok)
            sleep(HZ / 2);
    }

    snprintf(cmd, sizeof(cmd), "BT:CONNECT:%s", mac);
    ctl_rc = bt_sys_command(cmd, reply, sizeof(reply));
    connect_reply_ok = (ctl_rc == 0) && bt_sys_reply_ok(reply, "BT:CONNECT");

    if (!pair_reply_ok && !device->paired)
    {
        splash(HZ * 2, "BT pair failed");
        return;
    }

    if (!connect_reply_ok)
    {
        splash(HZ * 2, "BT connect failed");
        return;
    }

    bt_set_selected_mac(mac);

    routed = bt_route_to_bluetooth(mac);
    if (!routed)
    {
        snprintf(cmd, sizeof(cmd), "BT:CONNECT:%s", mac);
        ctl_rc = bt_sys_command(cmd, reply, sizeof(reply));
        if (ctl_rc == 0 && bt_sys_reply_ok(reply, "BT:CONNECT"))
        {
            sleep(HZ / 2);
            routed = bt_route_to_bluetooth(mac);
        }
    }

    if (routed)
        splash(HZ, "BT connected");
    else
        splash(HZ * 2, "BT connected, no audio route");
}

static void bt_disconnect(void)
{
    char mac[18];
    char cmd[96];
    char reply[BT_SYS_REPLY_MAX];

    mac[0] = '\0';
    if (!bt_get_active_mac(mac, sizeof(mac)) && bt_selected_mac[0])
        snprintf(mac, sizeof(mac), "%s", bt_selected_mac);

    if (mac[0])
    {
        snprintf(cmd, sizeof(cmd), "BT:DISCONNECT:%s", mac);
        bt_sys_command(cmd, reply, sizeof(reply));
    }

    bt_set_selected_mac(NULL);
    bt_active_codec[0] = '\0';
    bt_route_to_local(false);
    splash(HZ, "Disconnected");
}

static void bt_show_status(void)
{
    struct simplelist_info info;
    char active_mac[18];

    simplelist_info_init(&info, "Status", 0, NULL);
    simplelist_reset_lines();

    if (bt_get_active_mac(active_mac, sizeof(active_mac)))
    {
        simplelist_addline("Device: Bluetooth");
        simplelist_addline("MAC: %s", active_mac);
        simplelist_addline("Connected: %s",
                           bt_is_connected(active_mac) ? "Yes" : "No");
        simplelist_addline("A2DP PCM: %s",
                           bt_bluealsa_pcm_ready(active_mac) ? "Ready" : "Not ready");
        simplelist_addline("Codec: %s",
                           bt_active_codec[0] ? bt_active_codec : "Unknown");
    }
    else if (bt_selected_mac[0])
    {
        simplelist_addline("Device: Last selected");
        simplelist_addline("MAC: %s", bt_selected_mac);
        simplelist_addline("Connected: No");
    }
    else
    {
        simplelist_addline("Device: Local");
    }

    simplelist_addline("Output: %s", bt_playback_dev);

    info.count = simplelist_get_line_count();
    simplelist_show_list(&info);
}

int hiby_bluetooth_menu(void)
{
    static const char *const action_items[] =
    {
        "Status",
        "Devices",
        "Disconnect",
    };

    int action = -1;

    while (true)
    {
        struct simplelist_info info;

        simplelist_info_init(&info, "Bluetooth",
            (int)(sizeof(action_items) / sizeof(action_items[0])),
            (void *)action_items);
        info.get_name = bt_action_name_cb;
        info.action_callback = bt_simplelist_ok_cancel;
        info.selection = -1;
        info.title_icon = Icon_Submenu;

        simplelist_show_list(&info);
        action = info.selection;
        if (action < 0)
            break;

        switch (action)
        {
            case 0:
                bt_show_status();
                break;
            case 1:
                bt_show_devices();
                break;
            case 2:
                bt_disconnect();
                break;
            default:
                break;
        }
    }
    return 0;
}

#endif /* HIBY_LINUX */
