#!/bin/bash
# Arma los paquetes .deb en contenedores Docker, uno por distribución, y prueba
# cada uno instalándolo en un contenedor limpio de la misma distribución.
# Uso: tools/empaquetar.sh            (deja los .deb en dist/)
#      MANTENEDOR="Nombre <correo>" tools/empaquetar.sh
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION_APP=$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' src/main.c)
MANTENEDOR=${MANTENEDOR:-"SFTP Commander <sftp-commander@localhost>"}
mkdir -p dist

# imagen base y sufijo de la versión del paquete
DISTROS=("debian:13 deb13" "ubuntu:24.04 ubuntu24.04")

for d in "${DISTROS[@]}"; do
    read -r IMAGEN SUFIJO <<< "$d"
    echo "== Armando para $IMAGEN =="
    docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/dist \
        -e VERSION_APP="$VERSION_APP" -e SUFIJO="$SUFIJO" -e MANTENEDOR="$MANTENEDOR" \
        -e DUENO="$(id -u):$(id -g)" \
        "$IMAGEN" bash /src/tools/deb-armar.sh
done

for d in "${DISTROS[@]}"; do
    read -r IMAGEN SUFIJO <<< "$d"
    echo
    echo "== Probando la instalación en $IMAGEN limpio =="
    docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/dist:ro -e VERSION_APP="$VERSION_APP" -e SUFIJO="$SUFIJO" \
        "$IMAGEN" bash /src/tools/deb-probar.sh
done

echo
ls -l dist/*.deb
