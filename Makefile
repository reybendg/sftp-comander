# Linux: necesita libgtk-4-dev, libssh-dev y libsecret-1-dev
# Windows: en MSYS2 UCRT64 (mingw-w64-ucrt-x86_64-gtk4, -libssh, -gcc, -pkgconf, make)
CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
PREFIX  ?= $(HOME)/.local
SRC      = src/main.c src/sftp.c src/llavero.c src/prueba.c

ifeq ($(OS),Windows_NT)
PKGS     = gtk4 libssh
BIN      = sftp-commander.exe
# -mwindows: sin ventana de consola; ws2_32 sockets; advapi32 credenciales
LIBS     = $(shell pkg-config --libs $(PKGS)) -lws2_32 -ladvapi32 -mwindows
else
PKGS     = gtk4 gio-unix-2.0 libssh libsecret-1
BIN      = sftp-commander
LIBS     = $(shell pkg-config --libs $(PKGS))
endif
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
VERSION_APP := $(shell sed -n 's/^\#define VERSION "\(.*\)"/\1/p' src/main.c)
TAMANOS  = 16 24 32 48 64 128 256

ifeq ($(OS),Windows_NT)
# ícono y datos de versión dentro del .exe
RES      = src/recursos.o
$(RES): data/sftp-commander.rc data/sftp-commander.ico src/main.c
	windres -I data -DVER_NUM=$(subst .,$(comma),$(VERSION_APP)),0 -DVER_TXT='\"$(VERSION_APP)\"' $< -o $@
comma := ,
endif

$(BIN): $(SRC) src/sftp.h src/llavero.h $(RES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(RES) $(LIBS)

# Regenera los PNG y el .ico desde data/sftp-commander.svg (necesita rsvg-convert)
icono:
	tools/icono.sh

install: sftp-commander
	install -Dm755 sftp-commander $(PREFIX)/bin/sftp-commander
	install -Dm644 data/sftp-commander.svg $(PREFIX)/share/icons/hicolor/scalable/apps/sftp-commander.svg
	for s in $(TAMANOS); do install -Dm644 data/icono-$$s.png $(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/sftp-commander.png; done
	-gtk-update-icon-cache -q $(PREFIX)/share/icons/hicolor 2>/dev/null
	install -Dm644 sftp-commander.desktop $(PREFIX)/share/applications/sftp-commander.desktop
	sed -i 's|^Exec=.*|Exec=$(PREFIX)/bin/sftp-commander %u|' $(PREFIX)/share/applications/sftp-commander.desktop
	-update-desktop-database $(PREFIX)/share/applications 2>/dev/null
	-xdg-mime default sftp-commander.desktop x-scheme-handler/sftp 2>/dev/null

uninstall:
	rm -f $(PREFIX)/bin/sftp-commander $(PREFIX)/share/applications/sftp-commander.desktop
	rm -f $(PREFIX)/share/icons/hicolor/*/apps/sftp-commander.png $(PREFIX)/share/icons/hicolor/scalable/apps/sftp-commander.svg

clean:
	rm -f sftp-commander sftp-commander.exe src/recursos.o

# Paquetes .deb para Debian 13 y Ubuntu 24.04 en dist/ (necesita Docker)
deb:
	tools/empaquetar.sh

# Carpeta portable e instalador para Windows en dist/ (en MSYS2 UCRT64, con NSIS)
windows: $(BIN)
	tools/windows-portable.sh
	makensis -V2 -DVERSION_APP=$(VERSION_APP) tools/instalador.nsi

.PHONY: install uninstall clean deb windows icono
