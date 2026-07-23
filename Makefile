VERSION := 0.9.0
CC      ?= cc
CFLAGS  += -O2 -Wall -Wextra -std=gnu99 $(shell pkg-config --cflags sdl2)
LDLIBS  += $(shell pkg-config --libs sdl2) -lm

PREFIX  ?= $(HOME)/.local

# tray icon support (GTK3 + dlopen'd libayatana-appindicator)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
ifneq ($(GTK_CFLAGS),)
CFLAGS += -DUSE_TRAY -DICON_DIR='"$(CURDIR)/icons"' $(GTK_CFLAGS)
LDLIBS += $(shell pkg-config --libs gtk+-3.0) -ldl
endif

power_reactor: main.c
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

run: power_reactor
	./power_reactor

# address/UB sanitizer build for testing
asan: main.c
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -o power_reactor_asan \
	  main.c $(LDLIBS)

install: power_reactor
	install -Dm755 power_reactor $(PREFIX)/bin/power_reactor
	install -Dm644 icons/power-reactor.svg \
	  $(PREFIX)/share/icons/hicolor/scalable/apps/power-reactor.svg
	install -Dm644 icons/power-reactor-amber.svg \
	  $(PREFIX)/share/icons/hicolor/scalable/apps/power-reactor-amber.svg
	install -Dm644 icons/power-reactor-red.svg \
	  $(PREFIX)/share/icons/hicolor/scalable/apps/power-reactor-red.svg
	mkdir -p $(PREFIX)/share/applications
	sed "s|@BIN@|$(PREFIX)/bin/power_reactor|" power-reactor.desktop \
	  > $(PREFIX)/share/applications/power-reactor.desktop
	-gtk-update-icon-cache -q $(PREFIX)/share/icons/hicolor 2>/dev/null

uninstall:
	rm -f $(PREFIX)/bin/power_reactor
	rm -f $(PREFIX)/share/icons/hicolor/scalable/apps/power-reactor*.svg
	rm -f $(PREFIX)/share/applications/power-reactor.desktop

# service uses the installed binary when present, else the repo build
BIN_PATH = $(shell [ -x $(PREFIX)/bin/power_reactor ] \
	   && echo $(PREFIX)/bin/power_reactor \
	   || echo $(CURDIR)/power_reactor)

install-service: power_reactor
	mkdir -p $(HOME)/.config/systemd/user
	sed "s|@BIN@|$(BIN_PATH)|" power-reactor.service \
	  > $(HOME)/.config/systemd/user/power-reactor.service
	systemctl --user daemon-reload
	systemctl --user enable --now power-reactor.service

uninstall-service:
	systemctl --user disable --now power-reactor.service || true
	rm -f $(HOME)/.config/systemd/user/power-reactor.service
	systemctl --user daemon-reload

deb: power_reactor
	rm -rf build/pkg
	install -Dm755 power_reactor build/pkg/usr/bin/power_reactor
	install -Dm644 icons/power-reactor.svg \
	  build/pkg/usr/share/icons/hicolor/scalable/apps/power-reactor.svg
	install -Dm644 icons/power-reactor-amber.svg \
	  build/pkg/usr/share/icons/hicolor/scalable/apps/power-reactor-amber.svg
	install -Dm644 icons/power-reactor-red.svg \
	  build/pkg/usr/share/icons/hicolor/scalable/apps/power-reactor-red.svg
	mkdir -p build/pkg/usr/share/applications
	sed "s|@BIN@|/usr/bin/power_reactor|" power-reactor.desktop \
	  > build/pkg/usr/share/applications/power-reactor.desktop
	mkdir -p build/pkg/usr/lib/systemd/user
	sed "s|@BIN@|/usr/bin/power_reactor|" power-reactor.service \
	  > build/pkg/usr/lib/systemd/user/power-reactor.service
	install -Dm644 README.md build/pkg/usr/share/doc/power-reactor/README.md
	mkdir -p build/pkg/DEBIAN
	printf 'Package: power-reactor\nVersion: %s\nArchitecture: amd64\nMaintainer: lahiru <lahirunirmalx@gmail.com>\nDepends: libsdl2-2.0-0, libgtk-3-0, upower\nRecommends: libayatana-appindicator3-1, libnotify-bin\nSection: utils\nPriority: optional\nHomepage: https://github.com/lahirunirmalx/PWR-REACTOR\nDescription: Retro CRT/VFD battery telemetry panel\n Shows battery state of every connected device (laptop, phones,\n wireless peripherals, UPS) on a retro military CRT-style panel\n with tray icon, low battery alerts and charge trend scope.\n' \
	  "$(VERSION)" > build/pkg/DEBIAN/control
	dpkg-deb --build --root-owner-group build/pkg \
	  build/power-reactor_$(VERSION)_amd64.deb

clean:
	rm -f power_reactor power_reactor_asan
	rm -rf build

.PHONY: run asan install uninstall install-service uninstall-service deb clean
