/*
 * Power Reactor - battery and power telemetry for every device your
 * machine can see: laptop, phones, wireless peripherals, UPS units.
 *
 * Built on GTK 3, which is the one toolkit shipped by every Ubuntu
 * release from 20.04 LTS through 26.04 LTS, so one binary gets the
 * native Yaru look and dark mode on all of them.
 *
 * Build:  make
 * Deps :  GTK 3, upower (recommended), libayatana-appindicator (tray)
 *
 * Flags:  --hidden   start minimised to the tray
 *         --compact  smaller default window
 *         --dark     force the dark theme
 *
 * Config: ~/.config/power-reactor.conf, created on first run.
 */

#include "power.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A snap-packaged terminal (VS Code, for one) exports GTK module paths
 * that point inside its own snap and break GTK for host binaries. Drop
 * them when we inherited a foreign SNAP environment. */
static void sanitize_snap_env(void)
{
    static const char *vars[] = {
        "GTK_PATH", "GTK_EXE_PREFIX", "GDK_PIXBUF_MODULE_FILE",
        "GDK_PIXBUF_MODULEDIR", "GSETTINGS_SCHEMA_DIR", "GIO_MODULE_DIR",
        "XDG_DATA_HOME", "LOCPATH",
    };
    const char *lp, *name;
    size_t i;

    if (!getenv("SNAP"))
        return;
    /* SNAP is also set inside our own snap, where these variables are
     * exactly what the gnome extension needs. Only strip a foreign one. */
    name = getenv("SNAP_NAME");
    if (name && !strcmp(name, "power-reactor"))
        return;
    for (i = 0; i < sizeof vars / sizeof vars[0]; i++)
        unsetenv(vars[i]);
    lp = getenv("LD_LIBRARY_PATH");
    if (lp && strstr(lp, "/snap/"))
        unsetenv("LD_LIBRARY_PATH");
}

static void usage(const char *prog)
{
    printf("Usage: %s [--hidden] [--compact] [--dark] [--version]\n\n"
           "  --hidden   start minimised to the tray\n"
           "  --compact  smaller default window\n"
           "  --dark     force the dark theme\n"
           "  --version  print the version and exit\n", prog);
}

int main(int argc, char **argv)
{
    int hidden = 0;
    int i;

    power_config_load();

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hidden"))
            hidden = 1;
        else if (!strcmp(argv[i], "--compact"))
            g_cfg.compact = 1;
        else if (!strcmp(argv[i], "--dark"))
            g_cfg.dark = 1;
        else if (!strcmp(argv[i], "--version")) {
            printf("Power Reactor %s\n", APP_VERSION);
            return 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    sanitize_snap_env();
    ui_set_start_hidden(hidden);

    /* GTK gets only the program name: the flags above are ours, and
     * GApplication would reject them */
    return ui_run(1, argv);
}
