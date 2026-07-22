CC      ?= cc
CFLAGS  += -O2 -Wall -Wextra -std=gnu99 $(shell pkg-config --cflags sdl2)
LDLIBS  += $(shell pkg-config --libs sdl2) -lm

# tray icon support (GTK3 + dlopen'd libayatana-appindicator)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
ifneq ($(GTK_CFLAGS),)
CFLAGS += -DUSE_TRAY $(GTK_CFLAGS)
LDLIBS += $(shell pkg-config --libs gtk+-3.0) -ldl
endif

power_reactor: main.c
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

run: power_reactor
	./power_reactor

install-service: power_reactor
	mkdir -p $(HOME)/.config/systemd/user
	sed "s|@BIN@|$(CURDIR)/power_reactor|" power-reactor.service \
	  > $(HOME)/.config/systemd/user/power-reactor.service
	systemctl --user daemon-reload
	systemctl --user enable --now power-reactor.service

uninstall-service:
	systemctl --user disable --now power-reactor.service || true
	rm -f $(HOME)/.config/systemd/user/power-reactor.service
	systemctl --user daemon-reload

clean:
	rm -f power_reactor

.PHONY: run clean install-service uninstall-service
