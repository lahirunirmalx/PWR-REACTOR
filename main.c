/*
 * PWR-REACTOR MK.II - USB power telemetry panel
 *
 * Data source: `upower --dump` (phones, mice, keyboards and other USB/BT
 * peripherals with proper model names), falling back to raw
 * /sys/class/power_supply when upower is unavailable.
 *
 * Runs as a resident tray application: closing the window hides it, a
 * tray icon (StatusNotifier via libayatana-appindicator, loaded with
 * dlopen so no -dev package is needed) restores it, and plugging in a
 * new device battery pops the panel up automatically.
 *
 * Build:  make
 * Deps :  SDL2, GTK3 (tray, optional), upower (recommended)
 *
 * Flags:  --hidden   start minimized to tray (used by the service)
 *         --amber    start with amber phosphor
 *         --widget   borderless always-on-top mini panel (drag by header)
 *
 * Config: ~/.config/power-reactor.conf (created with defaults on first
 * run): scan interval, alert thresholds, popup/notify/sound switches,
 * phosphor, tray label, widget mode and position.
 *
 * Keys :  H hold scan, L lamp test, P phosphor, ESC hide, Q quit
 */

#include <SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef USE_TRAY
#include <dlfcn.h>
#include <gtk/gtk.h>
#endif

#define WIN_W 640
#define WIN_H 420
#define SCREEN_X 12
#define SCREEN_Y 12
#define SCREEN_W (WIN_W - 24)
#define SCREEN_H (WIN_H - 24)

#define ROW_Y0 (SCREEN_Y + 34)
#define ROW_H  46
#define BOT_Y  (SCREEN_Y + SCREEN_H - 54)

#define MAX_DEVS 16

#define HIST_N 180 /* samples kept per device, one per scan (~6 min) */

#define SYSFS_PS "/sys/class/power_supply"

/* ------------------------------------------------------------------ */
/* theme - single source for every color on the panel                  */
/* ------------------------------------------------------------------ */

typedef struct {
    SDL_Color fg;
    SDL_Color dim;
    SDL_Color amber;
    SDL_Color red;
    SDL_Color green;
    SDL_Color metal;
    SDL_Color metal2;
    SDL_Color metal3;
    SDL_Color screen;
    SDL_Color lampoff;
} Theme;

static Theme g_theme;
static int g_amber_mode = 0;

static void theme_apply(void)
{
    if (g_amber_mode) {
        g_theme.fg  = (SDL_Color){255, 178, 44, 255};
        g_theme.dim = (SDL_Color){96, 62, 14, 255};
    } else {
        g_theme.fg  = (SDL_Color){92, 255, 138, 255};
        g_theme.dim = (SDL_Color){26, 82, 44, 255};
    }
    g_theme.amber   = (SDL_Color){255, 176, 32, 255};
    g_theme.red     = (SDL_Color){255, 70, 54, 255};
    g_theme.green   = (SDL_Color){90, 255, 120, 255};
    g_theme.metal   = (SDL_Color){52, 56, 52, 255};
    g_theme.metal2  = (SDL_Color){86, 92, 86, 255};
    g_theme.metal3  = (SDL_Color){24, 26, 24, 255};
    g_theme.screen  = (SDL_Color){7, 13, 9, 255};
    g_theme.lampoff = (SDL_Color){40, 34, 30, 255};
}

/* ------------------------------------------------------------------ */
/* config - ~/.config/power-reactor.conf, created with defaults on     */
/* first run. key=value lines, # comments.                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int scan_ms;
    int warn_pct;
    int crit_pct;
    int popup_on_plug;
    int notify;
    int sound;
    int amber;
    int tray_label;
    int widget;
    int widget_x; /* -1 = auto top-right */
    int widget_y;
} Config;

static Config g_cfg = {2000, 15, 5, 1, 1, 1, 0, 1, 0, -1, -1};

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

static void config_path(char *out, size_t n)
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
        "# start with amber phosphor instead of green\n"
        "amber=0\n"
        "# show lowest device percentage next to the tray icon\n"
        "tray_label=1\n"
        "# widget mode: borderless always-on-top mini panel\n"
        "widget=0\n"
        "# widget position, -1 = auto top-right corner\n"
        "widget_x=-1\n"
        "widget_y=-1\n");
    fclose(f);
}

static void config_load(void)
{
    char path[512], line[128];
    FILE *f;
    config_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f) {
        config_write_default(path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq, *key = line, *val;
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '#' || !line[0])
            continue;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        val = eq + 1;
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
        else if (!strcmp(key, "amber"))
            g_cfg.amber = (int)parse_long(val, 0, 1, 0);
        else if (!strcmp(key, "tray_label"))
            g_cfg.tray_label = (int)parse_long(val, 0, 1, 1);
        else if (!strcmp(key, "widget"))
            g_cfg.widget = (int)parse_long(val, 0, 1, 0);
        else if (!strcmp(key, "widget_x"))
            g_cfg.widget_x = (int)parse_long(val, -1, 32767, -1);
        else if (!strcmp(key, "widget_y"))
            g_cfg.widget_y = (int)parse_long(val, -1, 32767, -1);
    }
    fclose(f);
    if (g_cfg.crit_pct > g_cfg.warn_pct)
        g_cfg.crit_pct = g_cfg.warn_pct;
}

/* trace palette for the trend scope */
static SDL_Color trace_color(int i)
{
    switch (i % 4) {
    case 0:  return g_theme.fg;
    case 1:  return g_theme.amber;
    case 2:  return (SDL_Color){120, 200, 255, 255};
    default: return g_theme.red;
    }
}

/* ------------------------------------------------------------------ */
/* 5x7 bitmap font, columns, LSB = top row. chars 0x20..0x5F           */
/* ------------------------------------------------------------------ */

static const unsigned char FONT[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x41,0x22,0x14,0x08,0x00}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
};

/* ------------------------------------------------------------------ */
/* device model + history                                              */
/* ------------------------------------------------------------------ */

enum { KIND_SYSBAT = 0, KIND_DEVBAT, KIND_MAINS };

typedef struct {
    char label[28];
    char tag[12];
    char status[8];
    int  kind;
    int  capacity;
    int  online;
    long voltage_uv;
    long power_uw;
    int  est_min;   /* minutes to empty/full, -1 unknown */
    int  est_ttf;   /* 1 = time to full, 0 = time to empty */
} Dev;

static Dev g_devs[MAX_DEVS];
static int g_ndevs = 0;
static int g_upower_link = 0;

typedef struct {
    char label[28];
    signed char cap[HIST_N]; /* -1 = no sample */
    int  head;
    int  used;
    int  active; /* seen in latest scan */
} Hist;

static Hist g_hist[MAX_DEVS];
static int g_nhist = 0;
static float g_pwr_hist[HIST_N]; /* total load W per scan */
static int g_pwr_head = 0, g_pwr_used = 0;

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
        if (g_nhist >= MAX_DEVS)
            return;
        h = &g_hist[g_nhist++];
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

static int dev_cmp(const void *a, const void *b)
{
    return ((const Dev *)a)->kind - ((const Dev *)b)->kind;
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
    str_upper(dv->label);
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
    FILE *p = popen("upower --dump 2>/dev/null", "r");
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
        str_upper(dv->label);
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

    p = popen("timeout 2 adb devices -l 2>/dev/null", "r");
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
                 "timeout 2 adb -s %.63s shell dumpsys battery 2>/dev/null",
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
        if (!SDL_strcasecmp(g_devs[i].label, label))
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
    str_upper(clean);
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
              "true true 2>/dev/null", "r");
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
              ".GetManagedObjects 2>/dev/null", "r");
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

    p = popen("timeout 2 upsc -l 2>/dev/null", "r");
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

        snprintf(cmd, sizeof cmd, "timeout 2 upsc %.63s 2>/dev/null",
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
                snprintf(model, sizeof model, "%.63s", line + 11);
            else if (!strncmp(line, "ups.status:", 11))
                snprintf(status, sizeof status, "%.31s", line + 12);
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
static void sanitize_text(char *s)
{
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
              (*s >= '0' && *s <= '9') || strchr(" .%:-", *s)))
            *s = ' ';
}

static void send_notification(const char *title, const char *body,
                              int critical)
{
    char t[64], b[96], cmd[320];
    snprintf(t, sizeof t, "%.60s", title);
    snprintf(b, sizeof b, "%.90s", body);
    sanitize_text(t);
    sanitize_text(b);
    snprintf(cmd, sizeof cmd,
             "notify-send -a PWR-REACTOR -i power-reactor -u %s "
             "\"%s\" \"%s\" >/dev/null 2>&1 &",
             critical ? "critical" : "normal", t, b);
    if (system(cmd) == -1)
        return;
    if (g_cfg.sound) {
        snprintf(cmd, sizeof cmd,
                 "paplay /usr/share/sounds/freedesktop/stereo/%s.oga "
                 ">/dev/null 2>&1 &",
                 critical ? "dialog-error" : "dialog-warning");
        if (system(cmd) == -1)
            return;
    }
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
            if (g_nalerts >= MAX_DEVS)
                continue;
            al = &g_alerts[g_nalerts++];
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
            snprintf(title, sizeof title, "%.27s AT %d%%", dv->label,
                     dv->capacity);
            snprintf(body, sizeof body, want == AL_CRIT
                     ? "CRITICAL - RECHARGE IMMEDIATELY"
                     : "LOW BATTERY - RECHARGE SOON");
            send_notification(title, body, want == AL_CRIT);
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
static int scan_devices(void)
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
    for (i = 0; i < g_nhist; i++)
        g_hist[i].active = 0;
    for (i = 0; i < g_ndevs; i++) {
        if (g_devs[i].kind != KIND_MAINS && g_devs[i].capacity >= 0)
            hist_push(g_devs[i].label, g_devs[i].capacity);
        if (g_devs[i].power_uw > 0)
            total_w += (float)(g_devs[i].power_uw / 1e6);
    }
    g_pwr_hist[g_pwr_head] = total_w;
    g_pwr_head = (g_pwr_head + 1) % HIST_N;
    if (g_pwr_used < HIST_N)
        g_pwr_used++;

    for (i = 0; i < g_ndevs; i++)
        estimate_from_history(&g_devs[i]);
    alert_check();

    return popped;
}

/* ------------------------------------------------------------------ */
/* drawing primitives                                                  */
/* ------------------------------------------------------------------ */

static SDL_Renderer *g_r;
static float g_bright = 1.0f;

static void setc(SDL_Color c, int alpha)
{
    SDL_SetRenderDrawColor(g_r,
        (Uint8)(c.r * g_bright), (Uint8)(c.g * g_bright),
        (Uint8)(c.b * g_bright), (Uint8)alpha);
}

static void frect(int x, int y, int w, int h)
{
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(g_r, &r);
}

static void fcircle(int cx, int cy, int rad)
{
    int dy;
    for (dy = -rad; dy <= rad; dy++) {
        int dx = (int)floor(sqrt((double)rad * rad - (double)dy * dy));
        frect(cx - dx, cy + dy, dx * 2 + 1, 1);
    }
}

static void draw_text(int x, int y, int scale, const char *s, SDL_Color c)
{
    int pass;
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_ADD);
    for (pass = 0; pass < 2; pass++) {
        const char *p;
        int cx = x;
        int pad = pass == 0 ? 1 : 0;
        setc(c, pass == 0 ? 40 : 255);
        for (p = s; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            int col;
            if (ch >= 'a' && ch <= 'z')
                ch -= 32;
            if (ch < 0x20 || ch > 0x5F)
                ch = '?';
            for (col = 0; col < 5; col++) {
                unsigned char bits = FONT[ch - 0x20][col];
                int row;
                for (row = 0; row < 7; row++)
                    if (bits & (1 << row))
                        frect(cx + col * scale - pad,
                              y + row * scale - pad,
                              scale + pad, scale + pad);
            }
            cx += 6 * scale;
        }
    }
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
}

static int text_w(int scale, const char *s)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

static const unsigned char SEG7[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void draw_7seg(int x, int y, int w, int h, int digit, SDL_Color c,
                      SDL_Color offc)
{
    int t = w / 5;
    if (t < 2)
        t = 2;
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_ADD);
    {
        unsigned char m = (digit >= 0 && digit <= 9) ? SEG7[digit]
                        : (digit == -2 ? 0x40 : 0x00);
        struct { int x, y, w, h; } segs[7] = {
            { x + t,         y,             w - 2 * t, t          },
            { x + w - t,     y + t,         t,         h / 2 - t  },
            { x + w - t,     y + h / 2,     t,         h / 2 - t  },
            { x + t,         y + h - t,     w - 2 * t, t          },
            { x,             y + h / 2,     t,         h / 2 - t  },
            { x,             y + t,         t,         h / 2 - t  },
            { x + t,         y + h / 2 - t / 2, w - 2 * t, t      },
        };
        int i, pass;
        for (i = 0; i < 7; i++) {
            int on = (m >> i) & 1;
            if (!on) {
                setc(offc, 255);
                frect(segs[i].x, segs[i].y, segs[i].w, segs[i].h);
                continue;
            }
            for (pass = 0; pass < 2; pass++) {
                int pad = pass == 0 ? 1 : 0;
                setc(c, pass == 0 ? 55 : 255);
                frect(segs[i].x - pad, segs[i].y - pad,
                      segs[i].w + 2 * pad, segs[i].h + 2 * pad);
            }
        }
    }
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
}

static void draw_lamp(int cx, int cy, int rad, SDL_Color c, int lit)
{
    setc(g_theme.metal3, 255);
    fcircle(cx, cy, rad + 3);
    setc(g_theme.metal2, 255);
    fcircle(cx, cy, rad + 1);
    if (!lit) {
        setc(g_theme.lampoff, 255);
        fcircle(cx, cy, rad);
        setc((SDL_Color){70, 62, 56, 255}, 255);
        fcircle(cx - rad / 3, cy - rad / 3, rad / 4 ? rad / 4 : 1);
        return;
    }
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_ADD);
    setc(c, 40);
    fcircle(cx, cy, rad + 5);
    setc(c, 70);
    fcircle(cx, cy, rad + 2);
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
    setc(c, 255);
    fcircle(cx, cy, rad);
    setc((SDL_Color){255, 250, 220, 255}, 220);
    fcircle(cx, cy, rad / 2);
    setc((SDL_Color){255, 255, 255, 255}, 230);
    fcircle(cx - rad / 3, cy - rad / 3, rad / 4 ? rad / 4 : 1);
}

static void draw_toggle(int cx, int cy, int on)
{
    int i;
    setc(g_theme.metal3, 255);
    frect(cx - 19, cy - 16, 38, 32);
    setc(g_theme.metal2, 255);
    frect(cx - 17, cy - 14, 34, 28);
    setc(g_theme.metal, 255);
    frect(cx - 15, cy - 12, 30, 24);
    setc(g_theme.metal3, 255);
    fcircle(cx, cy, 8);
    setc(g_theme.metal2, 255);
    fcircle(cx, cy, 6);
    {
        int dir = on ? -1 : 1;
        int len = 13;
        for (i = 0; i < len; i++) {
            int w = 6 - i * 2 / len;
            setc((SDL_Color){150, 155, 150, 255}, 255);
            frect(cx - w / 2, cy + dir * i, w, 1);
        }
        setc((SDL_Color){190, 195, 190, 255}, 255);
        fcircle(cx, cy + dir * len, 4);
        setc((SDL_Color){235, 240, 235, 255}, 255);
        fcircle(cx - 1, cy + dir * len - 1, 1);
    }
}

static void draw_screw(int cx, int cy)
{
    setc(g_theme.metal3, 255);
    fcircle(cx, cy, 5);
    setc(g_theme.metal2, 255);
    fcircle(cx, cy, 4);
    setc(g_theme.metal3, 255);
    frect(cx - 3, cy, 7, 1);
    frect(cx, cy - 3, 1, 7);
}

static void draw_gauge(int x, int y, int w, int h, int pct, int charging,
                       Uint32 ticks)
{
    int nseg = 15;
    int gap = 2;
    int segw = (w - gap * (nseg - 1)) / nseg;
    int i;
    SDL_Color c = g_theme.green;

    if (pct >= 0 && pct <= 20)
        c = g_theme.red;
    else if (pct > 20 && pct <= 50)
        c = g_theme.amber;

    setc(g_theme.metal3, 255);
    frect(x - 3, y - 3, w + 6, h + 6);
    setc(g_theme.screen, 255);
    frect(x - 1, y - 1, w + 2, h + 2);

    for (i = 0; i < nseg; i++) {
        int sx = x + i * (segw + gap);
        int lit = pct >= 0 && (i + 1) * 100 <= pct * nseg + 50;
        int sweep = charging && (int)((ticks / 90) % (Uint32)nseg) == i;
        if (lit || sweep) {
            SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_ADD);
            setc(c, 50);
            frect(sx - 1, y - 1, segw + 2, h + 2);
            setc(c, 255);
            frect(sx, y, segw, h);
            SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
        } else {
            setc(g_theme.dim, 120);
            frect(sx, y, segw, h);
        }
    }
}

/* ------------------------------------------------------------------ */
/* trend scope + load meter                                            */
/* ------------------------------------------------------------------ */

static float g_meter_max = 10.0f;
static float g_needle = 0.0f;
static float g_wpeak = 0.0f;

static float current_load_w(void)
{
    int idx = (g_pwr_head - 1 + HIST_N) % HIST_N;
    return g_pwr_used ? g_pwr_hist[idx] : 0.0f;
}

static void draw_scope(int x, int y, int w, int h)
{
    int ix = x + 8, iy = y + 16, iw = w - 16, ih = h - 30;
    int i, t;
    int step = 2;
    int maxs = iw / step;

    setc(g_theme.metal3, 255);
    frect(x - 2, y - 2, w + 4, h + 4);
    setc((SDL_Color){4, 9, 6, 255}, 255);
    frect(x, y, w, h);
    draw_text(x + 6, y + 4, 1, "CHARGE TREND [6 MIN]", g_theme.fg);

    /* grid */
    for (i = 0; i <= 4; i++) {
        int gy = iy + ih - ih * i / 4;
        setc(g_theme.dim, i % 2 ? 60 : 120);
        frect(ix, gy, iw, 1);
    }
    for (i = 0; i < iw; i += 30) {
        setc(g_theme.dim, 50);
        for (t = 0; t < ih; t += 4)
            frect(ix + i, iy + t, 1, 1);
    }
    draw_text(ix + iw - text_w(1, "100"), iy - 2, 1, "100", g_theme.dim.g >
              40 ? g_theme.fg : g_theme.fg);

    /* load histogram bars along the bottom (scaled to meter range) */
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_ADD);
    for (i = 0; i < g_pwr_used && i < maxs; i++) {
        int idx = (g_pwr_head - 1 - i + 2 * HIST_N) % HIST_N;
        float v = g_pwr_hist[idx] / (g_meter_max > 1 ? g_meter_max : 1);
        int bh = (int)(v * (ih - 4));
        if (bh > ih - 4)
            bh = ih - 4;
        if (bh > 0) {
            setc(g_theme.red, 60);
            frect(ix + iw - (i + 1) * step, iy + ih - bh, step - 1, bh);
        }
    }
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);

    /* capacity traces with phosphor persistence (older = dimmer) */
    {
        int hi, li = 0;
        for (hi = 0; hi < g_nhist; hi++) {
            Hist *hs = &g_hist[hi];
            SDL_Color c;
            int n, px = -1, py = -1;
            if (!hs->active || hs->used < 2)
                continue;
            c = trace_color(li);
            n = hs->used < maxs ? hs->used : maxs;
            SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_ADD);
            for (i = 0; i < n; i++) {
                int idx = (hs->head - n + i + 2 * HIST_N) % HIST_N;
                int cap = hs->cap[idx];
                int cx2 = ix + iw - (n - i) * step;
                int cy2 = iy + ih - (ih * cap) / 100;
                if (cap < 0) {
                    px = -1;
                    continue;
                }
                if (px >= 0) {
                    int a = 70 + 180 * i / n;
                    setc(c, a);
                    SDL_RenderDrawLine(g_r, px, py, cx2, cy2);
                    SDL_RenderDrawLine(g_r, px, py + 1, cx2, cy2 + 1);
                }
                px = cx2;
                py = cy2;
            }
            /* bright head dot */
            if (px >= 0) {
                setc(c, 255);
                frect(px - 1, py - 1, 3, 3);
            }
            SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
            /* legend */
            {
                char lg[20];
                snprintf(lg, sizeof lg, "%.14s", hs->label);
                draw_text(x + 6, iy + 2 + li * 10, 1, lg, c);
            }
            li++;
        }
        if (!li)
            draw_text(ix + 20, iy + ih / 2 - 4, 1, "ACQUIRING DATA...",
                      g_theme.dim.g ? g_theme.fg : g_theme.fg);
    }
}

static void draw_meter(int x, int y, int w, int h)
{
    int cx = x + w / 2;
    int cy = y + h - 16;
    int r = h - 44;
    int i;
    float load = current_load_w();
    char buf[24];

    if (r > w / 2 - 14)
        r = w / 2 - 14;

    /* auto range + damped needle + peak hold */
    if (load > g_meter_max * 0.95f)
        g_meter_max = ceilf(load / 5.0f) * 5.0f + 5.0f;
    g_needle += (load - g_needle) * 0.08f;
    if (load > g_wpeak)
        g_wpeak = load;
    else
        g_wpeak -= 0.005f;
    if (g_wpeak < 0)
        g_wpeak = 0;

    setc(g_theme.metal3, 255);
    frect(x - 2, y - 2, w + 4, h + 4);
    setc((SDL_Color){10, 12, 10, 255}, 255);
    frect(x, y, w, h);
    draw_text(x + 6, y + 4, 1, "BUS LOAD", g_theme.fg);

    /* scale arc: 210 deg .. 330 deg (up-left to up-right) */
    for (i = 0; i <= 20; i++) {
        double a = (210.0 + 120.0 * i / 20.0) * M_PI / 180.0;
        int major = i % 5 == 0;
        int x1 = cx + (int)(cos(a) * r);
        int y1 = cy + (int)(sin(a) * r);
        int x2 = cx + (int)(cos(a) * (r - (major ? 8 : 4)));
        int y2 = cy + (int)(sin(a) * (r - (major ? 8 : 4)));
        setc(major ? g_theme.fg : g_theme.dim, 255);
        SDL_RenderDrawLine(g_r, x1, y1, x2, y2);
    }
    draw_text(cx - r + 2, cy - r / 2 - 2, 1, "0", g_theme.fg);
    snprintf(buf, sizeof buf, "%.0f", (double)g_meter_max);
    draw_text(cx + r - 2 - text_w(1, buf), cy - r / 2 - 2, 1, buf,
              g_theme.fg);

    /* peak hold marker */
    {
        double frac = g_wpeak / g_meter_max;
        double a;
        if (frac > 1)
            frac = 1;
        a = (210.0 + 120.0 * frac) * M_PI / 180.0;
        setc(g_theme.red, 255);
        SDL_RenderDrawLine(g_r,
            cx + (int)(cos(a) * (r - 10)), cy + (int)(sin(a) * (r - 10)),
            cx + (int)(cos(a) * (r - 2)),  cy + (int)(sin(a) * (r - 2)));
    }

    /* needle */
    {
        double frac = g_needle / g_meter_max;
        double a;
        int nx, ny, o;
        if (frac > 1)
            frac = 1;
        if (frac < 0)
            frac = 0;
        a = (210.0 + 120.0 * frac) * M_PI / 180.0;
        nx = cx + (int)(cos(a) * (r - 6));
        ny = cy + (int)(sin(a) * (r - 6));
        SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_ADD);
        setc(g_theme.amber, 70);
        for (o = -1; o <= 1; o++)
            SDL_RenderDrawLine(g_r, cx + o, cy, nx + o, ny);
        setc(g_theme.amber, 255);
        SDL_RenderDrawLine(g_r, cx, cy, nx, ny);
        SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
    }
    /* pivot dome */
    setc(g_theme.metal2, 255);
    fcircle(cx, cy, 5);
    setc(g_theme.metal3, 255);
    fcircle(cx, cy, 2);

    snprintf(buf, sizeof buf, "%5.2f W", (double)g_needle);
    draw_text(cx - text_w(1, buf) / 2, y + h - 12, 1, buf, g_theme.amber);
}

/* ------------------------------------------------------------------ */
/* panel chrome + crt effects                                          */
/* ------------------------------------------------------------------ */

static void draw_bezel(void)
{
    setc(g_theme.metal, 255);
    frect(0, 0, WIN_W, WIN_H);
    setc(g_theme.metal2, 255);
    frect(0, 0, WIN_W, 2);
    frect(0, 0, 2, WIN_H);
    setc(g_theme.metal3, 255);
    frect(0, WIN_H - 2, WIN_W, 2);
    frect(WIN_W - 2, 0, 2, WIN_H);
    setc(g_theme.metal3, 255);
    frect(SCREEN_X - 4, SCREEN_Y - 4, SCREEN_W + 8, SCREEN_H + 8);
    setc(g_theme.screen, 255);
    frect(SCREEN_X, SCREEN_Y, SCREEN_W, SCREEN_H);
    draw_screw(6, 6);
    draw_screw(WIN_W - 7, 6);
    draw_screw(6, WIN_H - 7);
    draw_screw(WIN_W - 7, WIN_H - 7);
}

static void draw_crt_overlay(void)
{
    int y, i;
    setc((SDL_Color){0, 0, 0, 255}, 50);
    for (y = SCREEN_Y; y < SCREEN_Y + SCREEN_H; y += 3)
        frect(SCREEN_X, y, SCREEN_W, 1);
    for (i = 0; i < 12; i++) {
        int a = 50 - i * 4;
        if (a < 0)
            a = 0;
        setc((SDL_Color){0, 0, 0, 255}, a);
        frect(SCREEN_X, SCREEN_Y + i, SCREEN_W, 1);
        frect(SCREEN_X, SCREEN_Y + SCREEN_H - 1 - i, SCREEN_W, 1);
        frect(SCREEN_X + i, SCREEN_Y, 1, SCREEN_H);
        frect(SCREEN_X + SCREEN_W - 1 - i, SCREEN_Y, 1, SCREEN_H);
    }
}

/* ------------------------------------------------------------------ */
/* screen content                                                      */
/* ------------------------------------------------------------------ */

static void draw_header(Uint32 ticks)
{
    char clock[16];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(clock, sizeof clock, "%02d:%02d:%02d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    draw_text(SCREEN_X + 14, SCREEN_Y + 8, 2, "PWR-REACTOR MK.II",
              g_theme.fg);
    if ((ticks / 500) % 2)
        draw_text(SCREEN_X + 240, SCREEN_Y + 12, 1, "* LIVE",
                  g_theme.green);
    draw_text(SCREEN_X + SCREEN_W - 14 - text_w(2, clock),
              SCREEN_Y + 8, 2, clock, g_theme.fg);
    setc(g_theme.dim, 255);
    frect(SCREEN_X + 12, SCREEN_Y + 28, SCREEN_W - 24, 1);
}

static void draw_percent_readout(int x, int y, int pct, int blink_on)
{
    int dw = 16, dh = 26, dg = 5;
    int d0, d1, d2;
    SDL_Color off = g_theme.dim;
    off.r = (Uint8)(off.r / 3);
    off.g = (Uint8)(off.g / 3);
    off.b = (Uint8)(off.b / 3);

    if (pct < 0) {
        d0 = d1 = d2 = -2;
    } else {
        d0 = pct / 100;
        d1 = (pct / 10) % 10;
        d2 = pct % 10;
        if (!d0) {
            d0 = -1;
            if (!d1 && pct < 10)
                d1 = -1;
        }
    }
    if (!blink_on)
        d0 = d1 = d2 = -1;
    draw_7seg(x, y, dw, dh, d0, g_theme.fg, off);
    draw_7seg(x + dw + dg, y, dw, dh, d1, g_theme.fg, off);
    draw_7seg(x + 2 * (dw + dg), y, dw, dh, d2, g_theme.fg, off);
    draw_text(x + 3 * (dw + dg) + 2, y + dh - 8, 1, "%", g_theme.fg);
}

static void draw_device_row(int idx, const Dev *dv, Uint32 ticks,
                            int lamp_test)
{
    int y = ROW_Y0 + idx * ROW_H;
    int charging = !strcmp(dv->status, "CHG");
    int critical = dv->capacity >= 0 && dv->capacity <= g_cfg.warn_pct &&
                   dv->kind != KIND_MAINS;
    int blink = (ticks / 400) % 2;
    char buf[64];

    setc(g_theme.dim, 110);
    frect(SCREEN_X + 12, y + ROW_H - 4, SCREEN_W - 24, 1);

    {
        SDL_Color lc = g_theme.green;
        int lit = 1;
        if (dv->kind == KIND_MAINS) {
            lit = dv->online == 1;
        } else if (critical) {
            lc = g_theme.red;
            lit = blink;
        } else if (charging) {
            lc = g_theme.amber;
            lit = blink;
        } else if (dv->capacity < 0) {
            lit = 0;
        }
        if (lamp_test) {
            lit = 1;
            lc = g_theme.amber;
        }
        draw_lamp(SCREEN_X + 26, y + 19, 7, lc, lit);
    }

    draw_text(SCREEN_X + 44, y + 4, 1, dv->label, g_theme.fg);
    snprintf(buf, sizeof buf, "%s STAT:%s", dv->tag, dv->status);
    draw_text(SCREEN_X + 44, y + 16, 1, buf, g_theme.fg);
    {
        char est[24] = "";
        int len = 0;
        buf[0] = 0;
        if (dv->est_min > 0) {
            if (dv->est_min >= 60)
                snprintf(est, sizeof est, "%s %dH%02dM",
                         dv->est_ttf ? "FULL IN" : "LEFT",
                         dv->est_min / 60, dv->est_min % 60);
            else
                snprintf(est, sizeof est, "%s %dM",
                         dv->est_ttf ? "FULL IN" : "LEFT", dv->est_min);
        }
        if (dv->voltage_uv > 0) {
            if (dv->power_uw >= 0)
                len = snprintf(buf, sizeof buf, "%5.2fV %5.2fW",
                               dv->voltage_uv / 1e6, dv->power_uw / 1e6);
            else
                len = snprintf(buf, sizeof buf, "%5.2fV",
                               dv->voltage_uv / 1e6);
        }
        if (est[0] && len >= 0 && (size_t)len + strlen(est) + 1 < sizeof buf) {
            if (len)
                strcat(buf, " ");
            strcat(buf, est);
        }
        if (buf[0])
            draw_text(SCREEN_X + 44, y + 28, 1, buf, g_theme.fg);
        else if (dv->capacity < 0 && dv->kind != KIND_MAINS)
            draw_text(SCREEN_X + 44, y + 28, 1, "NO TELEMETRY",
                      g_theme.amber);
    }

    if (dv->kind == KIND_MAINS) {
        const char *s = dv->online == 1 ? "LINE ONLINE" : "LINE OFFLINE";
        draw_text(SCREEN_X + 250, y + 14, 2, s,
                  dv->online == 1 ? g_theme.green : g_theme.red);
        return;
    }

    draw_gauge(SCREEN_X + 250, y + 12, 172, 14, dv->capacity, charging,
               ticks);
    draw_percent_readout(SCREEN_X + 448, y + 6, dv->capacity,
                         critical ? blink : 1);
}

typedef struct {
    SDL_Rect hit;
    const char *label;
    int *value;
} Station;

static int g_hold = 0, g_lamp_test = 0;

static void station_layout(Station *st, int nst)
{
    int i;
    for (i = 0; i < nst; i++) {
        int cx = SCREEN_X + 52 + i * 96;
        st[i].hit = (SDL_Rect){cx - 22, BOT_Y + 2, 44, 46};
    }
}

static void draw_bottom(Station *st, int nst, Uint32 ticks)
{
    int i;
    setc(g_theme.dim, 255);
    frect(SCREEN_X + 12, BOT_Y - 4, SCREEN_W - 24, 1);
    for (i = 0; i < nst; i++) {
        int cx = st[i].hit.x + st[i].hit.w / 2;
        draw_toggle(cx, BOT_Y + 20, *st[i].value);
        draw_text(cx - text_w(1, st[i].label) / 2, BOT_Y + 40, 1,
                  st[i].label, g_theme.fg);
    }
    {
        char buf[48];
        snprintf(buf, sizeof buf, "SOURCES: %02d", g_ndevs);
        draw_text(SCREEN_X + SCREEN_W - 24 - text_w(2, buf), BOT_Y + 6, 2,
                  buf, g_theme.fg);
        if (g_hold && (ticks / 400) % 2)
            draw_text(SCREEN_X + SCREEN_W - 24 - text_w(1, "SCAN HOLD"),
                      BOT_Y + 28, 1, "SCAN HOLD", g_theme.red);
        else if (!g_hold)
            draw_text(SCREEN_X + SCREEN_W - 24 -
                      text_w(1, g_upower_link ? "UPOWER LINK" : "SYSFS LINK"),
                      BOT_Y + 28, 1,
                      g_upower_link ? "UPOWER LINK" : "SYSFS LINK",
                      g_theme.fg);
    }
}

/* ------------------------------------------------------------------ */
/* tray icon (StatusNotifier via dlopen'd libayatana-appindicator)     */
/* ------------------------------------------------------------------ */

static int g_running = 1;
static int g_visible = 1;
static int g_want_show = 0;
static SDL_Window *g_win;

static volatile sig_atomic_t g_got_signal = 0;
static void on_signal(int sig)
{
    (void)sig;
    g_got_signal = 1;
}

#ifdef USE_TRAY

typedef void *(*ai_new_fn)(const char *, const char *, int);
typedef void *(*ai_new_path_fn)(const char *, const char *, int,
                                const char *);
typedef void (*ai_set_status_fn)(void *, int);
typedef void (*ai_set_menu_fn)(void *, GtkMenu *);
typedef void (*ai_set_title_fn)(void *, const char *);
typedef void (*ai_set_icon_full_fn)(void *, const char *, const char *);
typedef void (*ai_set_label_fn)(void *, const char *, const char *);
typedef void (*ai_set_sec_target_fn)(void *, GtkWidget *);

#ifndef ICON_DIR
#define ICON_DIR "."
#endif

static int g_tray_ok = 0;
static ai_set_title_fn g_ai_set_title;
static ai_set_icon_full_fn g_ai_set_icon;
static ai_set_label_fn g_ai_set_label;
static void *g_indicator;
static int g_tray_icon_state = -1; /* 0 green, 1 amber, 2 red */
static int g_tray_has_path = 0;

static void on_tray_show(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    /* middle-click / menu item: toggle */
    g_want_show = g_visible ? 2 : 1;
}

static void on_tray_quit(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    g_running = 0;
}

static int tray_init(void)
{
    void *h;
    ai_new_fn ai_new;
    ai_new_path_fn ai_new_path;
    ai_set_status_fn ai_set_status;
    ai_set_menu_fn ai_set_menu;
    GtkWidget *menu, *item;
    FILE *icon;

    if (!gtk_init_check(NULL, NULL))
        return 0;
    h = dlopen("libayatana-appindicator3.so.1", RTLD_NOW);
    if (!h)
        h = dlopen("libappindicator3.so.1", RTLD_NOW);
    if (!h)
        return 0;
    ai_new = (ai_new_fn)dlsym(h, "app_indicator_new");
    ai_new_path = (ai_new_path_fn)dlsym(h, "app_indicator_new_with_path");
    ai_set_status = (ai_set_status_fn)dlsym(h, "app_indicator_set_status");
    ai_set_menu = (ai_set_menu_fn)dlsym(h, "app_indicator_set_menu");
    g_ai_set_title = (ai_set_title_fn)dlsym(h, "app_indicator_set_title");
    g_ai_set_icon =
        (ai_set_icon_full_fn)dlsym(h, "app_indicator_set_icon_full");
    g_ai_set_label = (ai_set_label_fn)dlsym(h, "app_indicator_set_label");
    if (!ai_new || !ai_set_status || !ai_set_menu) {
        dlclose(h);
        return 0;
    }

    menu = gtk_menu_new();
    item = gtk_menu_item_new_with_label("Show / Hide Panel");
    g_signal_connect(item, "activate", G_CALLBACK(on_tray_show), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(item, "activate", G_CALLBACK(on_tray_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show_all(menu);

    /* APP_INDICATOR_CATEGORY_HARDWARE = 3, STATUS_ACTIVE = 1.
     * Icon resolution order: bundled repo dir, installed hicolor theme
     * (by name), stock battery icon as last resort. */
    g_indicator = NULL;
    icon = fopen(ICON_DIR "/power-reactor.svg", "r");
    if (icon) {
        fclose(icon);
        if (ai_new_path) {
            g_indicator = ai_new_path("power-reactor", "power-reactor",
                                      3, ICON_DIR);
            g_tray_has_path = g_indicator != NULL;
        }
    }
    if (!g_indicator) {
        char p[512];
        snprintf(p, sizeof p,
                 "%s/.local/share/icons/hicolor/scalable/apps"
                 "/power-reactor.svg",
                 getenv("HOME") ? getenv("HOME") : "");
        icon = fopen(p, "r");
        if (!icon)
            icon = fopen("/usr/share/icons/hicolor/scalable/apps"
                         "/power-reactor.svg", "r");
        if (icon) {
            fclose(icon);
            g_indicator = ai_new("power-reactor", "power-reactor", 3);
            g_tray_has_path = 1; /* theme lookup works for variants too */
        }
    }
    if (!g_indicator)
        g_indicator = ai_new("power-reactor", "battery-good-symbolic", 3);
    if (!g_indicator) {
        gtk_widget_destroy(menu);
        dlclose(h);
        return 0;
    }
    ai_set_status(g_indicator, 1);
    ai_set_menu(g_indicator, GTK_MENU(menu));
    if (g_ai_set_title)
        g_ai_set_title(g_indicator, "PWR-REACTOR");
    {
        ai_set_sec_target_fn sec =
            (ai_set_sec_target_fn)dlsym(h, "app_indicator_set_secondary_"
                                           "activate_target");
        GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
        if (sec && kids)
            sec(g_indicator, GTK_WIDGET(kids->data));
        g_list_free(kids);
    }
    return 1;
}

static void tray_pump(void)
{
    if (g_tray_ok)
        while (g_main_context_iteration(NULL, FALSE))
            ;
}

static void tray_update(void)
{
    char buf[64];
    int i, low = -1, state = 0;
    if (!g_tray_ok)
        return;
    for (i = 0; i < g_ndevs; i++) {
        Dev *dv = &g_devs[i];
        if (dv->kind == KIND_MAINS || dv->capacity < 0)
            continue;
        if (dv->kind == KIND_DEVBAT && (low < 0 || dv->capacity < low))
            low = dv->capacity;
        if (dv->capacity <= g_cfg.crit_pct)
            state = 2;
        else if (state < 1 && (dv->capacity <= g_cfg.warn_pct ||
                               !strcmp(dv->status, "CHG")))
            state = 1;
    }
    if (g_ai_set_icon && g_tray_has_path && state != g_tray_icon_state) {
        static const char *names[] = {
            "power-reactor", "power-reactor-amber", "power-reactor-red"
        };
        g_ai_set_icon(g_indicator, names[state], "PWR-REACTOR");
        g_tray_icon_state = state;
    }
    if (g_ai_set_label && g_cfg.tray_label) {
        if (low >= 0)
            snprintf(buf, sizeof buf, "%d%%", low);
        else
            buf[0] = 0;
        g_ai_set_label(g_indicator, buf, "100%");
    }
    if (g_ai_set_title) {
        snprintf(buf, sizeof buf, "PWR-REACTOR - %d sources", g_ndevs);
        g_ai_set_title(g_indicator, buf);
    }
}

#else
static int g_tray_ok = 0;
static int tray_init(void) { return 0; }
static void tray_pump(void) {}
static void tray_update(void) {}
#endif

/* When launched from a snap-packaged terminal (e.g. VSCode) the snap
 * exports GTK/GDK module paths that point inside its own snap and break
 * GTK for host binaries. Drop them if we inherited a foreign SNAP env. */
static void sanitize_snap_env(void)
{
    static const char *vars[] = {
        "GTK_PATH", "GTK_EXE_PREFIX", "GDK_PIXBUF_MODULE_FILE",
        "GDK_PIXBUF_MODULEDIR", "GSETTINGS_SCHEMA_DIR", "GIO_MODULE_DIR",
        "XDG_DATA_HOME", "LOCPATH",
    };
    size_t i;
    if (!getenv("SNAP"))
        return;
    for (i = 0; i < sizeof vars / sizeof vars[0]; i++)
        unsetenv(vars[i]);
    {
        const char *lp = getenv("LD_LIBRARY_PATH");
        if (lp && strstr(lp, "/snap/"))
            unsetenv("LD_LIBRARY_PATH");
    }
}

/* widget mode: the header strip doubles as a drag handle */
static SDL_HitTestResult widget_hit_test(SDL_Window *w, const SDL_Point *p,
                                         void *data)
{
    (void)w;
    (void)data;
    if (p->y < ROW_Y0)
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

static void panel_show(void)
{
    SDL_ShowWindow(g_win);
    SDL_RestoreWindow(g_win);
    SDL_RaiseWindow(g_win);
    g_visible = 1;
}

static void panel_hide(void)
{
    SDL_HideWindow(g_win);
    g_visible = 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    Uint32 last_scan = 0;
    int start_hidden = 0;
    int widget = 0;
    int persist;
    int i;

    config_load();
    g_amber_mode = g_cfg.amber ? 1 : 0;
    widget = g_cfg.widget ? 1 : 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hidden"))
            start_hidden = 1;
        else if (!strcmp(argv[i], "--amber"))
            g_amber_mode = 1;
        else if (!strcmp(argv[i], "--widget"))
            widget = 1;
    }

    sanitize_snap_env();
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    g_win = SDL_CreateWindow("PWR-REACTOR MK.II",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             WIN_W, WIN_H,
                             SDL_WINDOW_ALLOW_HIGHDPI
                             | (widget
                                ? (SDL_WINDOW_BORDERLESS |
                                   SDL_WINDOW_ALWAYS_ON_TOP |
                                   SDL_WINDOW_SKIP_TASKBAR)
                                : SDL_WINDOW_RESIZABLE)
                             | (start_hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!g_win) {
        fprintf(stderr, "window failed: %s\n", SDL_GetError());
        return 1;
    }
    if (widget) {
        int wx = g_cfg.widget_x, wy = g_cfg.widget_y;
        if (wx < 0 || wy < 0) {
            SDL_Rect ub;
            if (SDL_GetDisplayUsableBounds(0, &ub) == 0) {
                wx = ub.x + ub.w - WIN_W - 16;
                wy = ub.y + 40;
            } else {
                wx = 40;
                wy = 40;
            }
        }
        SDL_SetWindowPosition(g_win, wx, wy);
        SDL_SetWindowHitTest(g_win, widget_hit_test, NULL);
    }
    g_r = SDL_CreateRenderer(g_win, -1,
                             SDL_RENDERER_ACCELERATED |
                             SDL_RENDERER_PRESENTVSYNC);
    if (!g_r)
        g_r = SDL_CreateRenderer(g_win, -1, 0);
    if (!g_r) {
        fprintf(stderr, "renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(g_r, WIN_W, WIN_H);
    SDL_RenderSetIntegerScale(g_r, SDL_TRUE);
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);

    g_tray_ok = tray_init();
    persist = g_tray_ok || start_hidden;
    g_visible = !start_hidden;

    theme_apply();
    scan_devices();
    tray_update();
    srand((unsigned)time(NULL));

    while (g_running) {
        SDL_Event ev;
        Uint32 ticks = SDL_GetTicks();
        Station st[3] = {
            {{0, 0, 0, 0}, "HOLD",     &g_hold},
            {{0, 0, 0, 0}, "LAMP TST", &g_lamp_test},
            {{0, 0, 0, 0}, "PHOSPHOR", &g_amber_mode},
        };
        int nst = 3;

        if (g_got_signal)
            g_running = 0;

        station_layout(st, nst);
        tray_pump();

        if (g_want_show) {
            if (g_want_show == 2)
                panel_hide();
            else
                panel_show();
            g_want_show = 0;
        }

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                if (persist)
                    panel_hide();
                else
                    g_running = 0;
            } else if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE:
                    if (persist)
                        panel_hide();
                    else
                        g_running = 0;
                    break;
                case SDLK_q: g_running = 0; break;
                case SDLK_h: g_hold = !g_hold; break;
                case SDLK_l: g_lamp_test = !g_lamp_test; break;
                case SDLK_p:
                    g_amber_mode = !g_amber_mode;
                    theme_apply();
                    break;
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                SDL_Point p = {ev.button.x, ev.button.y};
                for (i = 0; i < nst; i++)
                    if (SDL_PointInRect(&p, &st[i].hit)) {
                        *st[i].value = !*st[i].value;
                        if (st[i].value == &g_amber_mode)
                            theme_apply();
                    }
            }
        }

        if (!g_hold && ticks - last_scan > (Uint32)g_cfg.scan_ms) {
            if (scan_devices() && g_cfg.popup_on_plug)
                panel_show(); /* new device battery plugged in */
            tray_update();
            last_scan = ticks;
        }

        if (!g_visible) {
            SDL_Delay(150);
            continue;
        }

        g_bright = 0.96f + 0.03f * sinf(ticks * 0.05f)
                 + 0.01f * ((float)rand() / RAND_MAX);
        if (g_bright > 1.0f)
            g_bright = 1.0f;

        draw_bezel();
        draw_header(ticks);
        {
            int max_rows = (BOT_Y - ROW_Y0 - 6) / ROW_H;
            int rows = g_ndevs < max_rows ? g_ndevs : max_rows;
            for (i = 0; i < rows; i++)
                draw_device_row(i, &g_devs[i], ticks, g_lamp_test);
            if (g_ndevs > max_rows) {
                char buf[32];
                snprintf(buf, sizeof buf, "+%d MORE SOURCES",
                         g_ndevs - max_rows);
                draw_text(SCREEN_X + 44, ROW_Y0 + max_rows * ROW_H, 1,
                          buf, g_theme.amber);
            }
            if (g_ndevs == 0)
                draw_text(SCREEN_X + 44, ROW_Y0 + 20, 2,
                          "NO POWER SOURCES DETECTED", g_theme.red);

            /* trend scope + load meter fill the space below the rows */
            {
                int sy = ROW_Y0 + rows * ROW_H + 8;
                int sh = BOT_Y - 12 - sy;
                if (sh >= 70) {
                    draw_scope(SCREEN_X + 14, sy, 408, sh);
                    draw_meter(SCREEN_X + 434, sy, SCREEN_W - 448, sh);
                }
            }
        }
        draw_bottom(st, nst, ticks);
        draw_crt_overlay();

        SDL_RenderPresent(g_r);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(g_r);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
