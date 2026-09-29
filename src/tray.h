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

/* refresh icon, label and tooltip from the current model */
void tray_update(void);

#endif /* TRAY_H */
