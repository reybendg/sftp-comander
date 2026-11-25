#!/bin/bash
# Prueba completa sin tocar la configuración del sistema:
# levanta un sshd temporal del usuario en 127.0.0.1:2299 (llaves en /tmp),
# corre la prueba de la capa SFTP y abre la ventana en Xvfb para sacar una captura.
set -euo pipefail
cd "$(dirname "$0")/.."
D=/tmp/sftpc-sshd
PUERTO=2299

make

rm -rf "$D"; mkdir -p "$D"
ssh-keygen -q -t ed25519 -N '' -f "$D/host_key"
ssh-keygen -q -t ed25519 -N '' -f "$D/cliente"
cp "$D/cliente.pub" "$D/authorized_keys"
# Puerto $PUERTO ofrece contraseña (sin root ni PAM nunca la acepta, pero la app debe pedirla).
# Puerto $SIN_CLAVE no ofrece contraseña, como un bastión que solo acepta llave o código 2FA.
SIN_CLAVE=$((PUERTO - 1))
cat > "$D/sshd_config" <<EOF
Port $PUERTO
Port $SIN_CLAVE
ListenAddress 127.0.0.1
HostKey $D/host_key
AuthorizedKeysFile $D/authorized_keys
PasswordAuthentication yes
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
PidFile $D/sshd.pid
Subsystem sftp internal-sftp
Match LocalPort $SIN_CLAVE
    PasswordAuthentication no
EOF
/usr/sbin/sshd -f "$D/sshd_config" -E "$D/sshd.log"
trap 'kill "$(cat "$D/sshd.pid" 2>/dev/null)" 2>/dev/null || true' EXIT
sleep 0.5

DEST="$USER@127.0.0.1:$PUERTO"
KH="$D/known_hosts"

echo "== Prueba de la capa SFTP =="
./sftp-commander --prueba "$DEST" -i "$D/cliente" --known-hosts "$KH" --aceptar-host

echo
echo "== Lo mismo pasando por un servidor de salto (el mismo sshd hace de salto y de destino) =="
./sftp-commander --prueba "$DEST" -J "$DEST" -i "$D/cliente" --known-hosts "$KH" --aceptar-host \
    | grep -E 'salto|túnel|destino|RESULTADO'

echo
echo "== Puerto sin contraseña: con la llave entra igual (y queda en known_hosts) =="
./sftp-commander --prueba "$USER@127.0.0.1:$SIN_CLAVE" -i "$D/cliente" --known-hosts "$KH" --aceptar-host \
    | grep -E 'autenticar|RESULTADO'

echo
echo "== Ventana en Xvfb =="
CAP="${1:-/tmp/sftpc-captura.png}"
export GDK_BACKEND=x11 GSK_RENDERER=cairo
xvfb-run -a -s "-screen 0 1280x800x24" \
    ./sftp-commander "$DEST" -i "$D/cliente" --known-hosts "$KH" --captura "$CAP"

# Lista de servidores de prueba en una config aparte (no toca ~/.config del usuario)
export XDG_CONFIG_HOME="$D/config"
mkdir -p "$XDG_CONFIG_HOME/sftp-commander"
mkdir -p "$D/remoto"
echo "texto original" > "$D/remoto/nota.txt"
echo "linea de log" > "$D/remoto/servicio.log"
cat > "$XDG_CONFIG_HOME/sftp-commander/servidores.ini" <<EOF
[Prueba local]
servidor=127.0.0.1
puerto=$PUERTO
usuario=$USER
llave=$D/cliente
carpeta=$D/remoto

[Sin llave]
servidor=127.0.0.1
puerto=$PUERTO
usuario=$USER

[Sin llave ni clave]
servidor=127.0.0.1
puerto=$SIN_CLAVE
usuario=$USER

[Otro servidor]
servidor=192.168.1.10
puerto=2222
usuario=admin

[Bastion prueba]
servidor=127.0.0.1
puerto=$PUERTO
usuario=$USER
llave=$D/cliente
carpeta=$D/remoto
salto=$USER@127.0.0.1:$PUERTO
llave_salto=$D/cliente

[Salto sin llave]
servidor=127.0.0.1
puerto=$PUERTO
usuario=$USER
llave=$D/cliente
salto=$USER@127.0.0.1:$PUERTO
EOF

echo
echo "== Conectar por nombre guardado (usa la llave del servidor, sin -i) =="
xvfb-run -a -s "-screen 0 1280x800x24" \
    ./sftp-commander "Prueba local" --known-hosts "$KH" --captura "${CAP%.png}-guardado.png"

# El menú contextual y el gestor son ventanas aparte: la app captura esa ventana
for demo in menu servidores; do
    echo
    echo "== Captura: $demo =="
    SFTPC_DEMO=$demo xvfb-run -a -s "-screen 0 1280x800x24" \
        ./sftp-commander "Prueba local" --known-hosts "$KH" --captura "${CAP%.png}-$demo.png"
done

echo
echo "== Aviso 'Conectando…' con spinner (192.0.2.1 no responde nunca) =="
SFTPC_DEMO=conectando xvfb-run -a -s "-screen 0 1280x800x24" \
    ./sftp-commander 192.0.2.1 --known-hosts "$KH" --captura "${CAP%.png}-conectando.png"

echo
echo "== Cancelar a mitad de la conexión =="
SFTPC_DEMO=cancelar xvfb-run -a -s "-screen 0 1280x800x24" \
    ./sftp-commander 192.0.2.1 --known-hosts "$KH" --captura "${CAP%.png}-cancelar.png" | tee "$D/cancelar.txt"
if grep -q "Estado: Conexión cancelada" "$D/cancelar.txt"; then
    echo "OK     la conexión se canceló al instante"
else
    echo "FALLA  no se canceló la conexión"
    exit 1
fi

# Corre un escenario SFTPC_DEMO y comprueba que la salida tenga un texto
escenario() {   # escenario <demo> <texto esperado> <descripción> [servidor guardado] [nombre de la captura]
    local srv=${4:-Prueba local} n=${5:-$1}
    echo
    echo "== $3 =="
    SFTPC_DEMO=$1 xvfb-run -a -s "-screen 0 1280x800x24" \
        ./sftp-commander "$srv" --known-hosts "$KH" --captura "${CAP%.png}-$n.png" \
        2>&1 | grep -v 'Gtk-' | tee "$D/$n.txt"
    if grep -qF "$2" "$D/$n.txt"; then
        echo "OK     $3"
    else
        echo "FALLA  $3: no aparece \"$2\""
        exit 1
    fi
}

escenario pestanas    "Pestañas: 2" "Dos pestañas conectadas a la vez"
escenario buscar      "Estado: 1 coincidencia(s) con \"log\"" "Buscar en el panel remoto filtra la lista"
escenario copiar-ruta "PORTAPAPELES: $D/remoto/nota.txt" "Copiar ruta deja la ruta completa en el portapapeles"
escenario perdida     "DIALOGO: Se perdió la conexión" "Al volver a una pestaña sin conexión pregunta si reconectar"
escenario reconectar  "Estado: Conectado a" "Reconectar desde el diálogo vuelve a la misma carpeta"
grep -q "Pestañas: 2" "$D/reconectar.txt" && echo "OK     la otra pestaña sigue abierta" \
    || { echo "FALLA  se perdió la otra pestaña"; exit 1; }

# servidor de salto: el mismo sshd hace de salto y de destino
escenario salto      "vía $USER@127.0.0.1:$PUERTO" "Conectar a través de un servidor de salto" "Bastion prueba"
escenario buscar     "Estado: 1 coincidencia(s) con \"log\"" "Trabajar a través del salto (buscar en el remoto)" \
    "Bastion prueba" buscar-salto
escenario reconectar "Estado: Conectado a" "Conexión perdida y reconectar a través del salto" \
    "Bastion prueba" reconectar-salto
escenario clave-salto "VENTANA CLAVE" "Pide la contraseña del salto si no acepta la llave" \
    "Salto sin llave"
escenario clave "VENTANA CLAVE" "Pide contraseña si el servidor no acepta la llave" "Sin llave"
escenario clave "no acepta contraseña" "Si el servidor no acepta contraseña, lo dice y no la pide" \
    "Sin llave ni clave" sin-clave
SFTPC_CLAVE=no-se-usa escenario clave "no se envió" \
    "Con contraseña escrita y un servidor que no la acepta, avisa sin pedirla otra vez" \
    "Sin llave ni clave" sin-clave-escrita
for f in sin-clave sin-clave-escrita; do
    if grep -q "VENTANA CLAVE" "$D/$f.txt"; then
        echo "FALLA  abrió la ventana de contraseña en un servidor que no la acepta ($f)"; exit 1
    fi
done

echo
echo "== Abrir nota.txt remoto, editarlo y que se suba solo =="
# temporales que ya había (de una sesión abierta del usuario): no cuentan
antes=$(ls ~/.cache/sftp-commander/editar/ 2>/dev/null | sort || true)
SFTPC_DEMO=editar xvfb-run -a -s "-screen 0 1280x800x24" \
    ./sftp-commander "Prueba local" --known-hosts "$KH" --captura "${CAP%.png}-editar.png"
if [ "$(cat "$D/remoto/nota.txt")" = "editado por la prueba" ]; then
    echo "OK     el cambio llegó al servidor"
else
    echo "FALLA  nota.txt en el servidor dice: $(cat "$D/remoto/nota.txt")"
    exit 1
fi
despues=$(ls ~/.cache/sftp-commander/editar/ 2>/dev/null | sort || true)
if [ -n "$(comm -13 <(echo "$antes") <(echo "$despues") | grep .)" ]; then
    echo "FALLA  quedaron temporales en ~/.cache/sftp-commander/editar"
else
    echo "OK     temporales borrados al salir"
fi
