#!/bin/bash
# Levanta un Warpgate de prueba en Docker (solo en 127.0.0.1) con un servidor SSH
# de destino propio, y crea tres usuarios para probar la autenticación:
#   wg-clave : solo contraseña
#   wg-otp   : contraseña + código de un solo uso (TOTP)
#   wg-web   : contraseña + aprobación en el navegador
# Uso: tools/warpgate-prueba.sh iniciar | parar
# Deja en /tmp/sftpc-wg/datos.env los datos para las pruebas (todo es de prueba).
set -euo pipefail
W=/tmp/sftpc-wg
IMAGEN=ghcr.io/warp-tech/warpgate:latest
NOMBRE=sftpc-warpgate
SSH_WG=2222      # SSH de Warpgate
HTTP_WG=8888     # web/API de Warpgate
SSH_DESTINO=2297 # sshd de destino (detrás de Warpgate)

parar() {
    docker rm -f $NOMBRE >/dev/null 2>&1 || true
    [ -f $W/sshd.pid ] && kill "$(cat $W/sshd.pid)" 2>/dev/null || true
}

if [ "${1:-}" = "parar" ]; then parar; echo "Warpgate de prueba detenido"; exit 0; fi
parar
rm -rf $W; mkdir -p $W/data $W/remoto
chmod 777 $W/data   # el contenedor corre con el usuario "warpgate"
CLAVE=$(head -c 12 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9')
TOKEN=$(head -c 24 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9')
SECRETO_HEX=$(head -c 20 /dev/urandom | od -An -tx1 | tr -d ' \n')   # semilla TOTP de prueba

echo "== Configurar Warpgate =="
docker run --rm -v $W/data:/data -e WARPGATE_ADMIN_PASSWORD="$CLAVE" $IMAGEN \
    --config /data/warpgate.yaml unattended-setup --data-path /data \
    --http-port $HTTP_WG --ssh-port $SSH_WG --listen-address 127.0.0.1 --external-host 127.0.0.1 >$W/setup.log 2>&1 \
    || { tail -20 $W/setup.log; exit 1; }

echo "== Servidor SSH de destino (puerto $SSH_DESTINO) con las llaves de Warpgate =="
docker run --rm -v $W/data:/data $IMAGEN --config /data/warpgate.yaml client-keys 2>/dev/null \
    | grep -E '^(ssh-|ecdsa-)' > $W/authorized_keys
ssh-keygen -q -t ed25519 -N '' -f $W/host_key
cat > $W/sshd_config <<EOF
Port $SSH_DESTINO
ListenAddress 127.0.0.1
HostKey $W/host_key
AuthorizedKeysFile $W/authorized_keys
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
PidFile $W/sshd.pid
Subsystem sftp internal-sftp
EOF
/usr/sbin/sshd -f $W/sshd_config -E $W/sshd.log
echo "texto por warpgate" > $W/remoto/nota.txt
echo "linea de log" > $W/remoto/servicio.log

echo "== Arrancar Warpgate =="
docker run -d --name $NOMBRE --network host -v $W/data:/data -e WARPGATE_ADMIN_TOKEN="$TOKEN" \
    $IMAGEN --config /data/warpgate.yaml run --enable-admin-token >/dev/null
for i in $(seq 30); do
    curl -sk -o /dev/null https://127.0.0.1:$HTTP_WG/@warpgate && break
    sleep 1
done

cat > $W/datos.env <<EOF
W=$W
SSH_WG=$SSH_WG
HTTP_WG=$HTTP_WG
SSH_DESTINO=$SSH_DESTINO
CLAVE=$CLAVE
TOKEN=$TOKEN
SECRETO_HEX=$SECRETO_HEX
EOF
chmod 600 $W/datos.env
python3 "$(dirname "$0")/warpgate-usuarios.py"
echo "Warpgate listo en 127.0.0.1:$SSH_WG (web https://127.0.0.1:$HTTP_WG)"
