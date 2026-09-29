# Power Reactor - GTK 3 battery telemetry dashboard
#
# GTK 3 is the floor and the ceiling on purpose: it is present on every
# Ubuntu from 20.04 LTS to 26.04 LTS, so one source tree and one .deb
# cover all of them.

VERSION := 1.0.0
APP_ID  := com.github.lahirunirmalx.PowerReactor

CC      ?= cc
PREFIX  ?= $(HOME)/.local

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)

ifeq ($(strip $(GTK_CFLAGS)),)
$(error GTK 3 development files not found - install libgtk-3-dev)
endif

# ICON_DIR lets `make run` use the icons in this tree. Installed and
# packaged builds must not carry a build path, so they set it empty and
# the tray resolves icons from the hicolor theme instead.
ICON_DIR ?= $(CURDIR)/icons

CFLAGS  += -O2 -Wall -Wextra -std=gnu99 \
           -DAPP_VERSION='"$(VERSION)"' \
           -DICON_DIR='"$(ICON_DIR)"' \
           $(GTK_CFLAGS)
LDLIBS  += $(GTK_LIBS) -ldl -lm

SRC := src/main.c src/power.c src/ui.c src/tray.c
HDR := src/power.h src/ui.h src/tray.h
BIN := power-reactor

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

run: $(BIN)
	./$(BIN)

# address/UB sanitizer build for testing
asan: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -o $(BIN)-asan \
	  $(SRC) $(LDLIBS)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 icons/power-reactor.svg \
	  $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/power-reactor.svg
	install -Dm644 icons/power-reactor-amber.svg \
	  $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/power-reactor-amber.svg
	install -Dm644 icons/power-reactor-red.svg \
	  $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/power-reactor-red.svg
	install -Dm644 data/$(APP_ID).metainfo.xml \
	  $(DESTDIR)$(PREFIX)/share/metainfo/$(APP_ID).metainfo.xml
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	sed "s|@BIN@|$(PREFIX)/bin/$(BIN)|" data/$(APP_ID).desktop \
	  > $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop
	-gtk-update-icon-cache -q $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null
	-update-desktop-database -q $(DESTDIR)$(PREFIX)/share/applications 2>/dev/null

uninstall:
	rm -f $(PREFIX)/bin/$(BIN)
	rm -f $(PREFIX)/share/icons/hicolor/scalable/apps/power-reactor*.svg
	rm -f $(PREFIX)/share/applications/$(APP_ID).desktop
	rm -f $(PREFIX)/share/metainfo/$(APP_ID).metainfo.xml

# the service prefers the installed binary, falling back to this tree
BIN_PATH = $(shell [ -x $(PREFIX)/bin/$(BIN) ] \
	   && echo $(PREFIX)/bin/$(BIN) \
	   || echo $(CURDIR)/$(BIN))

install-service: $(BIN)
	mkdir -p $(HOME)/.config/systemd/user
	sed "s|@BIN@|$(BIN_PATH)|" data/power-reactor.service \
	  > $(HOME)/.config/systemd/user/power-reactor.service
	systemctl --user daemon-reload
	systemctl --user enable --now power-reactor.service

uninstall-service:
	systemctl --user disable --now power-reactor.service || true
	rm -f $(HOME)/.config/systemd/user/power-reactor.service
	systemctl --user daemon-reload

deb:
	rm -rf build/pkg $(BIN)
	$(MAKE) $(BIN) ICON_DIR=
	$(MAKE) install DESTDIR=build/pkg PREFIX=/usr
	# caches belong to the installing system, not inside the package
	rm -f build/pkg/usr/share/applications/mimeinfo.cache
	rm -f build/pkg/usr/share/icons/hicolor/icon-theme.cache
	mkdir -p build/pkg/usr/lib/systemd/user
	sed "s|@BIN@|/usr/bin/$(BIN)|" data/power-reactor.service \
	  > build/pkg/usr/lib/systemd/user/power-reactor.service
	install -Dm644 README.md build/pkg/usr/share/doc/power-reactor/README.md
	mkdir -p build/pkg/DEBIAN
# Ubuntu 24.04 renamed these libraries for the 64-bit time_t transition
# (libgtk-3-0 -> libgtk-3-0t64), so the dependency is an alternation and
# the same control file resolves on 20.04 through 26.04. Build the deb on
# the oldest release you support: glibc symbols only version forward.
	printf 'Package: power-reactor\nVersion: %s\nArchitecture: %s\nMaintainer: lahiru <lahirunirmalx@gmail.com>\nDepends: libgtk-3-0 | libgtk-3-0t64, libglib2.0-0 | libglib2.0-0t64\nRecommends: upower, libayatana-appindicator3-1\nSuggests: android-tools-adb, nut-client, pulseaudio-utils\nSection: utils\nPriority: optional\nHomepage: https://github.com/lahirunirmalx/PWR-REACTOR\nDescription: Battery and power telemetry dashboard\n Shows the battery state of every connected device - laptop, phones,\n wireless peripherals and UPS units - on a clean GTK dashboard that\n follows the desktop theme, with low battery alerts, time estimates,\n a charge trend chart and a tray icon.\n' \
	  "$(VERSION)" "$$(dpkg --print-architecture)" > build/pkg/DEBIAN/control
	dpkg-deb --build --root-owner-group build/pkg \
	  build/power-reactor_$(VERSION)_$$(dpkg --print-architecture).deb

snap:
	snapcraft

clean:
	rm -f $(BIN) $(BIN)-asan
	rm -rf build

.PHONY: all run asan install uninstall install-service uninstall-service \
        deb snap clean
