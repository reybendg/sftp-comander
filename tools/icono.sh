#!/usr/bin/env bash
# Genera los PNG y el .ico del ícono a partir de data/sftp-commander.svg.
# Necesita rsvg-convert (librsvg). El .ico lleva cada tamaño como PNG (válido desde Windows Vista).
set -euo pipefail
cd "$(dirname "$0")/.."
SVG=data/sftp-commander.svg
TAMANOS=(16 24 32 48 64 128 256)

for s in "${TAMANOS[@]}"; do rsvg-convert -w "$s" -h "$s" "$SVG" -o "data/icono-$s.png"; done

# entero en little-endian como bytes crudos
le() { local v=$1 n=$2; for ((i = 0; i < n; i++)); do printf "\\x$(printf %02x $(((v >> (8 * i)) & 255)))"; done; }

ICO=data/sftp-commander.ico
{
    le 0 2; le 1 2; le ${#TAMANOS[@]} 2                       # cabecera: reservado, tipo ícono, cantidad
    off=$((6 + 16 * ${#TAMANOS[@]}))
    for s in "${TAMANOS[@]}"; do
        t=$(stat -c %s "data/icono-$s.png")
        lado=$((s == 256 ? 0 : s))                             # 0 significa 256
        le $lado 1; le $lado 1; le 0 1; le 0 1; le 1 2; le 32 2; le "$t" 4; le "$off" 4
        off=$((off + t))
    done
    for s in "${TAMANOS[@]}"; do cat "data/icono-$s.png"; done
} > "$ICO"
echo "Ícono: $ICO ($(stat -c %s "$ICO") bytes)"
