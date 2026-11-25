/* Prueba de punta a punta de la capa SFTP, sin interfaz:
 * conecta, sube una carpeta, la lista, la baja, compara, renombra y borra. */
#include "sftp.h"
#include "llavero.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fallas = 0;
static GError *err = NULL;

static gboolean paso(gboolean ok, const char *que)
{
    printf("%s  %s%s%s\n", ok ? "OK   " : "FALLA", que, err ? ": " : "", err ? err->message : "");
    if (!ok) fallas++;
    g_clear_error(&err);
    return ok;
}
#define PASO(cond, msg) do { if (!paso((cond), (msg))) return; } while (0)

static gboolean iguales(const char *a, const char *b)
{
    char *ca = NULL, *cb = NULL;
    gsize la = 0, lb = 0;
    gboolean r = g_file_get_contents(a, &ca, &la, NULL) && g_file_get_contents(b, &cb, &lb, NULL) &&
                 la == lb && memcmp(ca, cb, la) == 0;
    g_free(ca);
    g_free(cb);
    return r;
}

static gboolean contiene(GPtrArray *l, const char *nombre)
{
    for (guint i = 0; l && i < l->len; i++)
        if (strcmp(((Entrada *)g_ptr_array_index(l, i))->nombre, nombre) == 0) return TRUE;
    return FALSE;
}

/* Preguntas del servidor (OTP, aprobación web) en la terminal: una respuesta por línea */
static gboolean preguntas_terminal(const char *nombre, const char *instr, int n,
                                   const char * const *prompts, const gboolean *eco,
                                   char **resp, gpointer data)
{
    (void)eco; (void)data;
    if (nombre && *nombre) printf("       PREGUNTA [%s]\n", nombre);
    if (instr && *instr) printf("       %s\n", instr);
    for (int i = 0; i < n; i++) {
        char linea[512];
        printf("       %s", prompts[i]);
        fflush(stdout);
        if (!fgets(linea, sizeof linea, stdin)) return FALSE;   /* sin respuesta: cancelar */
        linea[strcspn(linea, "\r\n")] = '\0';
        printf("(respondido)\n");
        resp[i] = g_strdup(linea);
    }
    return TRUE;
}

/* Conecta y verifica la llave del servidor; FALSE si algo falló */
static gboolean conectar_verificar(Conexion *c, gboolean aceptar, const char *que)
{
    EstadoHost est;
    char *huella = NULL;
    char *t = g_strdup_printf("conectar %s", que);
    gboolean ok = paso(con_conectar(c, &est, &huella, &err), t);
    g_free(t);
    if (!ok) return FALSE;
    printf("       huella %s\n", huella);
    g_free(huella);
    if (!paso(est != HOST_CAMBIADO, "llave del servidor sin cambios")) return FALSE;
    if (est == HOST_NUEVO) {
        if (!paso(aceptar, "servidor conocido (usa --aceptar-host para guardarlo)")) return FALSE;
        if (!paso(con_guardar_host(c, &err), "guardar en known_hosts")) return FALSE;
    }
    return TRUE;
}

static void probar(Conexion *c, gboolean aceptar)
{
    if (!conectar_verificar(c, aceptar, con_salto(c) ? "al destino por el túnel" : "")) return;
    PASO(con_autenticar(c, g_getenv("SFTPC_CLAVE"), &err), "autenticar");
    char *home = con_ruta_inicial(c, &err);
    PASO(home != NULL, "carpeta inicial");
    printf("       %s\n", home);

    /* datos locales de prueba: texto con acentos, binario de 300 KB y un vacío */
    char *tmp = g_dir_make_tmp("sftpc-XXXXXX", NULL);
    char *datos = g_build_filename(tmp, "datos", NULL);
    char *sub = g_build_filename(datos, "sub", NULL);
    g_mkdir_with_parents(sub, 0755);
    char *fa = g_build_filename(datos, "a.txt", NULL);
    char *fb = g_build_filename(sub, "b.bin", NULL);
    char *fc = g_build_filename(datos, "c.vacio", NULL);
    g_file_set_contents(fa, "Hola ñandú, áéíóú\n", -1, NULL);
    gsize nb = 300000;
    guint8 *bin = g_malloc(nb);
    for (gsize i = 0; i < nb; i++) bin[i] = (guint8)g_random_int();
    g_file_set_contents(fb, (char *)bin, nb, NULL);
    g_file_set_contents(fc, "", 0, NULL);

    char *nombre = g_strdup_printf(".sftpc-prueba-%d", (int)getpid());
    char *base = ruta_unir(home, nombre);
    char *rdatos = ruta_unir(base, "datos");
    PASO(con_mkdir(c, base, &err), "crear carpeta remota");
    PASO(con_subir(c, datos, base, NULL, NULL, &err), "subir carpeta (recursivo)");
    GPtrArray *l = con_listar(c, rdatos, &err);
    PASO(l && l->len == 3 && contiene(l, "sub"), "listar remoto (3 entradas)");

    char *bajada = g_build_filename(tmp, "bajada", NULL);
    g_mkdir(bajada, 0755);
    PASO(con_bajar(c, rdatos, bajada, NULL, NULL, &err), "bajar carpeta (recursivo)");
    char *ba = g_build_filename(bajada, "datos", "a.txt", NULL);
    char *bb = g_build_filename(bajada, "datos", "sub", "b.bin", NULL);
    char *bc = g_build_filename(bajada, "datos", "c.vacio", NULL);
    PASO(iguales(fa, ba) && iguales(fb, bb) && iguales(fc, bc), "contenido idéntico tras subir y bajar");

    char *de = ruta_unir(rdatos, "a.txt"), *a = ruta_unir(rdatos, "a2.txt");
    PASO(con_renombrar(c, de, a, &err), "renombrar");
    l = con_listar(c, rdatos, &err);
    PASO(contiene(l, "a2.txt") && !contiene(l, "a.txt"), "listar tras renombrar");

    char *nuevo = ruta_unir(rdatos, "nuevo.txt");
    PASO(con_crear_archivo(c, nuevo, &err), "crear documento vacío");
    PASO(!con_crear_archivo(c, nuevo, &err), "no pisa un documento que ya existe");
    l = con_listar(c, rdatos, &err);
    PASO(contiene(l, "nuevo.txt"), "listar tras crear documento");

    PASO(con_borrar(c, base, &err), "borrar carpeta remota (recursivo)");
    l = con_listar(c, home, &err);
    PASO(l && !contiene(l, nombre), "ya no existe en el servidor");

    PASO(local_borrar(tmp, &err), "limpiar temporales locales");
}

/* Guarda, lee y borra una contraseña de prueba en el llavero del sistema */
static void probar_llavero(void)
{
    const char *srv = "prueba.sftpc.invalid", *nombre = "sftpc-prueba-llavero";
    if (!llavero_guardar(nombre, srv, "22", "prueba", "clave-de-prueba-ñ", &err)) {
        printf("OMITIDO llavero no disponible: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    paso(TRUE, "guardar contraseña en el llavero");
    char *c = llavero_buscar(srv, "22", "prueba", &err);
    PASO(g_strcmp0(c, "clave-de-prueba-ñ") == 0, "leerla del llavero");
    llavero_liberar(c);
    PASO(llavero_olvidar_nombre(nombre, &err), "olvidarla por nombre de servidor");
    c = llavero_buscar(srv, "22", "prueba", &err);
    PASO(c == NULL, "ya no está en el llavero");
}

int prueba_ejecutar(const char *destino, const char *identidad, const char *known_hosts, gboolean aceptar,
                    const char *salto)
{
    char *usuario, *host, *su = NULL, *sh = NULL;
    int puerto, sp = 0;
    if (!destino || !destino_parsear(destino, &usuario, &host, &puerto) ||
        (salto && !destino_parsear(salto, &su, &sh, &sp))) {
        fprintf(stderr, "Uso: sftp-commander --prueba usuario@host[:puerto] [-J salto] [-i llave] "
                        "[--known-hosts archivo] [--aceptar-host]\n");
        return 2;
    }
    Conexion *c = con_nueva(host, puerto, usuario, identidad, known_hosts);
    con_preguntas(c, preguntas_terminal, NULL);
    gboolean seguir = TRUE;
    if (salto) {
        /* primero el servidor de salto, después el túnel hacia el destino */
        Conexion *j = con_nueva(sh, sp, su, identidad, known_hosts);
        con_preguntas(j, preguntas_terminal, NULL);
        con_usar_salto(c, j);
        seguir = conectar_verificar(j, aceptar, "al servidor de salto") &&
                 paso(con_autenticar_salto(j, g_getenv("SFTPC_CLAVE"), &err), "autenticar en el salto") &&
                 paso(con_tunel(c, &err), "abrir el túnel hacia el destino");
    }
    if (seguir) probar(c, aceptar);
    con_cerrar(c);
    g_free(usuario); g_free(host); g_free(su); g_free(sh);
    probar_llavero();
    if (fallas) printf("\nRESULTADO: %d falla(s)\n", fallas);
    else printf("\nRESULTADO: todo OK\n");
    return fallas ? 1 : 0;
}
