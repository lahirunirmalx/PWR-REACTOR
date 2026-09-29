#include "tray.h"
#include "power.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#ifndef ICON_DIR
#define ICON_DIR "."
#endif

/* app_indicator entry points, resolved at runtime */
typedef void *(*ai_new_fn)(const char *, const char *, int);
typedef void *(*ai_new_path_fn)(const char *, const char *, int,
                                const char *);
typedef void (*ai_set_status_fn)(void *, int);
typedef void (*ai_set_menu_fn)(void *, GtkMenu *);
typedef void (*ai_set_title_fn)(void *, const char *);
typedef void (*ai_set_icon_full_fn)(void *, const char *, const char *);
typedef void (*ai_set_label_fn)(void *, const char *, const char *);
typedef void (*ai_set_sec_target_fn)(void *, GtkWidget *);

static int    g_tray_ok;
static void  *g_indicator;
static int    g_tray_icon_state = -1; /* 0 green, 1 amber, 2 red */
static int    g_tray_has_path;
static TrayCallbacks g_cb;

static ai_set_title_fn     g_ai_set_title;
static ai_set_icon_full_fn g_ai_set_icon;
static ai_set_label_fn     g_ai_set_label;

static void on_tray_show(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    if (g_cb.toggle)
        g_cb.toggle(g_cb.user);
}

static void on_tray_quit(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    (void)data;
    if (g_cb.quit)
        g_cb.quit(g_cb.user);
}

/* true when the named icon file is readable */
static int icon_readable(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

int tray_init(const TrayCallbacks *cb)
{
    void *h;
    ai_new_fn ai_new;
    ai_new_path_fn ai_new_path;
    ai_set_status_fn ai_set_status;
    ai_set_menu_fn ai_set_menu;
    GtkWidget *menu, *item;

    g_cb = *cb;

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
    item = gtk_menu_item_new_with_label("Show Power Reactor");
    g_signal_connect(item, "activate", G_CALLBACK(on_tray_show), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(item, "activate", G_CALLBACK(on_tray_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show_all(menu);

    /* APP_INDICATOR_CATEGORY_HARDWARE = 3, STATUS_ACTIVE = 1.
     * Icon resolution order: bundled repo dir, installed hicolor theme
     * (by name), stock battery icon as last resort. */
    if (icon_readable(ICON_DIR "/power-reactor.svg") && ai_new_path) {
        g_indicator = ai_new_path("power-reactor", "power-reactor", 3,
                                  ICON_DIR);
        g_tray_has_path = g_indicator != NULL;
    }
    if (!g_indicator) {
        char p[512];
        const char *home = g_get_home_dir();
        snprintf(p, sizeof p,
                 "%s/.local/share/icons/hicolor/scalable/apps"
                 "/power-reactor.svg", home ? home : "");
        if (icon_readable(p) ||
            icon_readable("/usr/share/icons/hicolor/scalable/apps"
                          "/power-reactor.svg")) {
            g_indicator = ai_new("power-reactor", "power-reactor", 3);
            g_tray_has_path = 1; /* theme lookup finds the variants too */
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
        g_ai_set_title(g_indicator, "Power Reactor");

    /* middle click on the tray icon toggles the window */
    {
        ai_set_sec_target_fn sec =
            (ai_set_sec_target_fn)dlsym(h, "app_indicator_set_secondary_"
                                           "activate_target");
        GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
        if (sec && kids)
            sec(g_indicator, GTK_WIDGET(kids->data));
        g_list_free(kids);
    }

    g_tray_ok = 1;
    return 1;
}

void tray_update(void)
{
    char buf[64];
    int i, low = -1, state = 0;

    if (!g_tray_ok)
        return;

    for (i = 0; i < g_ndevs; i++) {
        const Dev *dv = &g_devs[i];
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
        g_ai_set_icon(g_indicator, names[state], "Power Reactor");
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
        snprintf(buf, sizeof buf, "Power Reactor - %d source%s",
                 g_ndevs, g_ndevs == 1 ? "" : "s");
        g_ai_set_title(g_indicator, buf);
    }
}
