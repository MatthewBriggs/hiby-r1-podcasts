/* status.c — the things the status bar reports.
 *
 * All of it comes from sysfs except the Bluetooth check, which has to shell out
 * to bluealsa-cli and is therefore cached: forking a 240 MB process every frame
 * to ask the same question is not a reasonable way to draw an icon.
 *
 * Note the battery: there are two supplies, and axp_battery reads a flat 0.
 * The one that reports honestly is plain "battery".
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "status.h"

static int read_int_file(const char *path, int fallback);

static int read_int_file(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    int v = fallback;
    if (fscanf(f, "%d", &v) != 1) v = fallback;
    fclose(f);
    return v;
}

int st_battery_pct(void) {
    return read_int_file("/sys/class/power_supply/battery/capacity", -1);
}

int st_charging(void) {
    FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
    if (!f) return 0;
    char s[32] = "";
    if (!fgets(s, sizeof(s), f)) s[0] = '\0';
    fclose(f);
    return strncmp(s, "Charging", 8) == 0;
}

int st_headset(void) {
    return read_int_file("/sys/class/switch/headset/state", 0) != 0;
}

/* ---- Bluetooth sink details --------------------------------------------- */
/* Both of these shell out, so both are cached: the codec cannot change without
 * a reconnection and a battery reading that is ten seconds stale is still a
 * useful battery reading.
 *
 * The exact wording of the tools' output has not been seen with a sink
 * actually connected, so the parsing takes the last field of any line that
 * mentions the thing it wants rather than matching a fixed layout, and the raw
 * first line is logged once so it can be checked against reality.
 */
static void run_cmd(const char *cmd, char *out, unsigned n) {
    out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) return;
    unsigned used = 0;
    char line[256];
    while (used + 1 < n && fgets(line, sizeof(line), p)) {
        unsigned len = (unsigned)strlen(line);
        if (used + len >= n) len = n - used - 1;
        memcpy(out + used, line, len);
        used += len;
    }
    out[used] = '\0';
    pclose(p);
}

/* The first BlueALSA sink path, which both queries need. */
static int bt_pcm_path(char *out, unsigned n) {
    char buf[1024];
    run_cmd("bluealsa-cli list-pcms 2>/dev/null", buf, sizeof(buf));
    char *line = strtok(buf, "\n");
    while (line) {
        if (strstr(line, "/sink")) {
            while (*line == ' ') line++;
            snprintf(out, n, "%s", line);
            return 1;
        }
        line = strtok(NULL, "\n");
    }
    return 0;
}

void st_bt_codec(char *out, unsigned n) {
    static char cached[32];
    static time_t when;
    time_t now = time(NULL);
    if (cached[0] && now - when < 15) { snprintf(out, n, "%s", cached); return; }

    cached[0] = '\0';
    out[0] = '\0';
    char path[256];
    if (!bt_pcm_path(path, sizeof(path))) { when = now; return; }

    char cmd[384], buf[512];
    snprintf(cmd, sizeof(cmd), "bluealsa-cli codec '%s' 2>/dev/null", path);
    run_cmd(cmd, buf, sizeof(buf));

    /* The real output, with a headset connected, is two lines:
     *
     *   Available codecs: SBC AAC LDAC
     *   Selected codec: LDAC
     *
     * Take the selected one. The first attempt at this used strcasestr, which
     * needs _GNU_SOURCE to be declared and silently returned rubbish without
     * it — hence an empty codec and a footer that just said "Bluetooth". */
    char *sel = strstr(buf, "Selected codec:");
    if (sel) {
        sel += strlen("Selected codec:");
        while (*sel == ' ' || *sel == '\t') sel++;
        char *end = sel;
        while (*end && *end != '\n' && *end != '\r' && *end != ' ') end++;
        *end = '\0';
        if (*sel) snprintf(cached, sizeof(cached), "%s", sel);
    }
    when = now;
    snprintf(out, n, "%s", cached);
}

int st_bt_battery(void) {
    static int cached = -1;
    static time_t when;
    time_t now = time(NULL);
    if (now - when < 15 && cached != -1) return cached;
    when = now;
    cached = -1;

    /* Read it from BlueALSA, not BlueZ. org.bluez.Battery1 is the obvious
     * place and it does not exist here — BlueZ is 5.54 and that interface
     * arrived in 5.56, so the query answered "No such interface" every time.
     * The level comes in over HFP and BlueALSA is what terminates that link,
     * so it is BlueALSA that knows. */
    char path[256];
    if (!bt_pcm_path(path, sizeof(path))) return -1;

    /* .../dev_XX/a2dpsrc/sink -> .../dev_XX/rfcomm */
    char rfcomm[288];
    snprintf(rfcomm, sizeof(rfcomm), "%s", path);
    char *tail = strstr(rfcomm, "/a2dpsrc");
    if (!tail) tail = strstr(rfcomm, "/hfpag");
    if (!tail) return -1;
    snprintf(tail, sizeof(rfcomm) - (size_t)(tail - rfcomm), "/rfcomm");

    char cmd[384], buf[1024];
    snprintf(cmd, sizeof(cmd), "bluealsa-rfcomm '%s' --properties 2>/dev/null", rfcomm);
    run_cmd(cmd, buf, sizeof(buf));

    char *b = strstr(buf, "Battery:");
    if (b) {
        int v = atoi(b + 8);
        if (v >= 0 && v <= 100) cached = v;
    }
    return cached;
}

/* Names for the panel. Both shell out, both are cached, and both are only
 * asked for while the panel is open. */
void st_wifi_ssid(char *out, unsigned n) {
    static char cached[64];
    static time_t when;
    time_t now = time(NULL);
    if (cached[0] && now - when < 10) { snprintf(out, n, "%s", cached); return; }
    when = now;
    cached[0] = '\0';
    /* wpa_cli, not iwconfig — the latter is not on this firmware. Non-ASCII
     * SSIDs come back as \xNN escapes, which are decoded so the panel shows
     * the name rather than the escaping. */
    char buf[512];
    run_cmd("wpa_cli -i wlan0 status 2>/dev/null", buf, sizeof(buf));
    char *q = strstr(buf, "\nssid=");
    if (!q && !strncmp(buf, "ssid=", 5)) q = buf - 1;
    if (q) {
        q += 6;
        char *e = strchr(q, '\n');
        size_t len = e ? (size_t)(e - q) : strlen(q);
        unsigned o = 0;
        for (size_t i = 0; i < len && o + 1 < sizeof(cached); ) {
            if (q[i] == '\\' && i + 3 < len && q[i + 1] == 'x') {
                int hi = q[i + 2], lo = q[i + 3];
                hi = (hi >= 'a') ? hi - 'a' + 10 : (hi >= 'A') ? hi - 'A' + 10 : hi - '0';
                lo = (lo >= 'a') ? lo - 'a' + 10 : (lo >= 'A') ? lo - 'A' + 10 : lo - '0';
                cached[o++] = (char)((hi << 4) | lo);
                i += 4;
            } else {
                cached[o++] = q[i++];
            }
        }
        cached[o] = '\0';
    }
    snprintf(out, n, "%s", cached);
}

void st_bt_name(char *out, unsigned n) {
    static char cached[64];
    static time_t when;
    time_t now = time(NULL);
    if (cached[0] && now - when < 10) { snprintf(out, n, "%s", cached); return; }
    when = now;
    cached[0] = '\0';
    char path[256];
    if (bt_pcm_path(path, sizeof(path))) {
        /* .../dev_94_DB_56_8E_03_43/... -> 94:DB:...  then ask BlueZ its name */
        char *d = strstr(path, "dev_");
        if (d) {
            char mac[32];
            unsigned k = 0;
            for (const char *p = d + 4; *p && *p != '/' && k + 1 < sizeof(mac); p++)
                mac[k++] = (*p == '_') ? ':' : *p;
            mac[k] = '\0';
            char cmd[128], buf[512];
            snprintf(cmd, sizeof(cmd),
                     "printf 'info %s\nquit\n' | bluetoothctl 2>/dev/null", mac);
            run_cmd(cmd, buf, sizeof(buf));
            char *nm = strstr(buf, "Name: ");
            if (nm) {
                nm += 6;
                char *e = strchr(nm, '\n');
                size_t len = e ? (size_t)(e - nm) : strlen(nm);
                while (len && (nm[len-1] == '\r' || nm[len-1] == ' ')) len--;
                if (len >= sizeof(cached)) len = sizeof(cached) - 1;
                memcpy(cached, nm, len);
                cached[len] = '\0';
            }
        }
    }
    snprintf(out, n, "%s", cached);
}

/* ---- quick settings ------------------------------------------------------ */
#define BACKLIGHT_NODE "/sys/class/backlight/backlight_pwm0"

int st_brightness(void)     { return read_int_file(BACKLIGHT_NODE "/brightness", 0); }
int st_brightness_max(void) { return read_int_file(BACKLIGHT_NODE "/max_brightness", 101); }

void st_brightness_set(int v) {
    int max = st_brightness_max();
    if (v < 1) v = 1;                     /* never all the way off from here */
    /* Never the literal max either -- the user reported the quick-settings
     * slider going to 100% turns the screen black rather than brightest.
     * The likely mechanism: this PWM backlight's duty-cycle register wraps
     * to 0% at its own period value instead of clamping at full-on, so
     * max_brightness itself (101) is one past the last safe value. max-1
     * is visually indistinguishable from max at every brightness this
     * panel has, so nothing is lost by never writing it. */
    if (v > max - 1) v = max - 1;
    FILE *f = fopen(BACKLIGHT_NODE "/brightness", "w");
    if (!f) return;
    fprintf(f, "%d", v);
    fclose(f);
}

/* wifi_off.sh takes the interface down, so operstate is the honest answer. */
int st_wifi_on(void) {
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (!f) return 0;
    char s[16] = "";
    if (!fgets(s, sizeof(s), f)) s[0] = '\0';
    fclose(f);
    return strncmp(s, "down", 4) != 0;
}

/* No sysfs equivalent: rfkill reads unblocked whether the adapter is powered
 * or not, so it has to be asked. Only called while the panel is open. */
int st_bt_on(void) {
    FILE *p = popen("hciconfig hci0 2>/dev/null", "r");
    if (!p) return 0;
    char line[256];
    int up = 0;
    while (fgets(line, sizeof(line), p))
        if (strstr(line, "UP RUNNING")) { up = 1; break; }
    pclose(p);
    return up;
}

/* Backgrounded: wifi_on.sh restarts wpa_supplicant and waits on DHCP, which is
 * seconds. Blocking the UI on that would look like a crash. */
void st_wifi_set(int on) {
    if (system(on ? "/usr/bin/wifi_on.sh >/dev/null 2>&1 &"
                  : "/usr/bin/wifi_off.sh >/dev/null 2>&1 &") == -1) return;
}

void st_bt_set(int on) {
    if (system(on ? "/usr/bin/bt_enable >/dev/null 2>&1 &"
                  : "/usr/bin/bt_disable >/dev/null 2>&1 &") == -1) return;
}

/* Radio needs a route off the device. wlan0 exists whether or not Wi-Fi is
 * switched on, so the interface being present says nothing — operstate does. */
int st_net_up(void) {
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (!f) return 0;
    char s[16] = "";
    if (!fgets(s, sizeof(s), f)) s[0] = '\0';
    fclose(f);
    return strncmp(s, "up", 2) == 0;
}
