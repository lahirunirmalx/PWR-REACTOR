/*
 * Unit tests for the toolkit-independent parts of the model: the
 * battery selection the tray rotates through, and the presentation
 * helpers the tray and dashboard share.
 *
 * Builds against src/power.c with no GTK and no real devices: the
 * tests write g_devs directly, which is the point - rotation order and
 * wraparound can be checked without owning a drawer full of phones.
 */
#include "../src/power.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void check(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        g_fail++;
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want)) {
        printf("FAIL: %s: got \"%s\", want \"%s\"\n", what, got, want);
        g_fail++;
    }
}

static void add(int kind, const char *label, int capacity,
                const char *status)
{
    Dev *d = &g_devs[g_ndevs++];
    memset(d, 0, sizeof *d);
    d->kind = kind;
    snprintf(d->label, sizeof d->label, "%s", label);
    snprintf(d->status, sizeof d->status, "%s", status);
    d->capacity = capacity;
    d->online = -1;
    d->est_min = -1;
}

static void test_battery_selection(void)
{
    int i, seen[4] = {0, 0, 0, 0};

    g_ndevs = 0;
    add(KIND_SYSBAT, "L20B2PF0",     87, "DIS");
    add(KIND_MAINS,  "line_power_AC", -1, "---"); /* no percentage */
    add(KIND_DEVBAT, "Pixel 7",      42, "CHG");
    add(KIND_DEVBAT, "MX Master",     9, "DIS");
    add(KIND_DEVBAT, "Old Headset",  -1, "---"); /* no telemetry */

    check(power_battery_count() == 3,
          "mains and telemetry-less devices are not rotated through");

    /* the rotation must visit every battery and skip the others */
    for (i = 0; i < power_battery_count(); i++) {
        int at = power_battery_at(i);
        check(at >= 0 && at < g_ndevs, "battery index in range");
        check(g_devs[at].kind != KIND_MAINS, "never rotates onto mains");
        check(g_devs[at].capacity >= 0, "never rotates onto no telemetry");
        if (!strcmp(g_devs[at].label, "L20B2PF0"))  seen[0] = 1;
        if (!strcmp(g_devs[at].label, "Pixel 7"))   seen[1] = 1;
        if (!strcmp(g_devs[at].label, "MX Master")) seen[2] = 1;
    }
    check(seen[0] && seen[1] && seen[2],
          "every battery is reachable by rotating");

    check(power_battery_at(3) == -1, "out of range index rejected");
    check(power_battery_at(-1) == -1, "negative index rejected");

    /* a machine with nothing attached must not select anything */
    g_ndevs = 0;
    check(power_battery_count() == 0, "no devices means nothing to show");
    check(power_battery_at(0) == -1, "empty model selects nothing");

    /* only mains: the label would be blank, so there is nothing to
     * rotate and the tray must fall back to its generic title */
    g_ndevs = 0;
    add(KIND_MAINS, "line_power_ADP0", -1, "---");
    check(power_battery_count() == 0, "mains alone is not a battery");
}

static void test_display_helpers(void)
{
    char name[32];

    g_ndevs = 0;
    add(KIND_MAINS,  "line_power_ADP0", -1, "---");
    add(KIND_DEVBAT, "Pixel_7_Pro",     42, "CHG");
    add(KIND_DEVBAT, "Buds",            55, "FUL");
    add(KIND_DEVBAT, "Mouse",           30, "IDL");
    add(KIND_SYSBAT, "BAT0",            77, "DIS");

    power_display_name(&g_devs[0], name, sizeof name);
    check_str(name, "AC adapter", "mains gets a readable name");

    power_display_name(&g_devs[1], name, sizeof name);
    check_str(name, "Pixel 7 Pro", "underscores become spaces");

    check_str(power_status_text(&g_devs[1]), "Charging", "CHG");
    check_str(power_status_text(&g_devs[2]), "Fully charged", "FUL");
    check_str(power_status_text(&g_devs[3]), "Not charging", "IDL");
    check_str(power_status_text(&g_devs[4]), "Discharging", "DIS");

    /* a mains adapter reports a link flag, not a charge state */
    g_devs[0].online = 1;
    check_str(power_status_text(&g_devs[0]), "Connected", "mains online");
    g_devs[0].online = 0;
    check_str(power_status_text(&g_devs[0]), "Not connected",
              "mains offline");
}

static void test_worst_and_charging(void)
{
    g_ndevs = 0;
    add(KIND_MAINS,  "line_power_AC", -1, "---");
    add(KIND_SYSBAT, "BAT0",          77, "DIS");
    add(KIND_DEVBAT, "Mouse",          9, "DIS");

    check(power_worst_pct() == 9, "worst percentage ignores mains");
    check(!power_any_charging(), "nothing is charging here");

    snprintf(g_devs[2].status, sizeof g_devs[2].status, "CHG");
    check(power_any_charging(), "a charging device is detected");

    g_ndevs = 0;
    check(power_worst_pct() == -1, "no batteries means no worst value");
}

int main(void)
{
    test_battery_selection();
    test_display_helpers();
    test_worst_and_charging();

    if (g_fail) {
        printf("\n%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
