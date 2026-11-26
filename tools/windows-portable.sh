#!/usr/bin/env bash
# Arma la versión portable de Windows en dist/ (carpeta + .zip).
# Correr en MSYS2 UCRT64 después de compilar: make windows
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION_APP=$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' src/main.c)
NOMBRE="sftp-commander-${VERSION_APP}-windows-x64"
DEST="dist/$NOMBRE"
PFX=/ucrt64

[ -f sftp-commander.exe ] || { echo "Falta sftp-commander.exe: compila con make"; exit 1; }
rm -rf "$DEST" "dist/$NOMBRE.zip"
mkdir -p "$DEST/bin" "$DEST/lib" "$DEST/share/glib-2.0/schemas" "$DEST/share/icons"

# Programa y ayudantes de GLib para lanzar programas (abrir archivos)
cp sftp-commander.exe "$DEST/bin/"
strip "$DEST/bin/sftp-commander.exe"
cp "$PFX/bin/gspawn-win64-helper.exe" "$PFX/bin/gspawn-win64-helper-console.exe" "$DEST/bin/"

# Cargadores de imágenes de gdk-pixbuf (con rutas relativas en loaders.cache)
cp -r "$PFX/lib/gdk-pixbuf-2.0" "$DEST/lib/"
find "$DEST/lib/gdk-pixbuf-2.0" -name '*.dll.a' -delete
CACHE="$DEST/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
sed -e 's|C:\\\\msys64\\\\ucrt64\\\\||Ig' -e 's|C:/msys64/ucrt64/||Ig' -e 's|"/ucrt64/|"|g' \
    "$PFX/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" > "$CACHE"

# DLLs: todas las de /ucrt64 que piden el programa, los ayudantes y los cargadores
dlls() { ldd "$@" 2>/dev/null | awk '$3 ~ /^\/ucrt64\// {print $3}'; }
mapfile -t LISTA < <( { dlls "$DEST/bin/"*.exe; dlls "$DEST/lib/gdk-pixbuf-2.0/2.10.0/loaders/"*.dll; } | sort -u)
cp "${LISTA[@]}" "$DEST/bin/"

# Esquemas de configuración de GTK e íconos
cp "$PFX/share/glib-2.0/schemas/"*.xml "$DEST/share/glib-2.0/schemas/" 2>/dev/null || true
glib-compile-schemas "$DEST/share/glib-2.0/schemas"
rm -f "$DEST/share/glib-2.0/schemas/"*.xml
cp -r "$PFX/share/icons/Adwaita" "$PFX/share/icons/hicolor" "$DEST/share/icons/"
# ícono propio (la ventana lo busca por nombre en el tema hicolor)
for s in 16 24 32 48 64 128 256; do
    install -Dm644 "data/icono-$s.png" "$DEST/share/icons/hicolor/${s}x${s}/apps/sftp-commander.png"
done
cp data/sftp-commander.ico "$DEST/"
# la caché copiada no conoce el ícono propio: GTK no lo encontraría
gtk4-update-icon-cache -q -f -t "$DEST/share/icons/hicolor"

# textos propios de GTK y GLib en español ("Acerca de", "Créditos", diálogos de archivos)
mkdir -p "$DEST/share/locale/es/LC_MESSAGES"
cp "$PFX/share/locale/es/LC_MESSAGES/gtk40.mo" "$PFX/share/locale/es/LC_MESSAGES/glib20.mo" \
   "$DEST/share/locale/es/LC_MESSAGES/"

# Documentos
cp LICENSE "$DEST/LICENSE.txt"
cp README.md README.es.md CHANGELOG.md "$DEST/"
mkdir -p "$DEST/docs"; cp docs/user-guide.md docs/guia-de-uso.md "$DEST/docs/"

# Acceso directo en la raíz: SFTP Commander.cmd abre bin\sftp-commander.exe
printf '@echo off\r\nstart "" "%%~dp0bin\\sftp-commander.exe" %%*\r\n' > "$DEST/SFTP Commander.cmd"

echo "Carpeta: $DEST ($(du -sh "$DEST" | cut -f1))"

# .zip portable (bsdtar de MSYS2 sabe escribir zip)
(cd dist && bsdtar --format zip -cf "$NOMBRE.zip" "$NOMBRE")
echo "Zip: dist/$NOMBRE.zip ($(du -h "dist/$NOMBRE.zip" | cut -f1))"
