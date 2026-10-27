# Build yb-camerad and install the yogabook-camera package files.
# Use the Debian package for a live system: make install does not run
# dpkg-divert and would overwrite the distribution's Snapshot executable.
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Wno-unused-parameter
DESTDIR ?=
PREFIX ?= /usr

yb-camerad: yb-camerad.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) \
		$$($(PKG_CONFIG) --cflags --libs libpipewire-0.3 libsystemd)

install: yb-camerad
	install -D -m 0755 yb-camerad $(DESTDIR)$(PREFIX)/bin/yb-camerad
	install -D -m 0644 yb-camerad.service $(DESTDIR)$(PREFIX)/lib/systemd/user/yb-camerad.service
	install -D -m 0644 60-yb-camera.conf $(DESTDIR)$(PREFIX)/share/wireplumber/wireplumber.conf.d/60-yb-camera.conf
	install -D -m 0644 atomisp-yogabook-camera.conf $(DESTDIR)$(PREFIX)/lib/modprobe.d/atomisp-yogabook-camera.conf
	install -D -m 0755 snapshot $(DESTDIR)$(PREFIX)/bin/snapshot

clean:
	rm -f yb-camerad

.PHONY: install clean
