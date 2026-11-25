#!/bin/bash
# Prueba contra un bastión Warpgate real (en Docker, solo en 127.0.0.1): contraseña,
# contraseña + código OTP, y contraseña + aprobación en el navegador (aprobada por su API).
# Necesita Docker y python3. Deja Warpgate detenido al terminar.
set -uo pipefail
cd "$(dirname "$0")/.."
make 2>&1 | grep -E '(warning|error): '
tools/warpgate-prueba.sh >/dev/null 2>&1 || { echo "FALLA  no se pudo levantar Warpgate"; exit 1; }
trap 'tools/warpgate-prueba.sh parar >/dev/null' EXIT
. /tmp/sftpc-wg/datos.env
KH=$W/known_hosts
FALLAS=0
ok()    { echo "OK     $1"; }
falla() { echo "FALLA  $1"; FALLAS=$((FALLAS + 1)); }
destino() { echo "$1:destino@127.0.0.1:$SSH_WG"; }

echo "== Línea de comandos =="
SFTPC_CLAVE="$USR_CLAVE" ./sftp-commander --prueba "$(destino wg-clave)" --known-hosts $KH --aceptar-host \
    </dev/null >$W/clave.out 2>&1
grep -q 'RESULTADO: todo OK' $W/clave.out && ok "solo contraseña: prueba SFTP completa por Warpgate" \
    || falla "solo contraseña"

python3 tools/totp.py $SECRETO_HEX | SFTPC_CLAVE="$USR_CLAVE" ./sftp-commander --prueba "$(destino wg-otp)" \
    --known-hosts $KH >$W/otp.out 2>&1
grep -q 'One-time password' $W/otp.out && grep -q 'RESULTADO: todo OK' $W/otp.out \
    && ok "contraseña + código OTP: pide el código una vez y entra" || falla "contraseña + OTP"

antes=$(docker logs sftpc-warpgate 2>&1 | grep -c 'Recorded failed login')
echo 000000 | SFTPC_CLAVE="$USR_CLAVE" ./sftp-commander --prueba "$(destino wg-otp)" --known-hosts $KH \
    >$W/otp-mal.out 2>&1
despues=$(docker logs sftpc-warpgate 2>&1 | grep -c 'Recorded failed login')
[ $((despues - antes)) -eq 1 ] && ok "código incorrecto: un solo intento fallido (no bloquea la cuenta)" \
    || falla "código incorrecto: $((despues - antes)) intentos fallidos"

rm -f $W/entrada; mkfifo $W/entrada
SFTPC_CLAVE="$USR_CLAVE" timeout 60 ./sftp-commander --prueba "$(destino wg-web)" --known-hosts $KH \
    <$W/entrada >$W/web.out 2>&1 &
PID=$!
exec 3>$W/entrada
for i in $(seq 40); do grep -q 'Press Enter' $W/web.out && break; sleep 0.25; done
python3 tools/warpgate-aprobar.py wg-web "$USR_CLAVE" >/dev/null
echo >&3
wait $PID
exec 3>&-
grep -q 'https://' $W/web.out && grep -q 'RESULTADO: todo OK' $W/web.out \
    && ok "aprobación en el navegador: muestra el enlace y entra tras aprobar" || falla "aprobación web"

echo
echo "== Ventana (Xvfb) =="
export GDK_BACKEND=x11 GSK_RENDERER=cairo XDG_CONFIG_HOME=$W/config
mkdir -p $XDG_CONFIG_HOME/sftp-commander
cat > $XDG_CONFIG_HOME/sftp-commander/servidores.ini <<EOF
[Warpgate OTP]
servidor=127.0.0.1
puerto=$SSH_WG
usuario=wg-otp:destino
carpeta=$W/remoto

[Warpgate web]
servidor=127.0.0.1
puerto=$SSH_WG
usuario=wg-web:destino
EOF
CAP=/tmp/sftpc-captura

# OTP: la ventana pregunta el código y la prueba lo contesta sola
SFTPC_DEMO=otp SFTPC_CLAVE="$USR_CLAVE" SFTPC_RESPUESTA="$(python3 tools/totp.py $SECRETO_HEX)" \
    xvfb-run -a -s "-screen 0 1280x800x24" ./sftp-commander "Warpgate OTP" --known-hosts $KH \
    --captura $CAP-warpgate-otp.png 2>&1 | grep -v 'Gtk-' >$W/gui-otp.out
grep -q 'PREGUNTA: Two-factor authentication | One-time password:' $W/gui-otp.out && \
    grep -q 'Estado: Conectado a wg-otp:destino@127.0.0.1' $W/gui-otp.out \
    && ok "ventana: pide el código OTP y conecta" || { falla "ventana OTP"; cat $W/gui-otp.out; }

# aprobación web: captura la ventana con el enlace
SFTPC_DEMO=preguntas SFTPC_CLAVE="$USR_CLAVE" xvfb-run -a -s "-screen 0 1280x800x24" \
    ./sftp-commander "Warpgate web" --known-hosts $KH --captura $CAP-warpgate-web.png 2>&1 \
    | grep -v 'Gtk-' >$W/gui-web.out
grep -q 'PREGUNTA: Warpgate authentication | Press Enter when done: | con enlace' $W/gui-web.out \
    && ok "ventana: muestra el enlace de aprobación y el código de seguridad" \
    || { falla "ventana web"; cat $W/gui-web.out; }

echo
[ $FALLAS -eq 0 ] && echo "RESULTADO: todo OK" || echo "RESULTADO: $FALLAS falla(s)"
exit $FALLAS
