#!/usr/bin/env bash
# Capturas para el README con datos de ejemplo (usuario "demo", archivos inventados).
# Corre en un equipo con Docker: instala el .deb de Debian 13 en un contenedor limpio,
# levanta sshd adentro y saca las capturas en Xvfb. Deja los PNG en docs/img/.
#   tools/capturas.sh            capturas y borra el contenedor
#   tools/capturas.sh mantener   lo deja corriendo con sshd en el puerto 2296 (para
#                                capturar desde otro equipo); se quita con: docker rm -f sftpc-demo
set -euo pipefail
cd "$(dirname "$0")/.."
DEB=$(ls dist/sftp-commander_*~deb13_amd64.deb | sort -V | tail -1)
mkdir -p docs/img
docker rm -f sftpc-demo >/dev/null 2>&1 || true
docker run -d --name sftpc-demo -p 2296:22 -v "$PWD":/src:ro debian:13 sleep infinity >/dev/null

docker exec -i sftpc-demo bash -s "$DEB" <<'DENTRO'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq "/src/$1" openssh-server xvfb xauth fonts-cantarell adwaita-icon-theme >/dev/null
useradd -m -s /bin/bash demo
echo "127.0.0.1 web.ejemplo.com respaldo.ejemplo.com bastion.ejemplo.com" >> /etc/hosts
mkdir -p /run/sshd && ssh-keygen -A >/dev/null
echo "PasswordAuthentication no" > /etc/ssh/sshd_config.d/demo.conf
/usr/sbin/sshd

# servidor remoto (el mismo contenedor, en /srv/www)
mkdir -p /srv/www/sitio/{css,js,img} /srv/www/logs /srv/www/respaldos
cd /srv/www
echo '<h1>Sitio</h1>' > sitio/index.html; echo 'body{}' > sitio/css/estilo.css; echo '' > sitio/js/app.js
head -c 210000 /dev/urandom > sitio/img/logo.png
printf 'texto original\n' > nota.txt
for d in 01 02 03; do seq 1 3000 | sed "s/^/2026-11-$d GET \/ 200 /" > logs/acceso-2026-11-$d.log; done
seq 1 400 | sed 's/^/error de prueba /' > logs/error.log
head -c 5200000 /dev/urandom > respaldos/sitio-2026-11-20.tar.gz
printf 'server { listen 80; }\n' > nginx.conf
chown -R demo:demo /srv/www

su - demo <<'DEMO'
set -e
ssh-keygen -q -t ed25519 -N '' -f ~/.ssh/id_ed25519
cp ~/.ssh/id_ed25519.pub ~/.ssh/authorized_keys
for h in web.ejemplo.com respaldo.ejemplo.com bastion.ejemplo.com; do ssh-keyscan -q $h >> ~/.ssh/known_hosts 2>/dev/null; done
# equipo local de ejemplo
mkdir -p Documentos/Informes Proyectos/sitio-web/css Proyectos/sitio-web/img Descargas Imágenes Música
echo '<h1>Hola</h1>' > Proyectos/sitio-web/index.html
echo 'body{margin:0}' > Proyectos/sitio-web/css/estilo.css
head -c 180000 /dev/urandom > Proyectos/sitio-web/img/portada.jpg
head -c 52000 /dev/urandom > Documentos/Informes/ventas-2026.pdf
printf 'Lista de tareas\n' > Documentos/notas.txt
head -c 2400000 /dev/urandom > Descargas/respaldo-sitio.tar.gz
mkdir -p ~/.config/sftp-commander
cat > ~/.config/sftp-commander/servidores.ini <<EOF
[Servidor web]
servidor=web.ejemplo.com
usuario=demo
carpeta=/srv/www

[Respaldo]
servidor=respaldo.ejemplo.com
usuario=demo
carpeta=/srv/www/respaldos

[Interno vía bastión]
servidor=web.ejemplo.com
usuario=demo
carpeta=/srv/www/sitio
salto=demo@bastion.ejemplo.com
EOF
chmod 600 ~/.config/sftp-commander/servidores.ini
DEMO
# fechas parejas para que las capturas se vean ordenadas
find /srv/www /home/demo -mindepth 1 -not -path '*/.*' -exec touch -h -d '2026-11-20 10:15' {} +

su - demo <<'DEMO'
export GDK_BACKEND=x11 GSK_RENDERER=cairo LANG=C.UTF-8
cap() {  # cap <archivo> [SFTPC_DEMO] [servidor guardado]
    SFTPC_DEMO=${2:-} SFTPC_PESTANA="Respaldo" timeout 60 xvfb-run -a -s "-screen 0 1280x800x24" \
        sftp-commander "${3:-Servidor web}" --captura "/tmp/cap-$1.png" 2>&1 | grep -E 'Captura|Estado' || true
}
cap principal
cap buscar buscar
cap pestanas pestanas
cap servidores servidores
cap menu menu
cap salto "" "Interno vía bastión"
cap acerca acerca
DEMO
DENTRO

for f in principal buscar pestanas servidores menu salto acerca; do
    docker cp "sftpc-demo:/tmp/cap-$f.png" "docs/img/$f.png" 2>/dev/null || echo "falta $f"
done
if [ "${1:-}" = mantener ]; then
    docker cp sftpc-demo:/home/demo/.ssh/id_ed25519 /tmp/sftpc-demo-id && chmod 600 /tmp/sftpc-demo-id
    echo "Contenedor sftpc-demo corriendo: sshd en el puerto 2296, usuario demo, llave /tmp/sftpc-demo-id"
else
    docker rm -f sftpc-demo >/dev/null
fi
ls -la docs/img
