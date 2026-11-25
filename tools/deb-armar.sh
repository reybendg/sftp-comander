#!/bin/bash
# Corre dentro del contenedor (lo llama tools/empaquetar.sh): compila con las
# opciones de endurecimiento de Debian y arma el .deb en /dist.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends build-essential pkg-config dpkg-dev \
    libgtk-4-dev libssh-dev libsecret-1-dev >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; exit 1; }
. /etc/os-release

# compilar (CFLAGS/LDFLAGS por entorno: el Makefile les agrega pkg-config)
rm -rf /tmp/b && cp -r /src /tmp/b && cd /tmp/b
make clean >/dev/null
CFLAGS="$(dpkg-buildflags --get CFLAGS) $(dpkg-buildflags --get CPPFLAGS)" \
LDFLAGS="$(dpkg-buildflags --get LDFLAGS)" make 2>&1 | grep -E '(warning|error): ' || true
[ -x sftp-commander ] || { echo "NO COMPILA en $PRETTY_NAME"; exit 1; }

# árbol del paquete
P=/tmp/pkg
rm -rf $P
install -Dm755 sftp-commander $P/usr/bin/sftp-commander
strip --strip-unneeded --remove-section=.comment --remove-section=.note $P/usr/bin/sftp-commander
install -Dm644 sftp-commander.desktop $P/usr/share/applications/sftp-commander.desktop
# ícono propio en el tema hicolor (escalable + PNG)
install -Dm644 data/sftp-commander.svg $P/usr/share/icons/hicolor/scalable/apps/sftp-commander.svg
for s in 16 24 32 48 64 128 256; do
    install -Dm644 data/icono-$s.png $P/usr/share/icons/hicolor/${s}x${s}/apps/sftp-commander.png
done
DOC=$P/usr/share/doc/sftp-commander
install -Dm644 README.md $DOC/README.md
install -Dm644 docs/user-guide.md $DOC/user-guide.md
install -Dm644 docs/guia-de-uso.md $DOC/guia-de-uso.md
gzip -9n -c CHANGELOG.md > $DOC/changelog.gz
# changelog en formato Debian (lo exige un paquete no nativo, con "-1" en la versión)
cat > /tmp/changelog.Debian <<EOF
sftp-commander (${VERSION_APP}-1~${SUFIJO}) ${VERSION_CODENAME:-unstable}; urgency=medium

  * Paquete binario armado con tools/empaquetar.sh.
  * Los cambios del programa están en changelog.gz.

 -- ${MANTENEDOR}  $(date -R)
EOF
gzip -9n -c /tmp/changelog.Debian > $DOC/changelog.Debian.gz
chmod 644 $DOC/changelog.gz $DOC/changelog.Debian.gz
install -d $P/usr/share/man/man1
gzip -9n -c docs/sftp-commander.1 > $P/usr/share/man/man1/sftp-commander.1.gz
chmod 644 $P/usr/share/man/man1/sftp-commander.1.gz
cat > $DOC/copyright <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: sftp-commander

Files: *
Copyright: 2026 the SFTP Commander authors
License: GPL-3+

License: GPL-3+
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 .
 On Debian systems, the complete text of the GNU General Public License
 version 3 can be found in "/usr/share/common-licenses/GPL-3".
EOF

# dependencias calculadas de las bibliotecas que usa el binario
mkdir -p /tmp/sh/debian
printf 'Source: sftp-commander\n\nPackage: sftp-commander\nArchitecture: any\n' > /tmp/sh/debian/control
DEPENDS=$(cd /tmp/sh && dpkg-shlibdeps -O $P/usr/bin/sftp-commander 2>/dev/null | sed -n 's/^shlibs:Depends=//p')

ARCH=$(dpkg --print-architecture)
mkdir -p $P/DEBIAN
cat > $P/DEBIAN/control <<EOF
Package: sftp-commander
Version: ${VERSION_APP}-1~${SUFIJO}
Section: net
Priority: optional
Architecture: ${ARCH}
Maintainer: ${MANTENEDOR}
Installed-Size: $(du -sk --exclude=DEBIAN $P | cut -f1)
Depends: ${DEPENDS}
Recommends: gnome-keyring | keepassxc
Suggests: openssh-client
Description: two-pane SFTP client in the style of WinSCP Commander
 SFTP Commander shows the local computer and the server side by side and
 copies files between them with the keyboard (F5 copy, F8 delete) or the
 right-click menu. It has tabs with one session each, saved servers,
 passwords in the system keyring, search, and editing of remote files
 that are uploaded again on every save.
 .
 Native GTK 4 interface (in Spanish), built on libssh. It uses the
 standard OpenSSH files: ~/.ssh/config, ~/.ssh/known_hosts and ssh-agent.
EOF

DEB=/dist/sftp-commander_${VERSION_APP}-1~${SUFIJO}_${ARCH}.deb
dpkg-deb --root-owner-group -Zxz --build $P "$DEB" >/dev/null
chown "$DUENO" "$DEB"
echo "Armado: $(basename "$DEB") ($(du -h "$DEB" | cut -f1)) para $PRETTY_NAME"
echo "Depends: $DEPENDS"
