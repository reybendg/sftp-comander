#!/bin/bash
# Corre dentro de un contenedor limpio (lo llama tools/empaquetar.sh): instala el
# .deb con apt como lo haría un usuario y comprueba que el programa funcione.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
DEB=$(ls /dist/sftp-commander_${VERSION_APP}-1~${SUFIJO}_*.deb)
apt-get update -qq >/dev/null
if ! apt-get install -y -qq "$DEB" >/tmp/apt.log 2>&1; then
    echo "FALLA  apt no pudo instalar $(basename "$DEB")"; tail -20 /tmp/apt.log; exit 1
fi
echo "OK     apt instaló $(basename "$DEB") con sus dependencias"
v=$(sftp-commander --version)
[ "$v" = "sftp-commander $VERSION_APP" ] && echo "OK     $v" || { echo "FALLA  versión: $v"; exit 1; }
if ldd /usr/bin/sftp-commander | grep -q 'not found'; then
    echo "FALLA  faltan bibliotecas:"; ldd /usr/bin/sftp-commander | grep 'not found'; exit 1
fi
echo "OK     no falta ninguna biblioteca"

# lintian (revisor oficial de paquetes de Debian) y validar el .desktop
apt-get install -y -qq --no-install-recommends lintian desktop-file-utils >/dev/null 2>&1
desktop-file-validate /usr/share/applications/sftp-commander.desktop && echo "OK     .desktop válido"
echo "-- lintian:"
lintian --tag-display-limit 0 "$DEB" 2>&1 | grep -v 'root privileges' | sed 's/^/   /' || true
man -w sftp-commander >/dev/null 2>&1 || [ -f /usr/share/man/man1/sftp-commander.1.gz ] && echo "OK     página de manual instalada"

# conexión SFTP real contra un sshd temporal dentro del contenedor
apt-get install -y -qq --no-install-recommends openssh-server >/dev/null 2>&1
D=/tmp/s; mkdir -p $D /run/sshd
ssh-keygen -q -t ed25519 -N '' -f $D/host_key
ssh-keygen -q -t ed25519 -N '' -f $D/cliente
cp $D/cliente.pub $D/authorized_keys
printf 'Port 2299\nListenAddress 127.0.0.1\nHostKey %s/host_key\nAuthorizedKeysFile %s/authorized_keys\nPasswordAuthentication no\nStrictModes no\nSubsystem sftp internal-sftp\n' $D $D > $D/sshd_config
/usr/sbin/sshd -f $D/sshd_config
sleep 0.5
sftp-commander --prueba root@127.0.0.1:2299 -i $D/cliente --known-hosts $D/kh --aceptar-host 2>&1 | tail -1
