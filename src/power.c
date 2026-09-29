/*
 * power.c - telemetry data layer for PWR-REACTOR.
 *
 * Toolkit independent: discovers every battery the machine can see,
 * keeps a rolling charge history, estimates time to empty/full and
 * raises low battery alerts. The UI layer reads the model through
 * power.h and never touches these internals directly.
 *
 * Sources, in priority order: upower, /sys/class/power_supply, adb
 * (Android over USB), KDE Connect, GSConnect, NUT (upsc).
 */

#include "power.h"

#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYSFS_PS "/sys/class/power_supply"

/* ------------------------------------------------------------------ */
/* config - ~/.config/power-reactor.conf, created with defaults on     */
/* first run. key=value lines, # comments.                             */
/* ------------------------------------------------------------------ */

Config g_cfg = {2000, 15, 5, 1, 1, 1, 0, 1, 0};

/* step over leading blanks without ever passing the terminator */
static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* range-checked strtol for every number that comes from an external
 * process, a device or the config file. atoi overflow is UB. */
static long parse_long(const char *s, long lo, long hi, long def)
{
    char *end;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE)
        return def;
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void power_config_path(char *out, unsigned long n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(out, n, "%s/power-reactor.conf", xdg);
    else
        snprintf(out, n, "%s/.config/power-reactor.conf",
                 getenv("HOME") ? getenv("HOME") : ".");
}

static void config_write_default(const char *path)
{
    /* O_EXCL: never follow or clobber something that appeared between
     * the failed read and this write */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        return;
    }
    fprintf(f,
        "# PWR-REACTOR configuration\n"
        "# telemetry rescan interval in milliseconds\n"
        "scan_ms=2000\n"
        "# notification thresholds (percent)\n"
        "warn_pct=15\n"
        "crit_pct=5\n"
        "# raise the panel when a new device battery is plugged in\n"
        "popup_on_plug=1\n"
        "# desktop notifications on low battery\n"
        "notify=1\n"
        "# alert sound (freedesktop sound theme via paplay)\n"
        "sound=1\n"
        "# force the dark theme (0 = follow the desktop setting)\n"
        "dark=0\n"
        "# show lowest device percentage next to the tray icon\n"
        "tray_label=1\n"
        "# start in the compact window size\n"
        "compact=0\n");
    fclose(f);
}

void power_config_load(void)
{
    char path[512], line[128];
    FILE *f;
    power_config_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f) {
        config_write_default(path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq, *key, *val, *end;
        line[strcspn(line, "\r\n")] = 0;
        key = (char *)skip_spaces(line);
        if (key[0] == '#' || !key[0])
            continue;
        eq = strchr(key, '=');
        if (!eq)
            continue;
        *eq = 0;
        val = (char *)skip_spaces(eq + 1);
        /* trim trailing blanks from the key so "scan_ms = 2000" works */
        for (end = eq - 1; end >= key && (*end == ' ' || *end == '\t');)
            *end-- = 0;
        if (!strcmp(key, "scan_ms"))
            g_cfg.scan_ms = (int)parse_long(val, 500, 600000, 2000);
        else if (!strcmp(key, "warn_pct"))
            g_cfg.warn_pct = (int)parse_long(val, 0, 100, 15);
        else if (!strcmp(key, "crit_pct"))
            g_cfg.crit_pct = (int)parse_long(val, 0, 100, 5);
        else if (!strcmp(key, "popup_on_plug"))
            g_cfg.popup_on_plug = (int)parse_long(val, 0, 1, 1);
        else if (!strcmp(key, "notify"))
            g_cfg.notify = (int)parse_long(val, 0, 1, 1);
        else if (!strcmp(key, "sound"))
            g_cfg.sound = (int)parse_long(val, 0, 1, 1);
        else if (!strcmp(key, "dark"))
            g_cfg.dark = (int)parse_long(val, 0, 1, 0);
        else if (!strcmp(key, "tray_label"))
            g_cfg.tray_label = (int)parse_long(val, 0, 1, 1);
        else if (!strcmp(key, "compact"))
            g_cfg.compact = (int)parse_long(val, 0, 1, 0);
    }
    fclose(f);
    if (g_cfg.crit_pct > g_cfg.warn_pct)
        g_cfg.crit_pct = g_cfg.warn_pct;
}

/* ------------------------------------------------------------------ */
/* device model + history                                              */
/* ------------------------------------------------------------------ */

Dev   g_devs[MAX_DEVS];
int   g_ndevs = 0;
int   g_upower_link = 0;

Hist  g_hist[MAX_DEVS];
int   g_nhist = 0;
float g_pwr_now = 0.0f;

static void hist_push(const char *label, int cap)
{
    int i;
    Hist *h = NULL;
    for (i = 0; i < g_nhist; i++)
        if (!strcmp(g_hist[i].label, label)) {
            h = &g_hist[i];
            break;
        }
    if (!h) {
        if (g_nhist < MAX_DEVS) {
            h = &g_hist[g_nhist++];
        } else {
            /* full: recycle a slot whose device is gone rather than
             * refusing history for every device seen from now on */
            for (i = 0; i < g_nhist; i++)
                if (!g_hist[i].active) {
                    h = &g_hist[i];
                    break;
                }
            if (!h)
                return;
        }
        memset(h, 0, sizeof *h);
        snprintf(h->label, sizeof h->label, "%.27s", label);
    }
    h->cap[h->head] = (signed char)cap;
    h->head = (h->head + 1) % HIST_N;
    if (h->used < HIST_N)
        h->used++;
    h->active = 1;
}

/* ------------------------------------------------------------------ */
/* scanning (upower primary, sysfs fallback)                           */
/* ------------------------------------------------------------------ */

static void str_upper(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z')
            *s -= 32;
}

/* qsort is not stable, so ties are broken by name: without this two
 * devices of the same kind can swap places between scans and force the
 * UI to rebuild every row */
static int dev_cmp(const void *a, const void *b)
{
    const Dev *x = a, *y = b;
    if (x->kind != y->kind)
        return x->kind - y->kind;
    return strcmp(x->label, y->label);
}

typedef struct {
    char path[128];
    char model[64];
    char category[24];
    char state[24];
    char icon[64];
    int  percent;
    int  online;
    int  psupply;
    double volt;
    double rate_w;
    int  tte_min;
    int  ttf_min;
    int  active;
} UBlock;

/* "3.5 hours" / "42.0 minutes" -> minutes, clamped to a sane range */
static int parse_duration_min(const char *v)
{
    double d = atof(v);
    int m = -1;
    if (d < 0 || d > 1e6)
        return -1;
    if (strstr(v, "hour"))
        m = (int)(d * 60);
    else if (strstr(v, "minute"))
        m = (int)d;
    else if (strstr(v, "second"))
        m = (int)(d / 60);
    return m > 100000 ? 100000 : m;
}

static void ublock_reset(UBlock *u)
{
    memset(u, 0, sizeof *u);
    u->percent = -1;
    u->online = -1;
    u->psupply = -1;
    u->volt = -1;
    u->rate_w = -1;
    u->tte_min = -1;
    u->ttf_min = -1;
}

static void ublock_flush(const UBlock *u)
{
    Dev *dv;
    if (!u->active || g_ndevs >= MAX_DEVS)
        return;
    if (strstr(u->path, "DisplayDevice"))
        return;
    if (!u->category[0])
        return;

    dv = &g_devs[g_ndevs];
    memset(dv, 0, sizeof *dv);
    dv->capacity = u->percent;
    dv->online = u->online;
    dv->voltage_uv = (u->volt >= 0 && u->volt < 1000)
                   ? (long)(u->volt * 1e6) : -1;
    dv->power_uw = (u->rate_w >= 0 && u->rate_w < 10000)
                 ? (long)(u->rate_w * 1e6) : -1;
    dv->est_min = -1;
    if (u->tte_min > 0) {
        dv->est_min = u->tte_min;
        dv->est_ttf = 0;
    } else if (u->ttf_min > 0) {
        dv->est_min = u->ttf_min;
        dv->est_ttf = 1;
    }
    strcpy(dv->status, "---");

    if (!strcmp(u->category, "line-power")) {
        dv->kind = KIND_MAINS;
        strcpy(dv->tag, "AC LINE");
    } else if (!strcmp(u->category, "battery") && u->psupply == 1) {
        dv->kind = KIND_SYSBAT;
        strcpy(dv->tag, "MAIN BAT");
    } else {
        dv->kind = KIND_DEVBAT;
        snprintf(dv->tag, sizeof dv->tag, "%.11s", u->category);
        str_upper(dv->tag);
    }

    if (u->model[0]) {
        snprintf(dv->label, sizeof dv->label, "%.27s", u->model);
    } else {
        const char *b = strrchr(u->path, '/');
        snprintf(dv->label, sizeof dv->label, "%.27s", b ? b + 1 : u->path);
    }
    dv->label[24] = 0;

    if (!strcmp(u->state, "charging"))            strcpy(dv->status, "CHG");
    else if (!strcmp(u->state, "discharging"))    strcpy(dv->status, "DIS");
    else if (!strcmp(u->state, "fully-charged"))  strcpy(dv->status, "FUL");
    else if (!strcmp(u->state, "pending-charge")) strcpy(dv->status, "IDL");
    else if (strstr(u->icon, "charging"))         strcpy(dv->status, "CHG");

    g_ndevs++;
}

static int scan_upower(void)
{
    FILE *p = popen("LC_ALL=C timeout 2 upower --dump 2>/dev/null </dev/null", "r");
    char line[256];
    UBlock u;

    if (!p)
        return 0;
    ublock_reset(&u);
    g_ndevs = 0;

    while (fgets(line, sizeof line, p)) {
        char *s = line;
        char *colon;
        line[strcspn(line, "\n")] = 0;

        if (!strncmp(line, "Device:", 7)) {
            ublock_flush(&u);
            ublock_reset(&u);
            u.active = 1;
            s = line + 7;
            while (*s == ' ')
                s++;
            snprintf(u.path, sizeof u.path, "%.127s", s);
            continue;
        }
        if (!strncmp(line, "Daemon:", 7)) {
            ublock_flush(&u);
            u.active = 0;
            break;
        }
        if (!u.active)
            continue;

        while (*s == ' ' || *s == '\t')
            s++;
        colon = strchr(s, ':');
        if (!colon) {
            if (!u.category[0] && *s && !strpbrk(s, "0123456789"))
                snprintf(u.category, sizeof u.category, "%.23s", s);
            continue;
        }
        {
            char key[40];
            char *val = colon + 1;
            size_t klen = (size_t)(colon - s);
            if (klen >= sizeof key)
                continue;
            memcpy(key, s, klen);
            key[klen] = 0;
            while (klen && key[klen - 1] == ' ')
                key[--klen] = 0;
            while (*val == ' ' || *val == '\t')
                val++;

            if (!strcmp(key, "model"))
                snprintf(u.model, sizeof u.model, "%.63s", val);
            else if (!strcmp(key, "native-path") && !u.path[0])
                snprintf(u.path, sizeof u.path, "%.127s", val);
            else if (!strcmp(key, "percentage"))
                u.percent = (int)parse_long(val, -1, 100, -1);
            else if (!strcmp(key, "state"))
                snprintf(u.state, sizeof u.state, "%.23s", val);
            else if (!strcmp(key, "icon-name"))
                snprintf(u.icon, sizeof u.icon, "%.63s", val);
            else if (!strcmp(key, "voltage"))
                u.volt = atof(val);
            else if (!strcmp(key, "energy-rate"))
                u.rate_w = atof(val);
            else if (!strcmp(key, "online"))
                u.online = !strncmp(val, "yes", 3);
            else if (!strcmp(key, "power supply"))
                u.psupply = !strncmp(val, "yes", 3);
            else if (!strcmp(key, "time to empty"))
                u.tte_min = parse_duration_min(val);
            else if (!strcmp(key, "time to full"))
                u.ttf_min = parse_duration_min(val);
        }
    }
    ublock_flush(&u);
    pclose(p);
    qsort(g_devs, (size_t)g_ndevs, sizeof(Dev), dev_cmp);
    return g_ndevs;
}

static int read_sysfs_str(const char *dev, const char *attr, char *out,
                          size_t n)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, SYSFS_PS "/%s/%s", dev, attr);
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}

static long read_sysfs_long(const char *dev, const char *attr)
{
    char buf[64];
    if (read_sysfs_str(dev, attr, buf, sizeof buf) != 0)
        return -1;
    return atol(buf);
}

static void scan_sysfs(void)
{
    DIR *d = opendir(SYSFS_PS);
    struct dirent *e;
    char buf[64];

    g_ndevs = 0;
    if (!d)
        return;

    while ((e = readdir(d)) && g_ndevs < MAX_DEVS) {
        Dev *dv;
        if (e->d_name[0] == '.')
            continue;
        dv = &g_devs[g_ndevs];
        memset(dv, 0, sizeof *dv);
        dv->capacity = -1;
        dv->online = -1;
        dv->voltage_uv = -1;
        dv->power_uw = -1;
        dv->est_min = -1;
        strcpy(dv->status, "---");

        if (read_sysfs_str(e->d_name, "type", buf, sizeof buf) != 0)
            continue;

        if (!strcmp(buf, "Mains")) {
            dv->kind = KIND_MAINS;
            strcpy(dv->tag, "AC LINE");
        } else if (!strcmp(buf, "Battery")) {
            dv->kind = KIND_SYSBAT;
            strcpy(dv->tag, "MAIN BAT");
            if (read_sysfs_str(e->d_name, "scope", buf, sizeof buf) == 0 &&
                !strcmp(buf, "Device")) {
                dv->kind = KIND_DEVBAT;
                strcpy(dv->tag, "USB DEV");
            }
        } else {
            dv->kind = KIND_DEVBAT;
            strcpy(dv->tag, "USB");
        }

        if (read_sysfs_str(e->d_name, "model_name", dv->label,
                           sizeof dv->label) != 0 || !dv->label[0])
            snprintf(dv->label, sizeof dv->label, "%.27s", e->d_name);
        dv->label[24] = 0;

        dv->capacity = (int)read_sysfs_long(e->d_name, "capacity");
        if (dv->capacity > 100)
            dv->capacity = 100;
        dv->online = (int)read_sysfs_long(e->d_name, "online");
        dv->voltage_uv = read_sysfs_long(e->d_name, "voltage_now");
        {
            long cur = read_sysfs_long(e->d_name, "current_now");
            long pow = read_sysfs_long(e->d_name, "power_now");
            if (pow >= 0)
                dv->power_uw = pow;
            else if (cur >= 0 && dv->voltage_uv >= 0)
                dv->power_uw = (long)((double)cur * dv->voltage_uv / 1e6);
        }
        if (read_sysfs_str(e->d_name, "status", buf, sizeof buf) == 0) {
            if (!strcmp(buf, "Charging"))          strcpy(dv->status, "CHG");
            else if (!strcmp(buf, "Discharging"))  strcpy(dv->status, "DIS");
            else if (!strcmp(buf, "Full"))         strcpy(dv->status, "FUL");
            else if (!strcmp(buf, "Not charging")) strcpy(dv->status, "IDL");
        }
        g_ndevs++;
    }
    closedir(d);
    qsort(g_devs, (size_t)g_ndevs, sizeof(Dev), dev_cmp);
}

static Dev *dev_append(int kind, const char *tag, const char *label);

/* ---------------- Android via adb ---------------------------------- */

/* serials are embedded in a shell command - allow a safe charset only */
static int serial_ok(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '.' || *s == ':' ||
              *s == '-' || *s == '_'))
            return 0;
    return 1;
}

static void scan_adb(void)
{
    FILE *p;
    char line[256];
    char serial[8][64];
    char model[8][28];
    int n = 0, i;

    p = popen("timeout 2 adb devices -l 2>/dev/null </dev/null", "r");
    if (!p)
        return;
    while (fgets(line, sizeof line, p) && n < 8) {
        char *tok, *state, *m;
        line[strcspn(line, "\n")] = 0;
        if (strstr(line, "List of devices"))
            continue;
        tok = strtok(line, " \t");
        if (!tok)
            continue;
        state = strtok(NULL, " \t");
        if (!state || strcmp(state, "device"))
            continue;
        snprintf(serial[n], sizeof serial[0], "%.63s", tok);
        model[n][0] = 0;
        while ((m = strtok(NULL, " \t")))
            if (!strncmp(m, "model:", 6))
                snprintf(model[n], sizeof model[0], "%.27s", m + 6);
        n++;
    }
    pclose(p);

    for (i = 0; i < n && g_ndevs < MAX_DEVS; i++) {
        Dev *dv;
        char cmd[160];
        int level = -1, status = -1;
        long volt_mv = -1;

        if (!serial_ok(serial[i]))
            continue;
        snprintf(cmd, sizeof cmd,
                 "timeout 2 adb -s %.63s shell dumpsys battery "
                 "2>/dev/null </dev/null",
                 serial[i]);
        p = popen(cmd, "r");
        if (!p)
            continue;
        while (fgets(line, sizeof line, p)) {
            char *s = line;
            while (*s == ' ')
                s++;
            if (!strncmp(s, "level:", 6))
                level = (int)parse_long(s + 6, -1, 100, -1);
            else if (!strncmp(s, "status:", 7))
                status = (int)parse_long(s + 7, -1, 10, -1);
            else if (!strncmp(s, "voltage:", 8))
                volt_mv = parse_long(s + 8, -1, 1000000, -1);
        }
        pclose(p);
        if (level < 0)
            continue;

        {
            char name[28];
            if (model[i][0]) {
                char *c;
                snprintf(name, sizeof name, "%.27s", model[i]);
                for (c = name; *c; c++)
                    if (*c == '_')
                        *c = ' ';
            } else {
                snprintf(name, sizeof name, "ADB %.20s", serial[i]);
            }
            dv = dev_append(KIND_DEVBAT, "ANDROID", name);
        }
        if (!dv)
            continue;
        dv->capacity = level > 100 ? 100 : level;
        dv->voltage_uv = volt_mv > 0 ? volt_mv * 1000 : -1;
        switch (status) { /* android BatteryManager constants */
        case 2:  strcpy(dv->status, "CHG"); break;
        case 3:  strcpy(dv->status, "DIS"); break;
        case 4:  strcpy(dv->status, "IDL"); break;
        case 5:  strcpy(dv->status, "FUL"); break;
        default: break;
        }
    }
}

/* ---------------- shared helpers for extra sources ----------------- */

static int label_exists(const char *label)
{
    int i;
    for (i = 0; i < g_ndevs; i++)
        if (!strcasecmp(g_devs[i].label, label))
            return 1;
    return 0;
}

static Dev *dev_append(int kind, const char *tag, const char *label)
{
    Dev *dv;
    char clean[28];
    if (g_ndevs >= MAX_DEVS)
        return NULL;
    snprintf(clean, sizeof clean, "%.24s", label);
    if (label_exists(clean))
        return NULL;
    dv = &g_devs[g_ndevs++];
    memset(dv, 0, sizeof *dv);
    dv->kind = kind;
    snprintf(dv->tag, sizeof dv->tag, "%.11s", tag);
    snprintf(dv->label, sizeof dv->label, "%s", clean);
    dv->capacity = -1;
    dv->online = -1;
    dv->voltage_uv = -1;
    dv->power_uw = -1;
    dv->est_min = -1;
    strcpy(dv->status, "---");
    return dv;
}

/* first int in a gvariant-ish string, e.g. "(<int32 85>,)" -> 85 */
static int gv_int(const char *s)
{
    while (*s && (*s < '0' || *s > '9'))
        s++;
    return *s ? (int)parse_long(s, -1, 1000000, -1) : -1;
}

/* first 'quoted' string into out */
static int gv_str(const char *s, char *out, size_t n)
{
    const char *a = strchr(s, '\''), *b;
    if (!a)
        return -1;
    b = strchr(a + 1, '\'');
    if (!b)
        return -1;
    if ((size_t)(b - a - 1) >= n)
        return -1;
    memcpy(out, a + 1, (size_t)(b - a - 1));
    out[b - a - 1] = 0;
    return 0;
}

/* ---------------- KDE Connect (phone battery over wifi) ------------ */

static void scan_kdeconnect(void)
{
    FILE *p;
    char buf[2048];
    char ids[8][64];
    int nids = 0, i;
    size_t got;

    p = popen("timeout 2 gdbus call --session --dest org.kde.kdeconnect "
              "--object-path /modules/kdeconnect "
              "--method org.kde.kdeconnect.daemon.devices "
              "true true 2>/dev/null </dev/null", "r");
    if (!p)
        return;
    got = fread(buf, 1, sizeof buf - 1, p);
    buf[got] = 0;
    pclose(p);

    {
        char *s = buf;
        while (nids < 8) {
            char *a = strchr(s, '\''), *b;
            if (!a)
                break;
            b = strchr(a + 1, '\'');
            if (!b)
                break;
            if ((size_t)(b - a - 1) < sizeof ids[0]) {
                memcpy(ids[nids], a + 1, (size_t)(b - a - 1));
                ids[nids][b - a - 1] = 0;
                if (serial_ok(ids[nids]))
                    nids++;
            }
            s = b + 1;
        }
    }

    for (i = 0; i < nids; i++) {
        char cmd[320], name[64];
        int charge, charging;
        Dev *dv;

        snprintf(cmd, sizeof cmd,
                 "timeout 2 gdbus call --session --dest org.kde.kdeconnect "
                 "--object-path /modules/kdeconnect/devices/%.63s "
                 "--method org.freedesktop.DBus.Properties.Get "
                 "org.kde.kdeconnect.device.battery charge 2>/dev/null",
                 ids[i]);
        p = popen(cmd, "r");
        if (!p)
            continue;
        got = fread(buf, 1, sizeof buf - 1, p);
        buf[got] = 0;
        pclose(p);
        charge = gv_int(buf);
        if (charge < 0 || charge > 100)
            continue;

        snprintf(cmd, sizeof cmd,
                 "timeout 2 gdbus call --session --dest org.kde.kdeconnect "
                 "--object-path /modules/kdeconnect/devices/%.63s "
                 "--method org.freedesktop.DBus.Properties.Get "
                 "org.kde.kdeconnect.device.battery isCharging 2>/dev/null",
                 ids[i]);
        p = popen(cmd, "r");
        if (!p)
            continue;
        got = fread(buf, 1, sizeof buf - 1, p);
        buf[got] = 0;
        pclose(p);
        charging = strstr(buf, "true") != NULL;

        snprintf(cmd, sizeof cmd,
                 "timeout 2 gdbus call --session --dest org.kde.kdeconnect "
                 "--object-path /modules/kdeconnect/devices/%.63s "
                 "--method org.freedesktop.DBus.Properties.Get "
                 "org.kde.kdeconnect.device name 2>/dev/null", ids[i]);
        p = popen(cmd, "r");
        name[0] = 0;
        if (p) {
            got = fread(buf, 1, sizeof buf - 1, p);
            buf[got] = 0;
            pclose(p);
            gv_str(buf, name, sizeof name);
        }
        if (!name[0])
            snprintf(name, sizeof name, "KDECONN %.16s", ids[i]);

        dv = dev_append(KIND_DEVBAT, "KDECONN", name);
        if (!dv)
            continue;
        dv->capacity = charge;
        strcpy(dv->status, charging ? "CHG" : "DIS");
    }
}

/* ---------------- GSConnect (GNOME shell extension) ----------------- */

static void scan_gsconnect(void)
{
    FILE *p;
    static char buf[32768];
    size_t got;
    char *chunk;

    p = popen("timeout 2 gdbus call --session "
              "--dest org.gnome.Shell.Extensions.GSConnect "
              "--object-path /org/gnome/Shell/Extensions/GSConnect "
              "--method org.freedesktop.DBus.ObjectManager"
              ".GetManagedObjects 2>/dev/null </dev/null", "r");
    if (!p)
        return;
    got = fread(buf, 1, sizeof buf - 1, p);
    buf[got] = 0;
    pclose(p);

    chunk = strstr(buf, "/Device/");
    while (chunk) {
        char *next = strstr(chunk + 8, "/Device/");
        char name[64] = "";
        int level = -1, charging = 0, connected = 0;
        char *f;
        size_t len = next ? (size_t)(next - chunk) : strlen(chunk);
        char save = chunk[len];

        chunk[len] = 0;
        if ((f = strstr(chunk, "'Name': <")))
            gv_str(f + 8, name, sizeof name);
        if ((f = strstr(chunk, "'Connected': <")))
            connected = strstr(f, "<true") != NULL;
        if ((f = strstr(chunk, "'Level': <")))
            level = gv_int(f + 9);
        if ((f = strstr(chunk, "'Charging': <")))
            charging = strstr(f, "<true") != NULL;
        chunk[len] = save;

        if (name[0] && connected && level >= 0 && level <= 100) {
            Dev *dv = dev_append(KIND_DEVBAT, "GSCONN", name);
            if (dv) {
                dv->capacity = level;
                strcpy(dv->status, charging ? "CHG" : "DIS");
            }
        }
        chunk = next;
    }
}

/* ---------------- UPS via NUT (upsc) -------------------------------- */

static void scan_nut(void)
{
    FILE *p;
    char line[256];
    char names[4][64];
    int n = 0, i;

    p = popen("timeout 2 upsc -l 2>/dev/null </dev/null", "r");
    if (!p)
        return;
    while (fgets(line, sizeof line, p) && n < 4) {
        line[strcspn(line, "\n")] = 0;
        if (serial_ok(line))
            snprintf(names[n++], sizeof names[0], "%.63s", line);
    }
    pclose(p);

    for (i = 0; i < n; i++) {
        char cmd[128], model[64] = "", status[32] = "";
        int charge = -1, runtime_s = -1;
        double volt = -1;
        Dev *dv;

        snprintf(cmd, sizeof cmd, "timeout 2 upsc %.63s 2>/dev/null </dev/null",
                 names[i]);
        p = popen(cmd, "r");
        if (!p)
            continue;
        while (fgets(line, sizeof line, p)) {
            line[strcspn(line, "\n")] = 0;
            if (!strncmp(line, "battery.charge:", 15))
                charge = (int)parse_long(line + 15, -1, 100, -1);
            else if (!strncmp(line, "battery.runtime:", 16))
                runtime_s = (int)parse_long(line + 16, -1, 6000000, -1);
            else if (!strncmp(line, "battery.voltage:", 16))
                volt = atof(line + 16);
            else if (!strncmp(line, "ups.model:", 10))
                snprintf(model, sizeof model, "%.63s",
                         skip_spaces(line + 10));
            else if (!strncmp(line, "ups.status:", 11))
                snprintf(status, sizeof status, "%.31s",
                         skip_spaces(line + 11));
        }
        pclose(p);
        if (charge < 0)
            continue;

        dv = dev_append(KIND_DEVBAT, "UPS", model[0] ? model : names[i]);
        if (!dv)
            continue;
        dv->capacity = charge > 100 ? 100 : charge;
        dv->voltage_uv = volt > 0 ? (long)(volt * 1e6) : -1;
        if (runtime_s > 0) {
            dv->est_min = runtime_s / 60;
            dv->est_ttf = 0;
        }
        if (strstr(status, "OB"))        strcpy(dv->status, "DIS");
        else if (strstr(status, "CHRG")) strcpy(dv->status, "CHG");
        else if (strstr(status, "OL"))   strcpy(dv->status, "IDL");
    }
}

/* ---------------- low battery notifications ------------------------ */

enum { AL_OK = 0, AL_WARN, AL_CRIT };

typedef struct {
    char label[28];
    int state;
} Alert;

static Alert g_alerts[MAX_DEVS];
static int g_nalerts = 0;

/* strip anything shell-risky out of notification text */
static PowerNotifyFn g_notify_fn;
static void *g_notify_user;

void power_set_notifier(PowerNotifyFn fn, void *user)
{
    g_notify_fn = fn;
    g_notify_user = user;
}

static void send_notification(const char *title, const char *body,
                              const char *label, int critical)
{
    char t[64], b[96];
    if (!g_notify_fn)
        return;
    snprintf(t, sizeof t, "%.60s", title);
    snprintf(b, sizeof b, "%.90s", body);
    g_notify_fn(t, b, label, critical, g_notify_user);
}

static void alert_check(void)
{
    int i, j;
    if (!g_cfg.notify)
        return;
    for (i = 0; i < g_ndevs; i++) {
        Dev *dv = &g_devs[i];
        Alert *al = NULL;
        int want;
        if (dv->kind == KIND_MAINS || dv->capacity < 0)
            continue;
        for (j = 0; j < g_nalerts; j++)
            if (!strcmp(g_alerts[j].label, dv->label)) {
                al = &g_alerts[j];
                break;
            }
        if (!al) {
            if (g_nalerts < MAX_DEVS) {
                al = &g_alerts[g_nalerts++];
            } else {
                /* full: recycle the entry of a device that is no longer
                 * present, so a long-running session keeps alerting */
                for (j = 0; j < g_nalerts; j++)
                    if (!label_exists(g_alerts[j].label)) {
                        al = &g_alerts[j];
                        break;
                    }
                if (!al)
                    continue;
            }
            snprintf(al->label, sizeof al->label, "%.27s", dv->label);
            al->state = AL_OK;
        }
        want = AL_OK;
        if (dv->capacity <= g_cfg.crit_pct)
            want = AL_CRIT;
        else if (dv->capacity <= g_cfg.warn_pct)
            want = AL_WARN;
        /* charging clears the alarm, and recovery needs hysteresis */
        if (!strcmp(dv->status, "CHG") ||
            dv->capacity > g_cfg.warn_pct + 5)
            al->state = AL_OK;
        if (want > al->state && strcmp(dv->status, "CHG")) {
            char title[64], body[96];
            snprintf(title, sizeof title, "%.27s at %d%%", dv->label,
                     dv->capacity);
            snprintf(body, sizeof body, want == AL_CRIT
                     ? "Critical battery level. Recharge now."
                     : "Battery is running low. Recharge soon.");
            send_notification(title, body, dv->label,
                              want == AL_CRIT);
            al->state = want;
        }
    }
}

/* slope-based estimate fallback from the charge history */
static void estimate_from_history(Dev *dv)
{
    int i;
    if (dv->est_min > 0 || dv->capacity < 0)
        return;
    for (i = 0; i < g_nhist; i++) {
        Hist *h = &g_hist[i];
        int oldest, newest, delta, mins;
        if (strcmp(h->label, dv->label) || h->used < 30)
            continue;
        oldest = h->cap[(h->head - h->used + HIST_N) % HIST_N];
        newest = h->cap[(h->head - 1 + HIST_N) % HIST_N];
        if (oldest < 0 || newest < 0)
            return;
        delta = newest - oldest;
        mins = (h->used - 1) * g_cfg.scan_ms / 60000;
        if (mins < 1 || abs(delta) < 2)
            return;
        if (delta < 0 && !strcmp(dv->status, "DIS")) {
            dv->est_min = newest * mins / -delta;
            dv->est_ttf = 0;
        } else if (delta > 0 && !strcmp(dv->status, "CHG")) {
            dv->est_min = (100 - newest) * mins / delta;
            dv->est_ttf = 1;
        }
        return;
    }
}

/* returns 1 when a new device battery appeared since the last scan */
int power_scan_ex(int push_history)
{
    static char prev[MAX_DEVS][28];
    static int nprev = -1;
    int i, j, popped = 0;
    float total_w = 0;

    g_upower_link = scan_upower() > 0;
    if (!g_upower_link)
        scan_sysfs();
    scan_adb();
    scan_kdeconnect();
    scan_gsconnect();
    scan_nut();
    qsort(g_devs, (size_t)g_ndevs, sizeof(Dev), dev_cmp);

    /* new-plug detection over device batteries */
    for (i = 0; i < g_ndevs; i++) {
        if (g_devs[i].kind != KIND_DEVBAT)
            continue;
        if (nprev >= 0) {
            int found = 0;
            for (j = 0; j < nprev; j++)
                if (!strcmp(prev[j], g_devs[i].label))
                    found = 1;
            if (!found)
                popped = 1;
        }
    }
    nprev = 0;
    for (i = 0; i < g_ndevs && nprev < MAX_DEVS; i++)
        if (g_devs[i].kind == KIND_DEVBAT)
            snprintf(prev[nprev++], sizeof prev[0], "%.27s",
                     g_devs[i].label);

    /* history samples */
    if (push_history)
        for (i = 0; i < g_nhist; i++)
            g_hist[i].active = 0;
    for (i = 0; i < g_ndevs; i++) {
        if (push_history && g_devs[i].kind != KIND_MAINS &&
            g_devs[i].capacity >= 0)
            hist_push(g_devs[i].label, g_devs[i].capacity);
        if (g_devs[i].power_uw > 0)
            total_w += (float)(g_devs[i].power_uw / 1e6);
    }
    g_pwr_now = total_w;

    for (i = 0; i < g_ndevs; i++)
        estimate_from_history(&g_devs[i]);
    alert_check();

    return popped;
}


/* ------------------------------------------------------------------ */
/* derived values for the dashboard                                    */
/* ------------------------------------------------------------------ */

/* most recent total bus load, in watts */
float power_total_load_w(void)
{
    return g_pwr_now;
}

/* lowest charge across every battery, -1 when nothing reports one */
int power_worst_pct(void)
{
    int i, worst = -1;
    for (i = 0; i < g_ndevs; i++) {
        if (g_devs[i].kind == KIND_MAINS || g_devs[i].capacity < 0)
            continue;
        if (worst < 0 || g_devs[i].capacity < worst)
            worst = g_devs[i].capacity;
    }
    return worst;
}

int power_any_charging(void)
{
    int i;
    for (i = 0; i < g_ndevs; i++)
        if (!strcmp(g_devs[i].status, "CHG"))
            return 1;
    return 0;
}
