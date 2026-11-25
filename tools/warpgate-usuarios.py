#!/usr/bin/env python3
"""Crea en el Warpgate de prueba (tools/warpgate-prueba.sh) un destino SSH y tres
usuarios con distintas exigencias de autenticación. Lee y completa /tmp/sftpc-wg/datos.env.
Todo es de prueba: corre solo contra 127.0.0.1."""
import json, os, secrets, ssl, sys, urllib.request

datos = dict(l.strip().split('=', 1) for l in open('/tmp/sftpc-wg/datos.env') if '=' in l)
W, TOKEN = datos['W'], datos['TOKEN']
BASE = 'https://127.0.0.1:%s/@warpgate/admin/api' % datos['HTTP_WG']
CTX = ssl._create_unverified_context()   # certificado autofirmado del Warpgate de prueba


def api(metodo, ruta, cuerpo=None):
    req = urllib.request.Request(BASE + ruta, method=metodo,
                                 data=json.dumps(cuerpo).encode() if cuerpo is not None else None,
                                 headers={'X-Warpgate-Token': TOKEN, 'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, context=CTX) as r:
            t = r.read()
            return json.loads(t) if t else None
    except urllib.error.HTTPError as e:
        sys.exit('FALLA  %s %s -> %s %s' % (metodo, ruta, e.code, e.read()[:300]))


usuario_destino = os.environ.get('USER', 'usuario')
destino = api('POST', '/targets', {
    'name': 'destino', 'description': 'sshd de prueba',
    'options': {'kind': 'Ssh', 'host': '127.0.0.1', 'port': int(datos['SSH_DESTINO']),
                'username': usuario_destino, 'allow_insecure_algos': False, 'auth': {'kind': 'PublicKey'}},
    'ticket_requests_disabled': False, 'ticket_require_approval': False, 'require_approval': False})

# confiar en la llave del destino: así no hace falta confirmarla en una terminal
tipo, b64 = open(W + '/host_key.pub').read().split()[:2]
api('POST', '/ssh/known-hosts', {'host': '127.0.0.1', 'port': int(datos['SSH_DESTINO']),
                                  'key_type': tipo, 'key_base64': b64})

rol = api('POST', '/roles', {'name': 'pruebas', 'description': ''})
api('POST', '/targets/%s/roles/%s' % (destino['id'], rol['id']))

clave_usuario = secrets.token_urlsafe(12)
secreto = bytes.fromhex(datos['SECRETO_HEX'])
politicas = {'wg-clave': ['Password'], 'wg-otp': ['Password', 'Totp'], 'wg-web': ['Password', 'WebUserApproval']}
for nombre, politica in politicas.items():
    u = api('POST', '/users', {'username': nombre, 'description': ''})
    api('POST', '/users/%s/credentials/passwords' % u['id'], {'password': clave_usuario})
    if 'Totp' in politica:
        api('POST', '/users/%s/credentials/otp' % u['id'], {'secret_key': list(secreto)})
    api('PUT', '/users/%s' % u['id'], {'username': nombre, 'description': '',
                                       'credential_policy': {'ssh': politica}})
    api('POST', '/users/%s/roles/%s' % (u['id'], rol['id']))

with open('/tmp/sftpc-wg/datos.env', 'a') as f:
    f.write('USR_CLAVE=%s\n' % clave_usuario)
print('OK     destino "destino" y usuarios wg-clave, wg-otp, wg-web creados')
