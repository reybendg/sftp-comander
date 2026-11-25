#!/usr/bin/env python3
"""Código TOTP (RFC 6238: SHA1, 6 dígitos, 30 s) para la semilla de prueba en hexadecimal."""
import hashlib, hmac, struct, sys, time

clave = bytes.fromhex(sys.argv[1])
paso = int(time.time()) // 30
h = hmac.new(clave, struct.pack('>Q', paso), hashlib.sha1).digest()
o = h[-1] & 0x0F
print('%06d' % ((struct.unpack('>I', h[o:o + 4])[0] & 0x7FFFFFFF) % 1000000))
