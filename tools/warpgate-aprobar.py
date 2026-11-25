#!/usr/bin/env python3
"""Hace lo que haría el usuario en el navegador con el Warpgate de prueba: entra con su
contraseña y aprueba la solicitud de inicio de sesión SSH pendiente.
Uso: warpgate-aprobar.py <usuario> <contraseña>"""
import http.cookiejar, json, ssl, sys, time, urllib.request

datos = dict(l.strip().split('=', 1) for l in open('/tmp/sftpc-wg/datos.env') if '=' in l)
BASE = 'https://127.0.0.1:%s/@warpgate/api' % datos['HTTP_WG']
ctx = ssl._create_unverified_context()
web = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx),
                                  urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))


def api(metodo, ruta, cuerpo=None):
    req = urllib.request.Request(BASE + ruta, method=metodo,
                                 data=json.dumps(cuerpo).encode() if cuerpo is not None else None,
                                 headers={'Content-Type': 'application/json'})
    with web.open(req) as r:
        t = r.read()
        return json.loads(t) if t else None


api('POST', '/auth/login', {'username': sys.argv[1], 'password': sys.argv[2]})
for _ in range(20):
    pendientes = api('GET', '/auth/web-auth-requests') or []
    if pendientes:
        break
    time.sleep(0.5)
else:
    sys.exit('FALLA  no hay solicitudes pendientes de aprobar')
for p in pendientes:
    api('POST', '/auth/state/%s/approve' % p['id'], {'scope': 'Once'})   # solo esta vez
    print('APROBADO %s (código %s)' % (p['id'], p.get('identification_string', '?')))
