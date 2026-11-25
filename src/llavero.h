/* Contraseñas en el llavero del sistema (Secret Service: gnome-keyring, KWallet, KeePassXC).
 * Se guardan cifradas por el llavero, nunca en archivos de la app.
 * Funciones síncronas: llamarlas desde el hilo de trabajo cuando sea posible. */
#pragma once
#include <glib.h>

/* Devuelve la contraseña (liberar con llavero_liberar) o NULL si no hay */
char    *llavero_buscar(const char *servidor, const char *puerto, const char *usuario, GError **err);
/* nombre: el del servidor guardado, o NULL si es una conexión suelta */
gboolean llavero_guardar(const char *nombre, const char *servidor, const char *puerto,
                         const char *usuario, const char *clave, GError **err);
/* Borra la contraseña asociada a un servidor guardado; TRUE si había una */
gboolean llavero_olvidar_nombre(const char *nombre, GError **err);
void     llavero_liberar(char *clave);
