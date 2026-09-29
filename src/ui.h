/*
 * ui.h - GTK dashboard entry point.
 */
#ifndef UI_H
#define UI_H

/* start minimised to the tray instead of showing the window */
void ui_set_start_hidden(int hidden);

/* builds the application and runs the GTK main loop */
int ui_run(int argc, char **argv);

#endif /* UI_H */
