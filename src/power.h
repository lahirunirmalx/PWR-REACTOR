/*
 * power.h - public interface of the telemetry data layer.
 *
 * The UI layer talks to the model only through this header, so the
 * panel can be rewritten (SDL to GTK, and whatever comes next) without
 * touching device discovery.
 */
#ifndef POWER_H
#define POWER_H

#define MAX_DEVS 16
#define HIST_N   180 /* samples kept per device, one per scan (~6 min) */

/* what a power source is: the machine's own battery, a peripheral
 * battery (phone, mouse, earbuds, UPS) or a mains adapter */
enum { KIND_SYSBAT = 0, KIND_DEVBAT, KIND_MAINS };

typedef struct {
    int scan_ms;
    int warn_pct;
    int crit_pct;
    int popup_on_plug;
    int notify;
    int sound;
    int dark;        /* 0 follow desktop, 1 force dark */
    int tray_label;  /* lowest device percentage next to the tray icon */
    int compact;     /* compact window by default */
} Config;

typedef struct {
    char label[28];
    char tag[12];
    char status[8];  /* CHG, DIS, FULL, IDLE, ONLINE, OFF, -- */
    int  kind;
    int  capacity;   /* percent, -1 = no telemetry */
    int  online;
    long voltage_uv;
    long power_uw;
    int  est_min;    /* minutes to empty/full, -1 unknown */
    int  est_ttf;    /* 1 = time to full, 0 = time to empty */
} Dev;

typedef struct {
    char label[28];
    signed char cap[HIST_N]; /* -1 = no sample */
    int  head;
    int  used;
    int  active;             /* seen in the latest scan */
} Hist;

/* the live model, rebuilt by every power_scan() */
extern Config g_cfg;
extern Dev    g_devs[MAX_DEVS];
extern int    g_ndevs;
extern Hist   g_hist[MAX_DEVS];
extern int    g_nhist;
extern float  g_pwr_now;          /* latest total bus load, in watts */
extern int    g_upower_link;      /* 1 = upower answered, 0 = sysfs only */

/* alert sink, installed by the UI so notifications can go through the
 * toolkit (GNotification) instead of shelling out */
typedef void (*PowerNotifyFn)(const char *title, const char *body,
                              int critical, void *user);
void power_set_notifier(PowerNotifyFn fn, void *user);

void power_config_load(void);
void power_config_path(char *out, unsigned long n);

/* rescan every source; returns 1 when a device battery just appeared */
int power_scan(void);

/* derived values for the dashboard */
float power_total_load_w(void);
int   power_worst_pct(void);   /* lowest device percent, -1 none */
int   power_any_charging(void);

#endif /* POWER_H */
