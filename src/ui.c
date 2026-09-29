/*
 * ui.c - Power Reactor dashboard, GTK 3.
 *
 * GTK 3 is deliberate: it is the one toolkit present on every Ubuntu
 * from 20.04 LTS to 26.04 LTS, so a single binary carries the native
 * Yaru look across all of them. Every colour comes from the desktop
 * theme through palette_load() - nothing here is hardcoded, so the
 * dashboard follows light/dark and the user's accent automatically.
 *
 * Layout: a summary card (worst battery, charge state, time estimate,
 * bus load), a list of per-device rows, and a cairo trend chart of the
 * last few minutes of charge history.
 */

#define _GNU_SOURCE

#include "ui.h"
#include "power.h"
#include "tray.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define APP_ID "com.github.lahirunirmalx.PowerReactor"

/* geometry only - colours live in the theme */
static const char APP_CSS[] =
    ".dash-card {"
    "  background-color: @theme_base_color;"
    "  border: 1px solid @borders;"
    "  border-radius: 12px;"
    "}"
    ".dash-hero-pct { font-size: 32px; font-weight: 300; }"
    ".dash-hero-state { font-size: 13px; }"
    ".dash-stat-value { font-size: 15px; font-weight: bold; }"
    ".dash-name { font-weight: bold; }"
    ".dash-dim { opacity: 0.55; font-size: 90%; }"
    ".dash-pct { font-feature-settings: 'tnum'; }"
    ".dash-list { background-color: transparent; }"
    ".dash-list row { padding: 10px 12px; border-radius: 8px; }"
    "levelbar trough, levelbar block {"
    "  min-height: 7px; border-radius: 4px;"
    "}";

/* ------------------------------------------------------------------ */
/* palette - the single place colours are resolved                     */
/* ------------------------------------------------------------------ */

typedef struct {
    GdkRGBA fg;      /* body text */
    GdkRGBA dim;     /* grid lines, axes */
    GdkRGBA accent;  /* selection / brand */
    GdkRGBA ok;
    GdkRGBA warn;
    GdkRGBA err;
} Palette;

/* Look a colour up in the live style context. The fallback is only
 * reached on a theme that does not define the name, and is derived
 * from the theme's own foreground so it still tracks light/dark. */
static void palette_pick(GtkStyleContext *ctx, const char *name,
                         const GdkRGBA *fallback, GdkRGBA *out)
{
    if (!gtk_style_context_lookup_color(ctx, name, out))
        *out = *fallback;
}

static void palette_load(GtkWidget *w, Palette *p)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    GtkStateFlags st = gtk_widget_get_state_flags(w);

    gtk_style_context_get_color(ctx, st, &p->fg);

    p->dim = p->fg;
    p->dim.alpha = 0.18;

    palette_pick(ctx, "theme_selected_bg_color", &p->fg, &p->accent);
    palette_pick(ctx, "success_color", &p->accent, &p->ok);
    palette_pick(ctx, "warning_color", &p->accent, &p->warn);
    palette_pick(ctx, "error_color", &p->accent, &p->err);
}

/* distinct-but-themed series colours for the trend chart */
static void series_color(const Palette *p, int i, GdkRGBA *out)
{
    const GdkRGBA *ring[4];
    ring[0] = &p->accent;
    ring[1] = &p->ok;
    ring[2] = &p->warn;
    ring[3] = &p->err;
    *out = *ring[i & 3];
    /* second lap through the ring is drawn lighter so eight traces
     * stay tellable apart without inventing colours */
    if (i >= 4)
        out->alpha = 0.45;
}

/* ------------------------------------------------------------------ */
/* widgets                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char       label[28];
    char       iconname[64]; /* last name set, to skip lookups */
    GtkWidget *row;
    GtkWidget *icon;
    GtkWidget *name;
    GtkWidget *sub;
    GtkWidget *level;
    GtkWidget *pct;
} Row;

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *header;
    GtkWidget *stack;
    GtkWidget *list;
    GtkWidget *hero_icon;
    GtkWidget *hero_pct;
    GtkWidget *hero_state;
    GtkWidget *stat_load;
    GtkWidget *stat_sources;
    GtkWidget *chart;
    Row        rows[MAX_DEVS];
    int        nrows;
    guint      tick_id;
    gboolean   tray_ok;
} Ui;

static Ui g_ui;
static int g_start_hidden;

void ui_set_start_hidden(int hidden)
{
    g_start_hidden = hidden;
}

/* ------------------------------------------------------------------ */
/* formatting helpers                                                  */
/* ------------------------------------------------------------------ */

static void fmt_duration(int minutes, char *out, size_t n)
{
    if (minutes <= 0) {
        snprintf(out, n, "--");
        return;
    }
    if (minutes < 60)
        snprintf(out, n, "%d min", minutes);
    else if (minutes % 60 == 0)
        snprintf(out, n, "%d h", minutes / 60);
    else
        snprintf(out, n, "%d h %d min", minutes / 60, minutes % 60);
}

static const char *status_text(const Dev *dv)
{
    if (!strcmp(dv->status, "CHG"))
        return "Charging";
    if (!strcmp(dv->status, "DIS"))
        return "Discharging";
    if (!strcmp(dv->status, "FUL"))
        return "Fully charged";
    if (!strcmp(dv->status, "IDL"))
        return "Not charging";
    /* mains adapters carry no charge state, only a link flag */
    if (dv->kind == KIND_MAINS)
        return dv->online > 0 ? "Connected" : "Not connected";
    return dv->capacity < 0 ? "No telemetry" : "Unknown";
}

/* kernel and upower names are identifiers; soften them for display
 * without touching what the data layer stores */
static void display_name(const Dev *dv, char *out, size_t n)
{
    size_t i;

    if (dv->kind == KIND_MAINS && !strncmp(dv->label, "line_power", 10)) {
        snprintf(out, n, "AC adapter");
        return;
    }
    snprintf(out, n, "%.27s", dv->label);
    for (i = 0; out[i]; i++)
        if (out[i] == '_')
            out[i] = ' ';
}

static int str_has(const char *hay, const char *needle)
{
    return strcasestr(hay, needle) != NULL;
}

/* symbolic icon for a device, from its name and source tag */
static void device_icon_name(const Dev *dv, char *out, size_t n)
{
    char hint[48];
    int step;

    if (dv->kind == KIND_MAINS) {
        snprintf(out, n, "ac-adapter-symbolic");
        return;
    }

    snprintf(hint, sizeof hint, "%.23s %.11s", dv->label, dv->tag);

    if (str_has(hint, "ups") || str_has(hint, "nut"))
        snprintf(out, n, "uninterruptible-power-supply-symbolic");
    else if (str_has(hint, "mouse"))
        snprintf(out, n, "input-mouse-symbolic");
    else if (str_has(hint, "keyboard") || str_has(hint, "kbd"))
        snprintf(out, n, "input-keyboard-symbolic");
    else if (str_has(hint, "head") || str_has(hint, "bud") ||
             str_has(hint, "audio") || str_has(hint, "pods"))
        snprintf(out, n, "audio-headphones-symbolic");
    else if (str_has(hint, "pad") || str_has(hint, "controller") ||
             str_has(hint, "xbox") || str_has(hint, "dual"))
        snprintf(out, n, "input-gaming-symbolic");
    else if (str_has(hint, "phone") || str_has(hint, "adb") ||
             str_has(hint, "kde") || str_has(hint, "gsc") ||
             str_has(hint, "pixel") || str_has(hint, "galaxy"))
        snprintf(out, n, "phone-symbolic");
    else if (dv->capacity < 0)
        snprintf(out, n, "battery-missing-symbolic");
    else {
        step = dv->capacity / 10 * 10;
        if (step > 100)
            step = 100;
        if (!strcmp(dv->status, "CHG"))
            snprintf(out, n, "battery-level-%d-charging-symbolic", step);
        else if (step == 100)
            snprintf(out, n, "battery-level-100-charged-symbolic");
        else
            snprintf(out, n, "battery-level-%d-symbolic", step);
    }
}

/* fall back to a always-present icon if the theme lacks the exact one */
static void set_icon(GtkWidget *img, const char *name, GtkIconSize size)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    const char *use = gtk_icon_theme_has_icon(theme, name)
                    ? name : "battery-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(img), use, size);
}

/* ------------------------------------------------------------------ */
/* trend chart                                                         */
/* ------------------------------------------------------------------ */

static gboolean on_chart_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    Palette pal;
    GtkAllocation a;
    int i, g, series = 0;
    double pad_l = 30, pad_r = 8, pad_t = 8, pad_b = 16;
    double gw, gh;

    (void)data;
    palette_load(w, &pal);
    gtk_widget_get_allocation(w, &a);
    gw = a.width - pad_l - pad_r;
    gh = a.height - pad_t - pad_b;
    if (gw <= 10 || gh <= 10)
        return FALSE;

    /* horizontal grid at 0 / 50 / 100 percent */
    cairo_set_line_width(cr, 1.0);
    for (g = 0; g <= 2; g++) {
        double y = pad_t + gh - gh * g / 2.0;
        cairo_set_source_rgba(cr, pal.dim.red, pal.dim.green,
                              pal.dim.blue, pal.dim.alpha);
        cairo_move_to(cr, pad_l, floor(y) + 0.5);
        cairo_line_to(cr, pad_l + gw, floor(y) + 0.5);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, pal.fg.red, pal.fg.green, pal.fg.blue,
                              0.45);
        cairo_set_font_size(cr, 10);
        cairo_move_to(cr, 2, y + 3);
        cairo_show_text(cr, g == 0 ? "0%" : g == 1 ? "50%" : "100%");
    }

    /* one trace per battery, oldest sample on the left */
    for (i = 0; i < g_nhist && series < 8; i++) {
        const Hist *h = &g_hist[i];
        GdkRGBA c;
        int k, started = 0;
        if (!h->active || h->used < 2)
            continue;
        series_color(&pal, series, &c);
        cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha);
        cairo_set_line_width(cr, 2.0);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        for (k = 0; k < h->used; k++) {
            int idx = (h->head - h->used + k + HIST_N) % HIST_N;
            int cap = h->cap[idx];
            double x, y;
            if (cap < 0) {
                started = 0;
                continue;
            }
            x = pad_l + gw * k / (double)(HIST_N - 1);
            y = pad_t + gh - gh * cap / 100.0;
            if (!started) {
                cairo_move_to(cr, x, y);
                started = 1;
            } else {
                cairo_line_to(cr, x, y);
            }
        }
        cairo_stroke(cr);
        series++;
    }

    if (!series) {
        cairo_set_source_rgba(cr, pal.fg.red, pal.fg.green, pal.fg.blue,
                              0.5);
        cairo_set_font_size(cr, 11);
        cairo_move_to(cr, pad_l + 4, pad_t + gh / 2);
        cairo_show_text(cr, "Collecting history...");
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* device rows                                                         */
/* ------------------------------------------------------------------ */

static GtkWidget *row_new(Row *r)
{
    GtkWidget *box, *text, *right;

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    r->icon = gtk_image_new_from_icon_name("battery-symbolic",
                                           GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_box_pack_start(GTK_BOX(box), r->icon, FALSE, FALSE, 0);

    text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    r->name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(r->name), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(r->name), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(r->name),
                                "dash-name");
    r->sub = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(r->sub), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(r->sub), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(r->sub),
                                "dash-dim");
    gtk_box_pack_start(GTK_BOX(text), r->name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), r->sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);

    right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(right, 132, -1);
    r->pct = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(r->pct), 1.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(r->pct),
                                "dash-pct");
    r->level = gtk_level_bar_new_for_interval(0.0, 100.0);
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(r->level),
                                   GTK_LEVEL_BAR_OFFSET_LOW, 15.0);
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(r->level),
                                   GTK_LEVEL_BAR_OFFSET_HIGH, 40.0);
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(r->level),
                                   GTK_LEVEL_BAR_OFFSET_FULL, 100.0);
    gtk_box_pack_start(GTK_BOX(right), r->pct, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), r->level, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), right, FALSE, FALSE, 0);

    r->row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r->row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(r->row), FALSE);
    gtk_container_add(GTK_CONTAINER(r->row), box);
    gtk_widget_show_all(r->row);
    return r->row;
}

static void row_bind(Row *r, const Dev *dv)
{
    char icon[64], sub[192], pct[16], est[32], name[32];
    int n = 0;

    device_icon_name(dv, icon, sizeof icon);
    if (strcmp(icon, r->iconname)) {
        set_icon(r->icon, icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
        snprintf(r->iconname, sizeof r->iconname, "%s", icon);
    }
    display_name(dv, name, sizeof name);
    gtk_label_set_text(GTK_LABEL(r->name), name);

    n += snprintf(sub + n, sizeof sub - n, "%s", status_text(dv));
    if (n >= (int)sizeof sub)
        n = (int)sizeof sub - 1;
    if (dv->voltage_uv > 0)
        n += snprintf(sub + n, sizeof sub - n, "  -  %.2f V",
                      dv->voltage_uv / 1e6);
    if (dv->power_uw > 0)
        n += snprintf(sub + n, sizeof sub - n, "  -  %.1f W",
                      dv->power_uw / 1e6);
    if (dv->est_min > 0) {
        fmt_duration(dv->est_min, est, sizeof est);
        n += snprintf(sub + n, sizeof sub - n, "  -  %s %s", est,
                      dv->est_ttf ? "to full" : "left");
    }
    if (dv->tag[0])
        snprintf(sub + n, sizeof sub - n, "  -  %s", dv->tag);
    gtk_label_set_text(GTK_LABEL(r->sub), sub);

    if (dv->capacity >= 0) {
        snprintf(pct, sizeof pct, "%d%%", dv->capacity);
        gtk_widget_set_sensitive(r->level, TRUE);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(r->level), dv->capacity);
        gtk_widget_set_visible(r->level, TRUE);
    } else {
        snprintf(pct, sizeof pct, "%s",
                 dv->kind == KIND_MAINS ? "AC" : "--");
        gtk_level_bar_set_value(GTK_LEVEL_BAR(r->level), 0);
        gtk_widget_set_visible(r->level, FALSE);
    }
    gtk_label_set_text(GTK_LABEL(r->pct), pct);
    snprintf(r->label, sizeof r->label, "%.27s", dv->label);
}

/* the row set only changes when devices come and go, so the common
 * case is an in-place update with no widget churn */
static int rows_match_model(const Ui *ui)
{
    int i;
    if (ui->nrows != g_ndevs)
        return 0;
    for (i = 0; i < g_ndevs; i++)
        if (strcmp(ui->rows[i].label, g_devs[i].label))
            return 0;
    return 1;
}

static void rows_rebuild(Ui *ui)
{
    GList *kids, *l;
    int i;

    kids = gtk_container_get_children(GTK_CONTAINER(ui->list));
    for (l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    ui->nrows = g_ndevs > MAX_DEVS ? MAX_DEVS : g_ndevs;
    for (i = 0; i < ui->nrows; i++) {
        memset(&ui->rows[i], 0, sizeof ui->rows[i]);
        gtk_container_add(GTK_CONTAINER(ui->list), row_new(&ui->rows[i]));
        row_bind(&ui->rows[i], &g_devs[i]);
    }
}

/* ------------------------------------------------------------------ */
/* dashboard refresh                                                   */
/* ------------------------------------------------------------------ */

static void hero_update(Ui *ui)
{
    char buf[160], est[32], icon[64], name[32];
    int worst = power_worst_pct();
    int charging = power_any_charging();
    const Dev *lead = NULL;
    int i;

    /* the hero reflects the battery that needs attention first */
    for (i = 0; i < g_ndevs; i++) {
        if (g_devs[i].kind == KIND_MAINS || g_devs[i].capacity < 0)
            continue;
        if (!lead || g_devs[i].capacity < lead->capacity)
            lead = &g_devs[i];
    }

    if (worst >= 0) {
        snprintf(buf, sizeof buf, "%d%%", worst);
        gtk_label_set_text(GTK_LABEL(ui->hero_pct), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(ui->hero_pct), "--");
    }

    if (lead) {
        device_icon_name(lead, icon, sizeof icon);
        set_icon(ui->hero_icon, icon, GTK_ICON_SIZE_DIALOG);
        display_name(lead, name, sizeof name);
        if (lead->est_min > 0) {
            fmt_duration(lead->est_min, est, sizeof est);
            snprintf(buf, sizeof buf, "%s  -  %s %s", name, est,
                     lead->est_ttf ? "until full" : "remaining");
        } else {
            snprintf(buf, sizeof buf, "%s  -  %s", name,
                     status_text(lead));
        }
    } else {
        set_icon(ui->hero_icon, charging ? "ac-adapter-symbolic"
                                         : "battery-missing-symbolic",
                 GTK_ICON_SIZE_DIALOG);
        snprintf(buf, sizeof buf, "%s",
                 g_ndevs ? "No battery reports a charge level"
                         : "Waiting for a power source");
    }
    gtk_label_set_text(GTK_LABEL(ui->hero_state), buf);

    snprintf(buf, sizeof buf, "%.1f W", power_total_load_w());
    gtk_label_set_text(GTK_LABEL(ui->stat_load), buf);
    snprintf(buf, sizeof buf, "%d", g_ndevs);
    gtk_label_set_text(GTK_LABEL(ui->stat_sources), buf);

    snprintf(buf, sizeof buf, "%d source%s  -  %s", g_ndevs,
             g_ndevs == 1 ? "" : "s",
             g_upower_link ? "upower" : "sysfs");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(ui->header), buf);
}

static void dashboard_refresh(Ui *ui)
{
    if (!rows_match_model(ui))
        rows_rebuild(ui);
    else {
        int i;
        for (i = 0; i < ui->nrows; i++)
            row_bind(&ui->rows[i], &g_devs[i]);
    }

    gtk_stack_set_visible_child_name(GTK_STACK(ui->stack),
                                     g_ndevs ? "dash" : "empty");
    hero_update(ui);
    gtk_widget_queue_draw(ui->chart);
}

static void window_present(Ui *ui)
{
    gtk_window_present(GTK_WINDOW(ui->window));
}

static gboolean on_tick(gpointer data)
{
    Ui *ui = data;
    int popped = power_scan();

    tray_update();
    if (gtk_widget_get_visible(ui->window))
        dashboard_refresh(ui);
    if (popped && g_cfg.popup_on_plug)
        window_present(ui);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* notifications                                                       */
/* ------------------------------------------------------------------ */

static void play_alert_sound(int critical)
{
    char *argv[3];
    char path[128];

    if (!g_cfg.sound)
        return;
    snprintf(path, sizeof path,
             "/usr/share/sounds/freedesktop/stereo/%s.oga",
             critical ? "dialog-error" : "dialog-warning");
    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return;
    /* spawned as an argv vector, never through a shell */
    argv[0] = (char *)"paplay";
    argv[1] = path;
    argv[2] = NULL;
    g_spawn_async(NULL, argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                  G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

static void on_power_notify(const char *title, const char *body,
                            int critical, void *user)
{
    Ui *ui = user;
    GNotification *n = g_notification_new(title);

    g_notification_set_body(n, body);
    g_notification_set_priority(n, critical
                                ? G_NOTIFICATION_PRIORITY_URGENT
                                : G_NOTIFICATION_PRIORITY_NORMAL);
    {
        GIcon *ic = g_themed_icon_new("power-reactor");
        g_notification_set_icon(n, ic);
        g_object_unref(ic);
    }
    {
        /* one id per device, so a low phone does not replace a low
         * laptop in the notification tray */
        char *id = g_strdup_printf("battery-%s-%s",
                                   critical ? "crit" : "low", title);
        g_application_send_notification(G_APPLICATION(ui->app), id, n);
        g_free(id);
    }
    g_object_unref(n);
    play_alert_sound(critical);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

static void act_refresh(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)a;
    (void)p;
    /* a real rescan, but without a history sample: this scan is off the
     * timer cadence the slope estimate assumes */
    power_scan_ex(0);
    tray_update();
    dashboard_refresh(data);
}

static void act_about(GSimpleAction *a, GVariant *p, gpointer data)
{
    Ui *ui = data;
    const char *authors[] = {"lahiru", NULL};

    (void)a;
    (void)p;
    gtk_show_about_dialog(GTK_WINDOW(ui->window),
        "program-name", "Power Reactor",
        "logo-icon-name", "power-reactor",
        "version", APP_VERSION,
        "comments", "Battery and power telemetry for every connected "
                    "device: laptop, phones, peripherals and UPS units.",
        "website", "https://github.com/lahirunirmalx/PWR-REACTOR",
        "license-type", GTK_LICENSE_MIT_X11,
        "authors", authors,
        NULL);
}

static void act_quit(GSimpleAction *a, GVariant *p, gpointer data)
{
    Ui *ui = data;

    (void)a;
    (void)p;
    if (ui->tick_id) {
        g_source_remove(ui->tick_id);
        ui->tick_id = 0;
    }
    g_application_quit(G_APPLICATION(ui->app));
}

static void act_toggle(GSimpleAction *a, GVariant *p, gpointer data)
{
    Ui *ui = data;

    (void)a;
    (void)p;
    if (gtk_widget_get_visible(ui->window))
        gtk_widget_hide(ui->window);
    else
        window_present(ui);
}

/* closing the window keeps the app alive in the tray */
static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    Ui *ui = data;

    (void)e;
    if (!ui->tray_ok)
        return FALSE; /* no tray to hide into: really quit */
    gtk_widget_hide(w);
    return TRUE;
}

static void tray_toggle_cb(gpointer user) { act_toggle(NULL, NULL, user); }
static void tray_quit_cb(gpointer user)   { act_quit(NULL, NULL, user); }

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

static GtkWidget *stat_new(const char *caption, GtkWidget **value)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *cap = gtk_label_new(caption);

    *value = gtk_label_new("--");
    gtk_label_set_xalign(GTK_LABEL(*value), 1.0);
    gtk_label_set_xalign(GTK_LABEL(cap), 1.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(*value),
                                "dash-stat-value");
    gtk_style_context_add_class(gtk_widget_get_style_context(cap),
                                "dash-dim");
    gtk_box_pack_start(GTK_BOX(box), *value, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), cap, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *hero_new(Ui *ui)
{
    GtkWidget *card, *box, *text, *stats;

    card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                "dash-card");

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    g_object_set(box, "margin", 16, NULL);

    ui->hero_icon = gtk_image_new_from_icon_name("battery-symbolic",
                                                 GTK_ICON_SIZE_DIALOG);
    gtk_box_pack_start(GTK_BOX(box), ui->hero_icon, FALSE, FALSE, 0);

    text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    ui->hero_pct = gtk_label_new("--");
    gtk_label_set_xalign(GTK_LABEL(ui->hero_pct), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->hero_pct),
                                "dash-hero-pct");
    ui->hero_state = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ui->hero_state), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(ui->hero_state),
                            PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(ui->hero_state), "dash-hero-state");
    gtk_box_pack_start(GTK_BOX(text), ui->hero_pct, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), ui->hero_state, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);

    stats = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_valign(stats, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(stats),
                       stat_new("Bus load", &ui->stat_load),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(stats),
                       stat_new("Sources", &ui->stat_sources),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), stats, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(card), box, TRUE, TRUE, 0);
    return card;
}

static GtkWidget *chart_card_new(Ui *ui)
{
    GtkWidget *card, *box, *title;

    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                "dash-card");

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    g_object_set(box, "margin", 14, NULL);

    title = gtk_label_new("Charge trend");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(title),
                                "dash-name");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    ui->chart = gtk_drawing_area_new();
    gtk_widget_set_size_request(ui->chart, -1, 110);
    g_signal_connect(ui->chart, "draw", G_CALLBACK(on_chart_draw), NULL);
    gtk_box_pack_start(GTK_BOX(box), ui->chart, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(card), box, TRUE, TRUE, 0);
    return card;
}

static GtkWidget *empty_page_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *img = gtk_image_new_from_icon_name(
        "battery-missing-symbolic", GTK_ICON_SIZE_DIALOG);
    GtkWidget *t1 = gtk_label_new("No power sources detected");
    GtkWidget *t2 = gtk_label_new(
        "Connect a device with a battery, or install upower "
        "for richer telemetry.");

    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(t1),
                                "dash-name");
    gtk_style_context_add_class(gtk_widget_get_style_context(t2),
                                "dash-dim");
    gtk_label_set_line_wrap(GTK_LABEL(t2), TRUE);
    gtk_label_set_justify(GTK_LABEL(t2), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), t1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), t2, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *dash_page_new(Ui *ui)
{
    GtkWidget *scroll, *page, *list_card, *list_box, *title;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    g_object_set(page, "margin", 16, NULL);

    gtk_box_pack_start(GTK_BOX(page), hero_new(ui), FALSE, FALSE, 0);

    list_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(list_card),
                                "dash-card");
    list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    g_object_set(list_box, "margin", 10, NULL);
    title = gtk_label_new("Power sources");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    g_object_set(title, "margin-start", 4, "margin-top", 4, NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(title),
                                "dash-name");
    gtk_box_pack_start(GTK_BOX(list_box), title, FALSE, FALSE, 0);

    ui->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->list),
                                    GTK_SELECTION_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->list),
                                "dash-list");
    gtk_box_pack_start(GTK_BOX(list_box), ui->list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(list_card), list_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), list_card, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), chart_card_new(ui), FALSE, FALSE, 0);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), page);
    return scroll;
}

static void header_build(Ui *ui)
{
    GtkWidget *menu_btn, *refresh;
    GMenu *menu;

    ui->header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(ui->header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(ui->header), "Power Reactor");

    refresh = gtk_button_new_from_icon_name("view-refresh-symbolic",
                                            GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(refresh, "Refresh now");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(refresh), "app.refresh");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(ui->header), refresh);

    menu = g_menu_new();
    g_menu_append(menu, "Refresh now", "app.refresh");
    g_menu_append(menu, "About Power Reactor", "app.about");
    g_menu_append(menu, "Quit", "app.quit");

    menu_btn = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(menu_btn),
        gtk_image_new_from_icon_name("open-menu-symbolic",
                                     GTK_ICON_SIZE_BUTTON));
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn),
                                   G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(menu_btn, "Main menu");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(ui->header), menu_btn);
    g_object_unref(menu);
}

static void css_load(void)
{
    GtkCssProvider *css = gtk_css_provider_new();

    gtk_css_provider_load_from_data(css, APP_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void on_activate(GtkApplication *app, gpointer data)
{
    Ui *ui = data;

    if (ui->window) {
        window_present(ui);
        return;
    }

    css_load();
    if (g_cfg.dark)
        g_object_set(gtk_settings_get_default(),
                     "gtk-application-prefer-dark-theme", TRUE, NULL);

    ui->window = gtk_application_window_new(app);
    gtk_window_set_icon_name(GTK_WINDOW(ui->window), "power-reactor");
    gtk_window_set_default_size(GTK_WINDOW(ui->window),
                                g_cfg.compact ? 460 : 720,
                                g_cfg.compact ? 520 : 620);
    header_build(ui);
    gtk_window_set_titlebar(GTK_WINDOW(ui->window), ui->header);
    g_signal_connect(ui->window, "delete-event",
                     G_CALLBACK(on_window_delete), ui);

    ui->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui->stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(GTK_STACK(ui->stack), dash_page_new(ui), "dash");
    gtk_stack_add_named(GTK_STACK(ui->stack), empty_page_new(), "empty");
    gtk_container_add(GTK_CONTAINER(ui->window), ui->stack);

    /* Show the contents but map the window itself only when it is
     * wanted: show_all() followed by hide() paints one frame, which
     * flashes on every login when the service starts us hidden. The
     * window stays registered with the application either way, so the
     * process lives on in the tray. */
    gtk_widget_show_all(ui->stack);
    /* without a tray icon there is no way back to a hidden window, so
     * --hidden is ignored rather than stranding the app off screen */
    if (!g_start_hidden || !ui->tray_ok)
        gtk_widget_show(ui->window);
    g_start_hidden = 0;
    dashboard_refresh(ui);
}

static const GActionEntry APP_ACTIONS[] = {
    {"refresh", act_refresh, NULL, NULL, NULL, {0}},
    {"about",   act_about,   NULL, NULL, NULL, {0}},
    {"quit",    act_quit,    NULL, NULL, NULL, {0}},
    {"toggle",  act_toggle,  NULL, NULL, NULL, {0}},
};

static void on_startup(GtkApplication *app, gpointer data)
{
    Ui *ui = data;
    static const char *quit_keys[]    = {"<Primary>q", NULL};
    static const char *refresh_keys[] = {"<Primary>r", "F5", NULL};

    g_action_map_add_action_entries(G_ACTION_MAP(app), APP_ACTIONS,
                                    G_N_ELEMENTS(APP_ACTIONS), ui);
    gtk_application_set_accels_for_action(app, "app.quit", quit_keys);
    gtk_application_set_accels_for_action(app, "app.refresh",
                                          refresh_keys);

    {
        TrayCallbacks cb;
        cb.toggle = tray_toggle_cb;
        cb.quit = tray_quit_cb;
        cb.user = ui;
        ui->tray_ok = tray_init(&cb);
    }

    power_set_notifier(on_power_notify, ui);
    power_scan();
    tray_update();
    ui->tick_id = g_timeout_add(g_cfg.scan_ms, on_tick, ui);
}

int ui_run(int argc, char **argv)
{
    int status;

    g_ui.app = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(g_ui.app, "startup", G_CALLBACK(on_startup), &g_ui);
    g_signal_connect(g_ui.app, "activate", G_CALLBACK(on_activate),
                     &g_ui);

    status = g_application_run(G_APPLICATION(g_ui.app), argc, argv);
    g_object_unref(g_ui.app);
    return status;
}
