/*
 * tray.h - StatusNotifier tray icon.
 *
 * libayatana-appindicator is loaded with dlopen, so the app builds and
 * runs on desktops that do not ship it (the tray is simply absent).
 */
#ifndef TRAY_H
#define TRAY_H

#include <gtk/gtk.h>

typedef struct {
    void (*toggle)(gpointer user); /* menu item / middle click */
    void (*quit)(gpointer user);
    gpointer user;
} TrayCallbacks;

/* returns 1 when a tray icon was created */
int  tray_init(const TrayCallbacks *cb);

/* refresh icon, label and hover text from the current model */
void tray_update(void);

/* Advance the label to the next battery. Returns 1 when the selection
 * actually moved, so the caller can skip a redundant refresh with only
 * one battery present. */
int tray_cycle_next(void);

#endif /* TRAY_H */
