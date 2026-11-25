#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#endif
#include "sftp.h"
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#endif

#define BLOQUE (32 * 1024)   /* tamaño seguro para cualquier servidor SFTP */

/* ---------- diferencias Linux / Windows en sockets ---------- */
#ifdef _WIN32
#define SHUT_RDWR SD_BOTH
#define sock_cerrar(s)        closesocket(s)
#define sock_leer(s, b, n)    recv((s), (b), (int)(n), 0)
#define sock_escribir(s, b, n) send((s), (b), (int)(n), 0)
#define sondear(f, n, t)      WSAPoll((f), (n), (t))
#define OPT(x) ((const char *)(x))
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
/* Windows no tiene socketpair: un par TCP por 127.0.0.1. Se comprueba que quien
 * se conectó al puerto de escucha sea nuestro propio extremo y no otro proceso. */
static int par_sockets(socket_t par[2])
{
    socket_t esc = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    par[0] = par[1] = SSH_INVALID_SOCKET;
    if (esc == INVALID_SOCKET) return -1;
    struct sockaddr_in a = { 0 }, b, c;
    int la = sizeof a, lb = sizeof b, lc = sizeof c;
    BOOL excl = TRUE;
    setsockopt(esc, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, OPT(&excl), sizeof excl);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(esc, (struct sockaddr *)&a, sizeof a) || getsockname(esc, (struct sockaddr *)&a, &la) ||
        listen(esc, 1)) goto mal;
    par[1] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (par[1] == INVALID_SOCKET || connect(par[1], (struct sockaddr *)&a, sizeof a)) goto mal;
    par[0] = accept(esc, (struct sockaddr *)&b, &lb);
    if (par[0] == INVALID_SOCKET || getsockname(par[1], (struct sockaddr *)&c, &lc) ||
        b.sin_port != c.sin_port || b.sin_addr.s_addr != c.sin_addr.s_addr) goto mal;
    closesocket(esc);
    int si = 1;   /* sin Nagle: el túnel lleva mensajes chicos de ida y vuelta */
    for (int i = 0; i < 2; i++) setsockopt(par[i], IPPROTO_TCP, TCP_NODELAY, OPT(&si), sizeof si);
    return 0;
mal:
    closesocket(esc);
    for (int i = 0; i < 2; i++)
        if (par[i] != SSH_INVALID_SOCKET && par[i] != INVALID_SOCKET) closesocket(par[i]);
    par[0] = par[1] = SSH_INVALID_SOCKET;
    return -1;
}
#else
#define sock_cerrar(s)         close(s)
#define sock_leer(s, b, n)     read((s), (b), (n))
#define sock_escribir(s, b, n) write((s), (b), (n))
#define sondear(f, n, t)       poll((f), (n), (t))
#define OPT(x) (x)
static int par_sockets(socket_t par[2]) { return socketpair(AF_UNIX, SOCK_STREAM, 0, par); }
#endif

struct Conexion {
    ssh_session  ssh;
    sftp_session sftp;
    char        *identidad;
    GMutex       mutex;
    /* servidor de salto: esta conexión viaja por un canal abierto en "salto" */
    Conexion    *salto;
    ssh_channel  canal;     /* (en el salto) canal direct-tcpip hacia el destino */
    socket_t     par[2];    /* (en el salto) socketpair: [0] lo usa la bomba, [1] el destino */
    GThread     *bomba;     /* (en el salto) hilo que pasa datos entre canal y socketpair */
    gint         parar;
    ConPreguntas preguntas; /* quién contesta las preguntas del servidor (OTP, web…) */
    gpointer     preguntas_data;
};

G_DEFINE_QUARK(sftp-commander-error, con_error)

void con_init(void) { ssh_init(); }

void entrada_free(gpointer p)
{
    Entrada *e = p;
    if (!e) return;
    g_free(e->nombre);
    g_free(e);
}

char *ruta_unir(const char *dir, const char *nombre)
{
#ifdef _WIN32
    /* ruta local de Windows (C:\…): con su separador. Las remotas siempre usan / */
    if (es_ruta_windows(dir)) return g_build_filename(dir, nombre, NULL);
#endif
    if (g_str_has_suffix(dir, "/")) return g_strconcat(dir, nombre, NULL);
    return g_strconcat(dir, "/", nombre, NULL);
}

char *ruta_remota_normalizar(const char *ruta, const char *base)
{
    char *todo = ruta[0] == '/' ? g_strdup(ruta) : g_strconcat(base ? base : "/", "/", ruta, NULL);
    char **partes = g_strsplit(todo, "/", -1);
    GPtrArray *pila = g_ptr_array_new();
    for (char **p = partes; *p; p++) {
        if (!**p || strcmp(*p, ".") == 0) continue;
        if (strcmp(*p, "..") == 0) { if (pila->len) g_ptr_array_set_size(pila, pila->len - 1); continue; }
        g_ptr_array_add(pila, *p);
    }
    GString *r = g_string_new(NULL);
    for (guint i = 0; i < pila->len; i++) g_string_append_printf(r, "/%s", (char *)pila->pdata[i]);
    if (!r->len) g_string_append_c(r, '/');
    g_ptr_array_free(pila, TRUE);
    g_strfreev(partes);
    g_free(todo);
    return g_string_free(r, FALSE);
}

gboolean destino_parsear(const char *txt, char **usuario, char **host, int *puerto)
{
    *usuario = NULL; *host = NULL; *puerto = 0;
    if (!txt) return FALSE;
    char *s = g_strstrip(g_strdup(txt));
    char *p = s;
    if (g_str_has_prefix(p, "sftp://")) p += 7;
    else if (g_str_has_prefix(p, "scp://")) p += 6;
    char *barra = strchr(p, '/');          /* la ruta de la URL se ignora por ahora */
    if (barra) *barra = '\0';
    char *arroba = strrchr(p, '@');
    if (arroba) {
        if (arroba > p) *usuario = g_strndup(p, arroba - p);
        p = arroba + 1;
    }
    char *dos = strrchr(p, ':');
    if (dos) {
        *puerto = atoi(dos + 1);
        *dos = '\0';
    }
    if (*p) *host = g_strdup(p);
    g_free(s);
    if (!*host) { g_clear_pointer(usuario, g_free); return FALSE; }
    return TRUE;
}

static const char *texto_sftp(int cod)
{
    switch (cod) {
    case SSH_FX_NO_SUCH_FILE:        return "no existe";
    case SSH_FX_PERMISSION_DENIED:   return "permiso denegado";
    case SSH_FX_FILE_ALREADY_EXISTS: return "ya existe";
    case SSH_FX_NO_SUCH_PATH:        return "la ruta no existe";
    case SSH_FX_WRITE_PROTECT:       return "solo lectura";
    default:                         return NULL;
    }
}

static gboolean fallo_sftp(Conexion *c, GError **err, const char *que, const char *ruta)
{
    const char *t = c->sftp ? texto_sftp(sftp_get_error(c->sftp)) : NULL;
    g_set_error(err, CON_ERROR, CON_ERR_SFTP, "%s %s: %s", que, ruta, t ? t : ssh_get_error(c->ssh));
    return FALSE;
}

static gboolean fallo_local(GError **err, const char *que, const char *ruta, int e)
{
    g_set_error(err, CON_ERROR, CON_ERR_LOCAL, "%s %s: %s", que, ruta, g_strerror(e));
    return FALSE;
}

/* ---------- conexión ---------- */

Conexion *con_nueva(const char *host, int puerto, const char *usuario,
                    const char *identidad, const char *known_hosts)
{
    Conexion *c = g_new0(Conexion, 1);
    g_mutex_init(&c->mutex);
    c->par[0] = c->par[1] = SSH_INVALID_SOCKET;
    c->ssh = ssh_new();
    c->identidad = g_strdup(identidad);
    ssh_options_set(c->ssh, SSH_OPTIONS_HOST, host);
    if (usuario && *usuario) ssh_options_set(c->ssh, SSH_OPTIONS_USER, usuario);
    if (puerto > 0) {
        unsigned int p = puerto;
        ssh_options_set(c->ssh, SSH_OPTIONS_PORT, &p);
    }
    if (known_hosts) ssh_options_set(c->ssh, SSH_OPTIONS_KNOWNHOSTS, known_hosts);
    long espera = 15;
    ssh_options_set(c->ssh, SSH_OPTIONS_TIMEOUT, &espera);
    /* Lee ~/.ssh/config: permite usar alias. Lo ya fijado arriba tiene prioridad. */
    ssh_options_parse_config(c->ssh, NULL);
    return c;
}

gboolean con_conectar(Conexion *c, EstadoHost *estado, char **huella, GError **err)
{
    gboolean ok = FALSE;
    ssh_key llave = NULL;
    unsigned char *hash = NULL;
    size_t hlen = 0;
    char *fp = NULL;

    g_mutex_lock(&c->mutex);
    if (ssh_connect(c->ssh) != SSH_OK) {
        g_set_error(err, CON_ERROR, CON_ERR_RED, "No se pudo conectar: %s", ssh_get_error(c->ssh));
        goto fin;
    }
    /* Detectar cortes de red: keepalive de TCP y límite para escrituras sin respuesta.
     * Sin esto, una conexión muerta (Wi-Fi caído, VPN cortada) puede colgar una lectura. */
    socket_t fd = ssh_get_fd(c->ssh);
    if (fd != SSH_INVALID_SOCKET) {
        int si = 1, inactivo = 30, intervalo = 10, intentos = 3;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, OPT(&si), sizeof si);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, OPT(&inactivo), sizeof inactivo);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, OPT(&intervalo), sizeof intervalo);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, OPT(&intentos), sizeof intentos);
#ifdef _WIN32
        DWORD limite_s = 20;   /* equivalente de Windows: reintentos máximos en segundos */
        setsockopt(fd, IPPROTO_TCP, TCP_MAXRT, OPT(&limite_s), sizeof limite_s);
#else
        unsigned int limite_ms = 20000;
        setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &limite_ms, sizeof limite_ms);
#endif
    }
    if (ssh_get_server_publickey(c->ssh, &llave) != SSH_OK ||
        ssh_get_publickey_hash(llave, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) != 0) {
        g_set_error(err, CON_ERROR, CON_ERR_RED, "No se pudo leer la llave del servidor: %s",
                    ssh_get_error(c->ssh));
        goto fin;
    }
    fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
    *huella = g_strdup(fp ? fp : "?");
    switch (ssh_session_is_known_server(c->ssh)) {
    case SSH_KNOWN_HOSTS_OK:
        *estado = HOST_OK; ok = TRUE; break;
    case SSH_KNOWN_HOSTS_NOT_FOUND:
    case SSH_KNOWN_HOSTS_UNKNOWN:
        *estado = HOST_NUEVO; ok = TRUE; break;
    case SSH_KNOWN_HOSTS_CHANGED:
    case SSH_KNOWN_HOSTS_OTHER:
        *estado = HOST_CAMBIADO; ok = TRUE; break;
    default:
        g_set_error(err, CON_ERROR, CON_ERR_RED, "Error leyendo known_hosts: %s", ssh_get_error(c->ssh));
    }
fin:
    if (fp) ssh_string_free_char(fp);
    if (hash) ssh_clean_pubkey_hash(&hash);
    if (llave) ssh_key_free(llave);
    g_mutex_unlock(&c->mutex);
    return ok;
}

gboolean con_guardar_host(Conexion *c, GError **err)
{
    g_mutex_lock(&c->mutex);
    gboolean ok = ssh_session_update_known_hosts(c->ssh) == SSH_OK;
    if (!ok)
        g_set_error(err, CON_ERROR, CON_ERR_RED, "No se pudo guardar en known_hosts: %s", ssh_get_error(c->ssh));
    g_mutex_unlock(&c->mutex);
    return ok;
}

static gboolean autenticar(Conexion *c, const char *clave, gboolean abrir_sftp, GError **err);

gboolean con_autenticar(Conexion *c, const char *clave, GError **err)
{
    return autenticar(c, clave, TRUE, err);
}

/* El servidor de salto solo necesita SSH: puede no tener SFTP */
gboolean con_autenticar_salto(Conexion *c, const char *clave, GError **err)
{
    return autenticar(c, clave, FALSE, err);
}

void con_preguntas(Conexion *c, ConPreguntas cb, gpointer data)
{
    c->preguntas = cb;
    c->preguntas_data = data;
}

/* ¿La pregunta del servidor pide la contraseña (y no un código de un solo uso)? */
static gboolean pide_clave(const char *prompt, gboolean eco)
{
    if (eco || !prompt) return FALSE;
    char *p = g_utf8_strdown(prompt, -1);
    gboolean r = (strstr(p, "password") || strstr(p, "contraseña") || strstr(p, "passphrase")) &&
                 !strstr(p, "one-time") && !strstr(p, "otp") && !strstr(p, "code") &&
                 !strstr(p, "código") && !strstr(p, "token") && !strstr(p, "verific");
    g_free(p);
    return r;
}

/* keyboard-interactive: el servidor manda preguntas (contraseña, código OTP, un enlace
 * para aprobar en el navegador…). La contraseña se contesta sola UNA vez; lo demás lo
 * contesta el usuario. Nunca se repite una respuesta: un bastión puede bloquear la
 * cuenta tras varios intentos fallidos. */
static int kbdint(Conexion *c, const char *clave, gboolean *clave_usada, gboolean *respondio,
                  gboolean *cancelado, gboolean *pidio_clave)
{
    int rc = ssh_userauth_kbdint(c->ssh, NULL, NULL);
    for (int vuelta = 0; rc == SSH_AUTH_INFO && vuelta < 20; vuelta++) {
        int n = ssh_userauth_kbdint_getnprompts(c->ssh);
        const char *nombre = ssh_userauth_kbdint_getname(c->ssh);
        const char *instr = ssh_userauth_kbdint_getinstruction(c->ssh);
        const char **prompts = g_new0(const char *, n + 1);
        gboolean *eco = g_new0(gboolean, n + 1);
        char **resp = g_new0(char *, n + 1);
        for (int i = 0; i < n; i++) {
            char e = 0;
            prompts[i] = ssh_userauth_kbdint_getprompt(c->ssh, i, &e);
            eco[i] = e != 0;
        }
        gboolean ok = TRUE;
        if (n == 1 && pide_clave(prompts[0], eco[0])) {
            *pidio_clave = TRUE;
            /* la contraseña: una sola vez; si la vuelve a pedir, era incorrecta */
            if (clave && *clave && !*clave_usada) {
                resp[0] = g_strdup(clave);
                *clave_usada = TRUE;
            } else {
                ok = FALSE;
            }
        } else if (n > 0 || (instr && *instr)) {
            if (c->preguntas) {
                ok = c->preguntas(nombre, instr, n, prompts, eco, resp, c->preguntas_data);
                if (!ok) *cancelado = TRUE;
                else *respondio = TRUE;
            } else {
                ok = FALSE;
            }
        }
        if (ok)
            for (int i = 0; i < n; i++) ssh_userauth_kbdint_setanswer(c->ssh, i, resp[i] ? resp[i] : "");
        for (int i = 0; i < n; i++)
            if (resp[i]) { memset(resp[i], 0, strlen(resp[i])); g_free(resp[i]); }
        g_free(resp);
        g_free(eco);
        g_free(prompts);
        if (!ok) return SSH_AUTH_DENIED;
        rc = ssh_userauth_kbdint(c->ssh, NULL, NULL);
    }
    return rc;
}

static gboolean autenticar(Conexion *c, const char *clave, gboolean abrir_sftp, GError **err)
{
    gboolean ok = FALSE;
    gboolean hay_clave = clave && *clave;
    gboolean llave_probada = FALSE, clave_probada = FALSE, kbd_probado = FALSE;
    gboolean clave_usada = FALSE, respondio = FALSE, cancelado = FALSE;
    /* ¿el servidor acepta contraseña? (método "password" o una pregunta que la pide) */
    gboolean acepta_clave = FALSE;

    g_mutex_lock(&c->mutex);
    int rc = ssh_userauth_none(c->ssh, NULL);
    /* Cada método se prueba una vez. Con "éxito parcial" (el servidor pide además otro
     * método, por ejemplo llave + código) se sigue con los que falten. */
    for (int vuelta = 0; vuelta < 8 && rc != SSH_AUTH_SUCCESS && rc != SSH_AUTH_ERROR && !cancelado; vuelta++) {
        int metodos = ssh_userauth_list(c->ssh, NULL);
        if (metodos & SSH_AUTH_METHOD_PASSWORD) acepta_clave = TRUE;
        /* preguntas que no se pueden mostrar: quizá pedían la contraseña, no se descarta */
        if ((metodos & SSH_AUTH_METHOD_INTERACTIVE) && !hay_clave && !c->preguntas) acepta_clave = TRUE;
        if ((metodos & SSH_AUTH_METHOD_PUBLICKEY) && !llave_probada) {
            /* 1) llaves: la indicada, o las de ~/.ssh y ssh-agent */
            llave_probada = TRUE;
            rc = SSH_AUTH_DENIED;
            if (c->identidad) {
                ssh_key k = NULL;
                if (ssh_pki_import_privkey_file(c->identidad, NULL, NULL, NULL, &k) == SSH_OK) {
                    rc = ssh_userauth_publickey(c->ssh, NULL, k);
                    ssh_key_free(k);
                }
            } else {
                rc = ssh_userauth_publickey_auto(c->ssh, NULL, NULL);
            }
        } else if ((metodos & SSH_AUTH_METHOD_PASSWORD) && hay_clave && !clave_probada) {
            /* 2) contraseña */
            clave_probada = clave_usada = TRUE;
            rc = ssh_userauth_password(c->ssh, NULL, clave);
        } else if ((metodos & SSH_AUTH_METHOD_INTERACTIVE) && !kbd_probado && (hay_clave || c->preguntas)) {
            /* 3) keyboard-interactive: contraseña por PAM, códigos, aprobación web */
            kbd_probado = TRUE;
            rc = kbdint(c, clave, &clave_usada, &respondio, &cancelado, &acepta_clave);
        } else {
            break;   /* no queda nada por probar */
        }
        if (rc == SSH_AUTH_PARTIAL) kbd_probado = FALSE;   /* puede pedir otra ronda de preguntas */
    }
    if (cancelado) {
        g_set_error_literal(err, CON_ERROR, CON_ERR_CANCELADO, "Inicio de sesión cancelado");
        goto fin;
    }
    if (rc == SSH_AUTH_ERROR) {
        g_set_error(err, CON_ERROR, CON_ERR_RED, "Error de autenticación: %s", ssh_get_error(c->ssh));
        goto fin;
    }
    if (rc != SSH_AUTH_SUCCESS) {
        if (respondio)   /* contestaste preguntas (código, aprobación) y no alcanzó */
            g_set_error_literal(err, CON_ERROR, CON_ERR_CODIGO,
                "El servidor rechazó el inicio de sesión: revisa el código o la aprobación y vuelve a conectar.");
        else if (!acepta_clave)   /* pedir una contraseña no serviría de nada */
            g_set_error_literal(err, CON_ERROR, CON_ERR_SIN_CLAVE, hay_clave
                ? "Este servidor no acepta contraseña: no se envió. Deja la contraseña vacía; "
                  "entra con una llave SSH registrada en el servidor o con las preguntas que haga (código 2FA, aprobación)."
                : "El servidor no aceptó tus llaves SSH y no acepta contraseña. "
                  "Registra tu llave pública en el servidor (en un bastión como Warpgate, en tu perfil de la web).");
        else
            g_set_error_literal(err, CON_ERROR, CON_ERR_AUTH, hay_clave
                ? "Usuario o contraseña incorrectos."
                : "El servidor no aceptó tus llaves SSH. Escribe la contraseña y vuelve a conectar.");
        goto fin;
    }
    if (!abrir_sftp) { ok = TRUE; goto fin; }
    c->sftp = sftp_new(c->ssh);
    if (!c->sftp || sftp_init(c->sftp) != SSH_OK) {
        /* Un bastión como Warpgate corta aquí si todavía no confía en la llave del servidor
         * de destino: solo la pregunta en una terminal (PTY), no por SFTP. */
        g_set_error(err, CON_ERROR, CON_ERR_SFTP,
                    "El servidor no abrió SFTP (%s). Si entras por un bastión como Warpgate y es la primera vez "
                    "con ese destino, conéctate una vez con ssh en una terminal para aceptar su llave, "
                    "o pide al administrador que la confirme.", ssh_get_error(c->ssh));
        if (c->sftp) { sftp_free(c->sftp); c->sftp = NULL; }
        goto fin;
    }
    ok = TRUE;
fin:
    g_mutex_unlock(&c->mutex);
    return ok;
}

char *con_descripcion(Conexion *c)
{
    char *u = NULL, *h = NULL;
    unsigned int p = 22;
    g_mutex_lock(&c->mutex);
    ssh_options_get(c->ssh, SSH_OPTIONS_USER, &u);
    ssh_options_get(c->ssh, SSH_OPTIONS_HOST, &h);
    ssh_options_get_port(c->ssh, &p);
    g_mutex_unlock(&c->mutex);
    char *r = p == 22 ? g_strdup_printf("%s@%s", u ? u : "?", h ? h : "?")
                      : g_strdup_printf("%s@%s:%u", u ? u : "?", h ? h : "?", p);
    if (u) ssh_string_free_char(u);
    if (h) ssh_string_free_char(h);
    return r;
}

void con_datos(Conexion *c, char **usuario, char **host, char **puerto)
{
    char *u = NULL, *h = NULL;
    unsigned int p = 22;
    g_mutex_lock(&c->mutex);
    ssh_options_get(c->ssh, SSH_OPTIONS_USER, &u);
    ssh_options_get(c->ssh, SSH_OPTIONS_HOST, &h);
    ssh_options_get_port(c->ssh, &p);
    g_mutex_unlock(&c->mutex);
    *usuario = g_strdup(u ? u : "");
    *host = g_strdup(h ? h : "");
    *puerto = g_strdup_printf("%u", p);
    if (u) ssh_string_free_char(u);
    if (h) ssh_string_free_char(h);
}

char *con_ruta_inicial(Conexion *c, GError **err)
{
    g_mutex_lock(&c->mutex);
    char *r = sftp_canonicalize_path(c->sftp, ".");
    char *res = NULL;
    if (r) { res = g_strdup(r); ssh_string_free_char(r); }
    else fallo_sftp(c, err, "No se pudo leer la carpeta inicial", ".");
    g_mutex_unlock(&c->mutex);
    return res;
}

GPtrArray *con_listar(Conexion *c, const char *ruta, GError **err)
{
    GPtrArray *res = NULL;
    g_mutex_lock(&c->mutex);
    sftp_dir d = sftp_opendir(c->sftp, ruta);
    if (!d) { fallo_sftp(c, err, "No se pudo abrir", ruta); goto fin; }
    res = g_ptr_array_new_with_free_func(entrada_free);
    sftp_attributes a;
    while ((a = sftp_readdir(c->sftp, d)) != NULL) {
        if (strcmp(a->name, ".") != 0 && strcmp(a->name, "..") != 0) {
            Entrada *e = g_new0(Entrada, 1);
            e->nombre = g_strdup(a->name);
            e->tam = a->size;
            e->mtime = a->mtime;
            e->permisos = a->permissions;
            e->es_dir = a->type == SSH_FILEXFER_TYPE_DIRECTORY;
            if (a->type == SSH_FILEXFER_TYPE_SYMLINK) {   /* ver a qué apunta */
                char *full = ruta_unir(ruta, a->name);
                sftp_attributes t = sftp_stat(c->sftp, full);
                if (t) {
                    e->es_dir = t->type == SSH_FILEXFER_TYPE_DIRECTORY;
                    e->tam = t->size;
                    sftp_attributes_free(t);
                }
                g_free(full);
            }
            g_ptr_array_add(res, e);
        }
        sftp_attributes_free(a);
    }
    if (!sftp_dir_eof(d)) {
        fallo_sftp(c, err, "Error leyendo", ruta);
        g_clear_pointer(&res, g_ptr_array_unref);
    }
    sftp_closedir(d);
fin:
    g_mutex_unlock(&c->mutex);
    return res;
}

gboolean con_mkdir(Conexion *c, const char *ruta, GError **err)
{
    g_mutex_lock(&c->mutex);
    gboolean ok = sftp_mkdir(c->sftp, ruta, 0755) == 0 || fallo_sftp(c, err, "No se pudo crear", ruta);
    g_mutex_unlock(&c->mutex);
    return ok;
}

gboolean con_crear_archivo(Conexion *c, const char *ruta, GError **err)
{
    g_mutex_lock(&c->mutex);
    sftp_file f = sftp_open(c->sftp, ruta, O_WRONLY | O_CREAT | O_EXCL, 0644);
    gboolean ok = f != NULL;
    if (f) sftp_close(f);
    else fallo_sftp(c, err, "No se pudo crear", ruta);
    g_mutex_unlock(&c->mutex);
    return ok;
}

gboolean con_renombrar(Conexion *c, const char *de, const char *a, GError **err)
{
    g_mutex_lock(&c->mutex);
    gboolean ok = sftp_rename(c->sftp, de, a) == 0 || fallo_sftp(c, err, "No se pudo renombrar", de);
    g_mutex_unlock(&c->mutex);
    return ok;
}

/* Hijos de una carpeta remota (sin . ni ..); omite enlaces a carpetas para no entrar en ciclos */
static GPtrArray *hijos_remotos(Conexion *c, const char *ruta, gboolean omitir_enlaces_dir, GError **err)
{
    sftp_dir d = sftp_opendir(c->sftp, ruta);
    if (!d) { fallo_sftp(c, err, "No se pudo abrir", ruta); return NULL; }
    GPtrArray *r = g_ptr_array_new_with_free_func(g_free);
    sftp_attributes a;
    while ((a = sftp_readdir(c->sftp, d)) != NULL) {
        gboolean usar = strcmp(a->name, ".") != 0 && strcmp(a->name, "..") != 0;
        char *full = ruta_unir(ruta, a->name);
        if (usar && omitir_enlaces_dir && a->type == SSH_FILEXFER_TYPE_SYMLINK) {
            sftp_attributes t = sftp_stat(c->sftp, full);
            if (t) {
                if (t->type == SSH_FILEXFER_TYPE_DIRECTORY) usar = FALSE;
                sftp_attributes_free(t);
            }
        }
        if (usar) g_ptr_array_add(r, full); else g_free(full);
        sftp_attributes_free(a);
    }
    sftp_closedir(d);
    return r;
}

static gboolean borrar_rec(Conexion *c, const char *ruta, GError **err)
{
    sftp_attributes a = sftp_lstat(c->sftp, ruta);
    if (!a) return fallo_sftp(c, err, "No se pudo leer", ruta);
    gboolean dir = a->type == SSH_FILEXFER_TYPE_DIRECTORY;
    sftp_attributes_free(a);
    if (!dir)
        return sftp_unlink(c->sftp, ruta) == 0 || fallo_sftp(c, err, "No se pudo borrar", ruta);
    GPtrArray *hijos = hijos_remotos(c, ruta, FALSE, err);
    if (!hijos) return FALSE;
    gboolean ok = TRUE;
    for (guint i = 0; ok && i < hijos->len; i++) ok = borrar_rec(c, g_ptr_array_index(hijos, i), err);
    g_ptr_array_unref(hijos);
    return ok && (sftp_rmdir(c->sftp, ruta) == 0 || fallo_sftp(c, err, "No se pudo borrar la carpeta", ruta));
}

gboolean con_borrar(Conexion *c, const char *ruta, GError **err)
{
    g_mutex_lock(&c->mutex);
    gboolean ok = borrar_rec(c, ruta, err);
    g_mutex_unlock(&c->mutex);
    return ok;
}

/* ---------- transferencias ---------- */

static gboolean avisar(ConProgreso cb, gpointer d, const char *nombre, guint64 hecho, guint64 total, GError **err)
{
    if (!cb || cb(nombre, hecho, total, d)) return TRUE;
    g_set_error_literal(err, CON_ERROR, CON_ERR_CANCELADO, "Transferencia cancelada");
    return FALSE;
}

static gboolean subir_archivo(Conexion *c, const char *local, const char *remoto, guint32 modo,
                              guint64 total, ConProgreso cb, gpointer d, GError **err)
{
    FILE *f = g_fopen(local, "rb");
    if (!f) return fallo_local(err, "No se pudo abrir", local, errno);
    sftp_file r = sftp_open(c->sftp, remoto, O_WRONLY | O_CREAT | O_TRUNC, modo);
    if (!r) { fclose(f); return fallo_sftp(c, err, "No se pudo crear", remoto); }

    char *nombre = g_path_get_basename(local);
    char *buf = g_malloc(BLOQUE);
    guint64 hecho = 0;
    gboolean ok = avisar(cb, d, nombre, 0, total, err);
    size_t n;
    while (ok && (n = fread(buf, 1, BLOQUE, f)) > 0) {
        size_t off = 0;
        while (ok && off < n) {
            ssize_t w = sftp_write(r, buf + off, n - off);
            if (w <= 0) ok = fallo_sftp(c, err, "Error escribiendo", remoto);
            else off += (size_t)w;
        }
        hecho += n;
        if (ok) ok = avisar(cb, d, nombre, hecho, total, err);
    }
    if (ok && ferror(f)) ok = fallo_local(err, "Error leyendo", local, errno);
    fclose(f);
    if (sftp_close(r) != SSH_NO_ERROR && ok) ok = fallo_sftp(c, err, "Error cerrando", remoto);
    g_free(buf);
    g_free(nombre);
    return ok;
}

static gboolean subir_rec(Conexion *c, const char *local, const char *remoto,
                          ConProgreso cb, gpointer d, GError **err)
{
    GStatBuf st, lst;
    if (g_stat(local, &st) != 0) return fallo_local(err, "No se pudo leer", local, errno);
#ifdef _WIN32
    /* Windows no tiene permisos Unix (daría 0666): se usan los habituales */
    st.st_mode = (st.st_mode & ~0777) | (S_ISDIR(st.st_mode) ? 0755 : 0644);
#endif
    if (!S_ISDIR(st.st_mode))
        return subir_archivo(c, local, remoto, st.st_mode & 0777, st.st_size, cb, d, err);
    /* no seguir enlaces a carpetas: evita ciclos */
    if (g_lstat(local, &lst) == 0 && S_ISLNK(lst.st_mode)) return TRUE;

    if (sftp_mkdir(c->sftp, remoto, st.st_mode & 0777) != 0) {
        sftp_attributes a = sftp_stat(c->sftp, remoto);   /* ya existe: se usa */
        gboolean es_dir = a && a->type == SSH_FILEXFER_TYPE_DIRECTORY;
        if (a) sftp_attributes_free(a);
        if (!es_dir) return fallo_sftp(c, err, "No se pudo crear la carpeta", remoto);
    }
    GDir *dir = g_dir_open(local, 0, err);
    if (!dir) return FALSE;
    gboolean ok = TRUE;
    const char *n;
    while (ok && (n = g_dir_read_name(dir)) != NULL) {
        char *l = g_build_filename(local, n, NULL);
        char *r = ruta_unir(remoto, n);
        ok = subir_rec(c, l, r, cb, d, err);
        g_free(l);
        g_free(r);
    }
    g_dir_close(dir);
    return ok;
}

gboolean con_subir(Conexion *c, const char *local, const char *dir_remoto,
                   ConProgreso cb, gpointer data, GError **err)
{
    char *base = g_path_get_basename(local);
    char *remoto = ruta_unir(dir_remoto, base);
    g_mutex_lock(&c->mutex);
    gboolean ok = subir_rec(c, local, remoto, cb, data, err);
    g_mutex_unlock(&c->mutex);
    g_free(remoto);
    g_free(base);
    return ok;
}

static gboolean bajar_archivo(Conexion *c, const char *remoto, const char *local, guint32 modo,
                              guint64 total, ConProgreso cb, gpointer d, GError **err)
{
    sftp_file r = sftp_open(c->sftp, remoto, O_RDONLY, 0);
    if (!r) return fallo_sftp(c, err, "No se pudo abrir", remoto);
    FILE *f = g_fopen(local, "wb");
    if (!f) { sftp_close(r); return fallo_local(err, "No se pudo crear", local, errno); }

    char *nombre = g_path_get_basename(remoto);
    char *buf = g_malloc(BLOQUE);
    guint64 hecho = 0;
    gboolean ok = avisar(cb, d, nombre, 0, total, err);
    ssize_t n = 0;
    while (ok && (n = sftp_read(r, buf, BLOQUE)) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) ok = fallo_local(err, "Error escribiendo", local, errno);
        hecho += (guint64)n;
        if (ok) ok = avisar(cb, d, nombre, hecho, total, err);
    }
    if (ok && n < 0) ok = fallo_sftp(c, err, "Error leyendo", remoto);
    sftp_close(r);
    if (fclose(f) != 0 && ok) ok = fallo_local(err, "Error cerrando", local, errno);
    if (ok) g_chmod(local, modo & 0777);
    g_free(buf);
    g_free(nombre);
    return ok;
}

static gboolean bajar_rec(Conexion *c, const char *remoto, const char *local,
                          ConProgreso cb, gpointer d, GError **err)
{
    sftp_attributes a = sftp_stat(c->sftp, remoto);
    if (!a) return fallo_sftp(c, err, "No se pudo leer", remoto);
    gboolean dir = a->type == SSH_FILEXFER_TYPE_DIRECTORY;
    guint32 modo = a->permissions;
    guint64 tam = a->size;
    sftp_attributes_free(a);
    if (!dir) return bajar_archivo(c, remoto, local, modo, tam, cb, d, err);

    if (g_mkdir_with_parents(local, 0755) != 0) return fallo_local(err, "No se pudo crear", local, errno);
    GPtrArray *hijos = hijos_remotos(c, remoto, TRUE, err);
    if (!hijos) return FALSE;
    gboolean ok = TRUE;
    for (guint i = 0; ok && i < hijos->len; i++) {
        const char *hr = g_ptr_array_index(hijos, i);
        char *base = g_path_get_basename(hr);
        char *hl = g_build_filename(local, base, NULL);
        ok = bajar_rec(c, hr, hl, cb, d, err);
        g_free(hl);
        g_free(base);
    }
    g_ptr_array_unref(hijos);
    return ok;
}

gboolean con_bajar(Conexion *c, const char *remoto, const char *dir_local,
                   ConProgreso cb, gpointer data, GError **err)
{
    char *base = g_path_get_basename(remoto);
    char *local = g_build_filename(dir_local, base, NULL);
    g_mutex_lock(&c->mutex);
    gboolean ok = bajar_rec(c, remoto, local, cb, data, err);
    g_mutex_unlock(&c->mutex);
    g_free(local);
    g_free(base);
    return ok;
}

/* ---------- servidor de salto (bastión) ---------- */

void con_usar_salto(Conexion *c, Conexion *salto) { c->salto = salto; }
Conexion *con_salto(Conexion *c) { return c->salto; }

static gboolean escribir_todo(socket_t fd, const char *b, ssize_t n)
{
    while (n > 0) {
        ssize_t w = sock_escribir(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return FALSE;
        b += w;
        n -= w;
    }
    return TRUE;
}

/* Hilo "bomba": pasa los bytes entre el canal del salto y el socketpair que usa
 * la conexión de destino como si fuera su socket TCP. Solo este hilo toca la
 * sesión del salto mientras el túnel está abierto. */
static gpointer bomba(gpointer d)
{
    Conexion *j = d;
    char buf[32768];
    socket_t sfd = ssh_get_fd(j->ssh);
    while (!g_atomic_int_get(&j->parar)) {
        /* del salto hacia el destino: todo lo que libssh ya tenga */
        int n;
        while ((n = ssh_channel_read_nonblocking(j->canal, buf, sizeof buf, 0)) > 0)
            if (!escribir_todo(j->par[0], buf, n)) goto fin;
        if (n == SSH_ERROR || ssh_channel_is_eof(j->canal) || !ssh_channel_is_open(j->canal)) goto fin;
        struct pollfd pf[2] = { { sfd, POLLIN, 0 }, { j->par[0], POLLIN, 0 } };
        if (sondear(pf, 2, 200) < 0) {
            if (errno == EINTR) continue;
            goto fin;
        }
        /* del destino hacia el salto */
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = sock_leer(j->par[0], buf, sizeof buf);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) goto fin;   /* el destino cerró */
            if (ssh_channel_write(j->canal, buf, (uint32_t)r) != r) goto fin;
        }
        if (pf[0].revents & (POLLHUP | POLLERR)) goto fin;
    }
fin:
    /* el destino ve "conexión cerrada" y lo trata como conexión perdida */
    shutdown(j->par[0], SHUT_RDWR);
    return NULL;
}

gboolean con_tunel(Conexion *c, GError **err)
{
    Conexion *j = c->salto;
    char *u, *h, *p;
    con_datos(c, &u, &h, &p);
    gboolean ok = FALSE;
    g_mutex_lock(&j->mutex);
    j->canal = ssh_channel_new(j->ssh);
    /* el nombre del destino lo resuelve el salto: sirve para nombres de su red interna */
    if (!j->canal || ssh_channel_open_forward(j->canal, h, atoi(p), "127.0.0.1", 22) != SSH_OK) {
        g_set_error(err, CON_ERROR, CON_ERR_RED, "no pudo abrir el túnel hacia %s:%s: %s",
                    h, p, ssh_get_error(j->ssh));
        if (j->canal) { ssh_channel_free(j->canal); j->canal = NULL; }
        goto fin;
    }
    if (par_sockets(j->par) != 0) {
        g_set_error(err, CON_ERROR, CON_ERR_LOCAL, "No se pudo crear el túnel local: %s", g_strerror(errno));
        goto fin;
    }
    int tam = 1 << 20;   /* buffers grandes: evita que los dos lados se esperen */
    for (int i = 0; i < 2; i++) {
        setsockopt(j->par[i], SOL_SOCKET, SO_SNDBUF, OPT(&tam), sizeof tam);
        setsockopt(j->par[i], SOL_SOCKET, SO_RCVBUF, OPT(&tam), sizeof tam);
    }
    g_mutex_lock(&c->mutex);
    socket_t fd = j->par[1];
    ssh_options_set(c->ssh, SSH_OPTIONS_FD, &fd);   /* libssh lo cierra al desconectar */
    g_mutex_unlock(&c->mutex);
    j->bomba = g_thread_new("salto", bomba, j);
    ok = TRUE;
fin:
    g_mutex_unlock(&j->mutex);
    g_free(u); g_free(h); g_free(p);
    return ok;
}

gboolean con_conectada(Conexion *c)
{
    if (!c) return FALSE;
    g_mutex_lock(&c->mutex);
    gboolean r = c->sftp && ssh_is_connected(c->ssh);
    g_mutex_unlock(&c->mutex);
    return r;
}

gboolean con_vivo(Conexion *c, GError **err)
{
    g_mutex_lock(&c->mutex);
    gboolean ok = c->sftp && ssh_is_connected(c->ssh);
    if (ok) {
        /* una consulta mínima al servidor: si la red cayó, falla aquí */
        sftp_attributes at = sftp_stat(c->sftp, ".");
        if (at) sftp_attributes_free(at);
        else ok = ssh_is_connected(c->ssh) && sftp_get_error(c->sftp) != SSH_FX_CONNECTION_LOST
                  && sftp_get_error(c->sftp) != SSH_FX_NO_CONNECTION;
    }
    if (!ok) g_set_error(err, CON_ERROR, CON_ERR_RED, "Se perdió la conexión: %s", ssh_get_error(c->ssh));
    g_mutex_unlock(&c->mutex);
    return ok;
}

void con_cortar(Conexion *c)
{
    g_mutex_lock(&c->mutex);
    socket_t fd = ssh_get_fd(c->ssh);
    if (fd != SSH_INVALID_SOCKET) shutdown(fd, SHUT_RDWR);
    g_mutex_unlock(&c->mutex);
}

void con_cerrar(Conexion *c)
{
    if (!c) return;
    g_mutex_lock(&c->mutex);
    if (c->sftp) sftp_free(c->sftp);
    if (ssh_is_connected(c->ssh)) ssh_disconnect(c->ssh);
    ssh_free(c->ssh);   /* cierra también el extremo [1] del túnel, si lo hay */
    g_mutex_unlock(&c->mutex);
    /* túnel por un salto: parar la bomba y cerrar el salto */
    if (c->salto) {
        Conexion *j = c->salto;
        g_atomic_int_set(&j->parar, 1);
        if (j->bomba) g_thread_join(j->bomba);
        if (j->canal) {
            ssh_channel_close(j->canal);
            ssh_channel_free(j->canal);
        }
        if (j->par[0] != SSH_INVALID_SOCKET) sock_cerrar(j->par[0]);
        if (!j->bomba && j->par[1] != SSH_INVALID_SOCKET) sock_cerrar(j->par[1]);   /* el destino nunca lo usó */
        con_cerrar(j);
    }
    g_mutex_clear(&c->mutex);
    g_free(c->identidad);
    g_free(c);
}

/* ---------- lado local ---------- */

GPtrArray *local_listar(const char *ruta, GError **err)
{
    GDir *dir = g_dir_open(ruta, 0, err);
    if (!dir) return NULL;
    GPtrArray *res = g_ptr_array_new_with_free_func(entrada_free);
    const char *n;
    while ((n = g_dir_read_name(dir)) != NULL) {
        char *full = g_build_filename(ruta, n, NULL);
        GStatBuf st;
        if (g_stat(full, &st) == 0 || g_lstat(full, &st) == 0) {
            Entrada *e = g_new0(Entrada, 1);
            e->nombre = g_strdup(n);
            e->es_dir = S_ISDIR(st.st_mode);
            e->tam = st.st_size;
            e->mtime = st.st_mtime;
            e->permisos = st.st_mode;
            g_ptr_array_add(res, e);
        }
        g_free(full);
    }
    g_dir_close(dir);
    return res;
}

gboolean local_mkdir(const char *ruta, GError **err)
{
    return g_mkdir(ruta, 0755) == 0 || fallo_local(err, "No se pudo crear", ruta, errno);
}

gboolean local_crear_archivo(const char *ruta, GError **err)
{
    int fd = g_open(ruta, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return fallo_local(err, "No se pudo crear", ruta, errno);
    close(fd);
    return TRUE;
}

gboolean local_renombrar(const char *de, const char *a, GError **err)
{
    return g_rename(de, a) == 0 || fallo_local(err, "No se pudo renombrar", de, errno);
}

gboolean local_borrar(const char *ruta, GError **err)
{
    GStatBuf st;
    if (g_lstat(ruta, &st) != 0) return fallo_local(err, "No se pudo leer", ruta, errno);
    if (!S_ISDIR(st.st_mode))
        return g_remove(ruta) == 0 || fallo_local(err, "No se pudo borrar", ruta, errno);
    GDir *dir = g_dir_open(ruta, 0, err);
    if (!dir) return FALSE;
    gboolean ok = TRUE;
    const char *n;
    while (ok && (n = g_dir_read_name(dir)) != NULL) {
        char *full = g_build_filename(ruta, n, NULL);
        ok = local_borrar(full, err);
        g_free(full);
    }
    g_dir_close(dir);
    return ok && (g_rmdir(ruta) == 0 || fallo_local(err, "No se pudo borrar la carpeta", ruta, errno));
}
