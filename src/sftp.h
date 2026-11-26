/* Capa de conexión: SFTP remoto con libssh y operaciones locales equivalentes.
 * Todas las funciones con Conexion* bloquean un mutex propio, así que pueden
 * llamarse desde un hilo de trabajo (una operación a la vez). */
#pragma once
#include <glib.h>

typedef struct Conexion Conexion;

typedef struct {
    char    *nombre;
    gboolean es_dir;
    guint64  tam;
    gint64   mtime;
    guint32  permisos;
} Entrada;

typedef enum { HOST_OK, HOST_NUEVO, HOST_CAMBIADO } EstadoHost;

/* Avance de una transferencia; devolver FALSE cancela. */
typedef gboolean (*ConProgreso)(const char *archivo, guint64 hecho, guint64 total, gpointer data);

#define CON_ERROR (con_error_quark())
GQuark con_error_quark(void);
enum { CON_ERR_RED, CON_ERR_AUTH, CON_ERR_SFTP, CON_ERR_LOCAL, CON_ERR_CANCELADO,
       CON_ERR_CODIGO,     /* se contestaron preguntas (OTP, aprobación) y el servidor no aceptó */
       CON_ERR_SIN_CLAVE   /* el servidor no acepta contraseña: no tiene sentido pedirla */ };

/* Preguntas del servidor durante el inicio de sesión (keyboard-interactive): código OTP,
 * aprobación en el navegador, etc. Se llama desde el hilo de trabajo y bloquea hasta que
 * el usuario contesta. Debe dejar n respuestas nuevas (g_malloc) en "respuestas".
 * Devuelve FALSE si el usuario canceló. La contraseña no pasa por aquí: se contesta sola. */
typedef gboolean (*ConPreguntas)(const char *nombre, const char *instruccion, int n,
                                 const char * const *prompts, const gboolean *eco,
                                 char **respuestas, gpointer data);

void       con_init(void);
const char *con_version_libssh(void);   /* para "Acerca de" */
/* Acepta "usuario@host:puerto", "sftp://usuario@host:puerto/..." o un alias de ~/.ssh/config */
gboolean   destino_parsear(const char *txt, char **usuario, char **host, int *puerto);

Conexion  *con_nueva(const char *host, int puerto, const char *usuario,
                     const char *identidad, const char *known_hosts);
gboolean   con_conectar(Conexion *c, EstadoHost *estado, char **huella, GError **err);
gboolean   con_guardar_host(Conexion *c, GError **err);
void       con_preguntas(Conexion *c, ConPreguntas cb, gpointer data);
gboolean   con_autenticar(Conexion *c, const char *clave, GError **err);
char      *con_descripcion(Conexion *c);
/* Usuario, servidor y puerto efectivos (tras aplicar ~/.ssh/config); liberar con g_free */
void       con_datos(Conexion *c, char **usuario, char **host, char **puerto);
char      *con_ruta_inicial(Conexion *c, GError **err);
GPtrArray *con_listar(Conexion *c, const char *ruta, GError **err);
gboolean   con_mkdir(Conexion *c, const char *ruta, GError **err);
gboolean   con_crear_archivo(Conexion *c, const char *ruta, GError **err);  /* vacío, falla si existe */
gboolean   con_borrar(Conexion *c, const char *ruta, GError **err);
gboolean   con_renombrar(Conexion *c, const char *de, const char *a, GError **err);
gboolean   con_subir(Conexion *c, const char *local, const char *dir_remoto,
                     ConProgreso cb, gpointer data, GError **err);
gboolean   con_bajar(Conexion *c, const char *remoto, const char *dir_local,
                     ConProgreso cb, gpointer data, GError **err);
/* Servidor de salto: c viaja por un túnel abierto en "salto" (c pasa a ser su dueño).
 * Orden: conectar y autenticar el salto (con_autenticar_salto), con_tunel(c), y recién
 * entonces con_conectar/con_autenticar de c como siempre. */
void       con_usar_salto(Conexion *c, Conexion *salto);
Conexion  *con_salto(Conexion *c);
gboolean   con_autenticar_salto(Conexion *c, const char *clave, GError **err);  /* sin abrir SFTP */
gboolean   con_tunel(Conexion *c, GError **err);
gboolean   con_conectada(Conexion *c);             /* según lo último que supo libssh */
gboolean   con_vivo(Conexion *c, GError **err);    /* pregunta al servidor; FALSE = conexión perdida */
void       con_cortar(Conexion *c);                /* solo pruebas: simula un corte de red */
void       con_cerrar(Conexion *c);

GPtrArray *local_listar(const char *ruta, GError **err);
gboolean   local_mkdir(const char *ruta, GError **err);
gboolean   local_crear_archivo(const char *ruta, GError **err);
gboolean   local_borrar(const char *ruta, GError **err);
gboolean   local_renombrar(const char *de, const char *a, GError **err);

void       entrada_free(gpointer e);
char      *ruta_unir(const char *dir, const char *nombre);
/* Normaliza una ruta remota (siempre con /) respecto de "base": quita ".", ".." y // */
char      *ruta_remota_normalizar(const char *ruta, const char *base);

/* Ruta local de Windows: "C:\…" o "\\servidor\…". Las remotas empiezan con / */
static inline gboolean es_ruta_windows(const char *r)
{
    return r && ((g_ascii_isalpha(r[0]) && r[1] == ':') || (r[0] == '\\' && r[1] == '\\'));
}

/* Prueba de punta a punta sin interfaz (src/prueba.c) */
int        prueba_ejecutar(const char *destino, const char *identidad,
                           const char *known_hosts, gboolean aceptar_host, const char *salto);
