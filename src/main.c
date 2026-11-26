/* sftp-commander: cliente SFTP de dos paneles (local | remoto) con GTK4 + libssh.
 * Cada pestaña es una Sesion con su propia conexión, paneles y barra de estado.
 * La interfaz corre en el hilo principal; cada operación de red va en un hilo
 * de trabajo (GTask) y vuelve a op_terminada() al acabar. Una operación a la vez
 * por pestaña. */
#include <gtk/gtk.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <gio/gdesktopappinfo.h>
#endif
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sftp.h"
#include "llavero.h"

#define VERSION "0.8.0"
#define WEB_PROYECTO "https://github.com/reybendg/sftp-comander"

/* ---------- elemento de la lista ---------- */

#define FI_TYPE_ITEM (fi_item_get_type())
G_DECLARE_FINAL_TYPE(FiItem, fi_item, FI, ITEM, GObject)
struct _FiItem {
    GObject  parent_instance;
    char    *nombre;
    gboolean es_dir;
    guint64  tam;
    gint64   mtime;
    guint32  permisos;
};
G_DEFINE_TYPE(FiItem, fi_item, G_TYPE_OBJECT)

static void fi_item_finalize(GObject *o)
{
    g_free(FI_ITEM(o)->nombre);
    G_OBJECT_CLASS(fi_item_parent_class)->finalize(o);
}
static void fi_item_class_init(FiItemClass *k) { G_OBJECT_CLASS(k)->finalize = fi_item_finalize; }
static void fi_item_init(FiItem *i) { (void)i; }

static FiItem *fi_item_new(const char *nombre, gboolean es_dir, guint64 tam, gint64 mtime, guint32 perm)
{
    FiItem *i = g_object_new(FI_TYPE_ITEM, NULL);
    i->nombre = g_strdup(nombre);
    i->es_dir = es_dir;
    i->tam = tam;
    i->mtime = mtime;
    i->permisos = perm;
    return i;
}

/* ---------- estado de la aplicación ---------- */

typedef struct App App;
typedef struct Sesion Sesion;
typedef struct _Op Op;
typedef struct DlgPregunta DlgPregunta;

typedef struct {
    Sesion            *s;
    gboolean           remoto;
    char              *cwd;
    GListStore        *store;      /* todo el contenido de la carpeta */
    GtkCustomFilter   *filtro;     /* lo que se ve: filtrado por la búsqueda */
    char              *patron;     /* texto buscado, en minúsculas (casefold) */
    GtkMultiSelection *sel;        /* sobre el modelo filtrado: sus posiciones son las visibles */
    GtkSortListModel  *orden;      /* store ordenado según la columna elegida (entre store y filtro) */
    GtkWidget         *marco, *titulo, *ruta, *vista, *menu, *buscar, *lupa;
    GtkBitset         *sel_previa; /* selección al presionar el mouse: arrastrar varios sin perderla */
    GString           *teclas;     /* letras tecleadas seguidas para saltar a un nombre */
    gint64             teclas_t;   /* cuándo fue la última (µs) */
} Panel;

/* Una pestaña: conexión, paneles, estado y aviso "Conectando…" propios */
struct Sesion {
    int        refs;
    gboolean   cerrada;        /* pestaña cerrada: los hilos que vuelvan solo limpian */
    App       *app;
    Panel      izq, der, *activo;
    Conexion  *con;
    char      *desc;
    gboolean   ocupado;
    gint       cancelar;       /* atómico: lo lee el hilo de trabajo */
    gint64     ultimo_prog;    /* solo lo usa el hilo de trabajo */
    Op        *op_con;         /* conectar/autenticar en curso */
    /* datos para conectar y reconectar esta pestaña */
    char      *c_servidor, *c_puerto, *c_usuario, *c_llave, *c_carpeta, *c_nombre;
    char      *c_salto, *c_llave_salto;   /* servidor de salto (usuario@host:puerto o alias) */
    char      *clave_pend;     /* contraseña escrita para el próximo intento */
    gboolean   recordar;       /* guardarla en el llavero si sirve */
    char      *clave_salto_pend;          /* lo mismo, para el servidor de salto */
    gboolean   recordar_salto, preg_salto;
    char      *desc_salto;     /* usuario@salto, para mostrar "destino vía salto" */
    char      *volver_a;       /* carpeta remota a recuperar al reconectar */
    char      *perdida;        /* descripción de la conexión que se perdió */
    DlgPregunta *dlg_pregunta; /* ventana con preguntas del servidor (OTP, enlace) abierta */
    gboolean   perdida_pendiente, dialogo_abierto;
    /* widgets */
    GtkWidget *pagina, *etiqueta, *estado, *barra, *btn_cancelar;
    GtkWidget *aviso, *aviso_spinner, *aviso_detalle, *aviso_tiempo;
    gint64     aviso_desde;
    guint      aviso_reloj;
};

struct App {
    GtkApplication *gapp;
    GtkWidget      *win, *tabs, *servidor, *puerto, *usuario, *clave, *btn_con, *dd_srv;
    GtkStringList  *nombres_srv;    /* índice 0 = texto fijo "Servidores" */
    GKeyFile       *servidores;
    char           *sel_llave, *sel_carpeta;   /* del servidor guardado elegido en la barra */
    char           *srv_nombre;                /* nombre del servidor guardado elegido */
    char           *sel_salto, *sel_llave_salto;   /* su servidor de salto, si tiene */
    gboolean        llenando, ocultos, captura_hecha, demo_hecho;
    GPtrArray      *sesiones;                  /* Sesion*, en orden de creación */
    Sesion         *s;                         /* la pestaña visible */
    GPtrArray      *ediciones;                 /* archivos remotos abiertos para editar */
    GMenuModel     *menu_ctx;
    Panel          *panel_dialogo;
    gboolean        confirmar_copia;           /* preguntar antes de copiar (preferencias.ini) */
    Panel          *arrastre;                  /* panel desde el que se arrastra, mientras dura */
    /* opciones de línea de comandos */
    char           *op_destino, *op_identidad, *op_known, *op_captura, *op_salto;
    GtkWidget      *captura_widget, *gestor_win;   /* solo para las capturas de prueba */
};

typedef enum {
    OP_CONECTAR, OP_AUTENTICAR, OP_LISTAR, OP_MKDIR, OP_NUEVO_DOC, OP_BORRAR, OP_RENOMBRAR, OP_COPIAR,
    OP_ABRIR, OP_GUARDAR_EDITADO, OP_VERIFICAR
} TipoOp;

struct _Op {
    TipoOp      tipo;
    Sesion     *s;
    Panel      *p;
    gboolean    remoto, guardar_host, recordar;
    gboolean    salto;          /* conectar/autenticar: trabaja sobre el servidor de salto */
    gboolean    abandonada;     /* conexión cancelada: el resultado se descarta */
    Conexion   *con;            /* la conexión de la pestaña al lanzar la operación */
    char       *ruta, *ruta2, *clave, *nombre_srv;
    GPtrArray  *nombres;
    /* resultados que deja el hilo */
    EstadoHost  est;
    gboolean    de_llavero;     /* la clave usada vino del llavero */
    char       *huella, *texto, *desc, *aviso;
    GPtrArray  *lista;
    GError     *err;
};

/* Archivo remoto bajado a una carpeta temporal y abierto con un programa local.
 * Cada vez que se guarda, se sube de nuevo al servidor por la pestaña conectada a él. */
typedef struct {
    App          *a;
    char         *local, *remoto, *nombre, *desc;
    GFileMonitor *monitor;
    guint         espera;
} Edicion;

static void panel_ir(Panel *p, const char *ruta);
static void desconectar(Sesion *s);
static void sesion_conectar(Sesion *s);
static void conexion_perdida(Sesion *s);
static void dialogo_perdida(Sesion *s);
static gboolean capturar(gpointer d);
static void programar_captura(App *a, guint ms);
static void gestor_abrir(App *a);
static Sesion *nueva_sesion(App *a);
static void cerrar_sesion(Sesion *s);
static void boton_con_actualizar(App *a);
static void sesion_titulo(Sesion *s);
static void pregunta_cerrar(DlgPregunta *d);
static gboolean preguntas_destino(const char *nombre, const char *instr, int n, const char * const *prompts,
                                  const gboolean *eco, char **resp, gpointer data);
static gboolean preguntas_salto(const char *nombre, const char *instr, int n, const char * const *prompts,
                                const gboolean *eco, char **resp, gpointer data);

/* Referencias atómicas: los hilos de trabajo también las toman (progreso, pasos) */
static Sesion *sesion_ref(Sesion *s) { g_atomic_int_inc(&s->refs); return s; }

static void sesion_unref(Sesion *s)
{
    if (!g_atomic_int_dec_and_test(&s->refs)) return;
    if (s->con) con_cerrar(s->con);
    for (Panel *p = &s->izq; p; p = p == &s->izq ? &s->der : NULL) {
        g_free(p->cwd);
        g_free(p->patron);
        g_clear_pointer(&p->sel_previa, gtk_bitset_unref);
        if (p->teclas) g_string_free(p->teclas, TRUE);
        g_clear_object(&p->store);
        g_clear_object(&p->sel);
    }
    g_free(s->desc);
    g_free(s->c_servidor); g_free(s->c_puerto); g_free(s->c_usuario);
    g_free(s->c_llave); g_free(s->c_carpeta); g_free(s->c_nombre);
    g_free(s->c_salto); g_free(s->c_llave_salto); g_free(s->desc_salto);
    if (s->clave_pend) { memset(s->clave_pend, 0, strlen(s->clave_pend)); g_free(s->clave_pend); }
    if (s->clave_salto_pend) { memset(s->clave_salto_pend, 0, strlen(s->clave_salto_pend)); g_free(s->clave_salto_pend); }
    g_free(s->volver_a);
    g_free(s->perdida);
    g_free(s);
}

static void estado(Sesion *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void estado(Sesion *s, const char *fmt, ...)
{
    if (s->cerrada) return;
    va_list ap;
    va_start(ap, fmt);
    char *t = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(s->estado), t);
    g_free(t);
}

static const char *texto_de(GtkWidget *w) { return gtk_editable_get_text(GTK_EDITABLE(w)); }

static void margenes(GtkWidget *w, int m)
{
    gtk_widget_set_margin_top(w, m);
    gtk_widget_set_margin_bottom(w, m);
    gtk_widget_set_margin_start(w, m);
    gtk_widget_set_margin_end(w, m);
}

/* "~/algo" -> "/home/usuario/algo" */
static char *expandir_home(const char *ruta)
{
    if (!ruta || !*ruta) return NULL;
    if (g_str_has_prefix(ruta, "~/")) return g_build_filename(g_get_home_dir(), ruta + 2, NULL);
    return g_strdup(ruta);
}

static char *dup_o_null(const char *t) { return (t && *t) ? g_strdup(t) : NULL; }

static Panel *otro_panel(Sesion *s, Panel *p) { return p == &s->izq ? &s->der : &s->izq; }

/* ---------- operaciones en segundo plano ---------- */

static Op *op_nueva(Sesion *s, TipoOp t)
{
    Op *o = g_new0(Op, 1);
    o->s = sesion_ref(s);
    o->tipo = t;
    return o;
}

static void op_free(Op *o)
{
    g_free(o->ruta);
    g_free(o->ruta2);
    g_free(o->huella);
    g_free(o->texto);
    g_free(o->desc);
    g_free(o->aviso);
    g_free(o->nombre_srv);
    if (o->clave) { memset(o->clave, 0, strlen(o->clave)); g_free(o->clave); }
    if (o->nombres) g_ptr_array_unref(o->nombres);
    if (o->lista) g_ptr_array_unref(o->lista);
    g_clear_error(&o->err);
    sesion_unref(o->s);
    g_free(o);
}

typedef struct { Sesion *s; double frac; char *texto; } Prog;

static gboolean prog_idle(gpointer d)
{
    Prog *g = d;
    if (!g->s->cerrada && g->s->ocupado) {   /* si ya terminó, no pisar el mensaje final */
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g->s->barra), g->frac);
        gtk_label_set_text(GTK_LABEL(g->s->estado), g->texto);
    }
    sesion_unref(g->s);
    g_free(g->texto);
    g_free(g);
    return G_SOURCE_REMOVE;
}

/* Se llama desde el hilo de trabajo: solo encola la actualización de la interfaz.
 * La Op tiene una referencia a la sesión, así que s sigue viva mientras tanto. */
static gboolean progreso(const char *archivo, guint64 hecho, guint64 total, gpointer d)
{
    Sesion *s = d;
    gint64 ahora = g_get_monotonic_time();
    if (hecho == total || ahora - s->ultimo_prog > 100000) {
        s->ultimo_prog = ahora;
        Prog *g = g_new0(Prog, 1);
        g->s = sesion_ref(s);
        g->frac = total ? (double)hecho / (double)total : 1.0;
        char *h = g_format_size(hecho), *t = g_format_size(total);
        g->texto = g_strdup_printf("%s   %s de %s", archivo, h, t);
        g_free(h);
        g_free(t);
        g_idle_add(prog_idle, g);
    }
    return !g_atomic_int_get(&s->cancelar);
}

/* Texto del paso actual en el aviso "Conectando…" (se llama desde el hilo) */
typedef struct { Sesion *s; Op *op; char *texto; } Paso;

static gboolean paso_idle(gpointer d)
{
    Paso *p = d;
    if (!p->s->cerrada && p->s->op_con == p->op) gtk_label_set_text(GTK_LABEL(p->s->aviso_detalle), p->texto);
    sesion_unref(p->s);
    g_free(p->texto);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void paso(Op *op, const char *texto)
{
    Paso *p = g_new0(Paso, 1);
    p->s = sesion_ref(op->s);   /* la suelta paso_idle */
    p->op = op;
    p->texto = g_strdup(texto);
    g_idle_add(paso_idle, p);
}

static void op_hilo(GTask *t, gpointer src, gpointer datos, GCancellable *cc)
{
    (void)src; (void)cc;
    Op *op = datos;
    Sesion *s = op->s;
    Conexion *c = op->con;
    switch (op->tipo) {
    case OP_CONECTAR:
        con_conectar(op->salto ? con_salto(c) : c, &op->est, &op->huella, &op->err);
        break;
    case OP_AUTENTICAR: {
        Conexion *destino = c;
        if (op->salto) c = con_salto(destino);   /* primero se entra al servidor de salto */
        if (op->guardar_host && !con_guardar_host(c, &op->err)) break;
        char *u, *h, *pt;
        con_datos(c, &u, &h, &pt);
        op->desc = con_descripcion(c);
        /* sin contraseña escrita: probar la del llavero, si hay */
        if (!op->clave || !*op->clave) {
            paso(op, "Buscando la contraseña en el llavero…");
            char *k = llavero_buscar(h, pt, u, NULL);
            if (k) {
                g_free(op->clave);
                op->clave = g_strdup(k);
                op->de_llavero = TRUE;
                llavero_liberar(k);
            }
        }
        char *txt = op->salto ? g_strdup_printf("Iniciando sesión en el servidor de salto como %s…", u)
                              : g_strdup_printf("Iniciando sesión como %s…", u);
        paso(op, txt);
        g_free(txt);
        gboolean ok = op->salto ? con_autenticar_salto(c, op->clave, &op->err)
                                : con_autenticar(c, op->clave, &op->err);
        if (ok && op->recordar && op->clave && *op->clave && !op->de_llavero) {
            GError *e = NULL;
            if (llavero_guardar(op->nombre_srv, h, pt, u, op->clave, &e))
                op->aviso = g_strdup("contraseña guardada en el llavero");
            else {
                op->aviso = g_strdup_printf("no se pudo guardar la contraseña en el llavero: %s", e->message);
                g_error_free(e);
            }
        }
        g_free(u); g_free(h); g_free(pt);
        if (!ok) break;
        if (op->salto) {   /* dentro del salto: abrir el túnel; el destino se conecta después */
            paso(op, "Abriendo el túnel hacia el servidor de destino…");
            con_tunel(destino, &op->err);
            break;
        }
        paso(op, "Abriendo la carpeta remota…");
        /* carpeta a recuperar (reconexión) o inicial del servidor guardado; si no existe, la de inicio */
        if (op->ruta) {
            op->lista = con_listar(c, op->ruta, NULL);
            if (op->lista) { op->texto = g_strdup(op->ruta); break; }
        }
        op->texto = con_ruta_inicial(c, &op->err);
        if (op->texto) op->lista = con_listar(c, op->texto, &op->err);
        break;
    }
    case OP_VERIFICAR:
        con_vivo(c, &op->err);
        break;
    case OP_ABRIR:            /* ruta: archivo remoto; ruta2: carpeta temporal local */
        con_bajar(c, op->ruta, op->ruta2, progreso, s, &op->err);
        break;
    case OP_GUARDAR_EDITADO:  /* ruta: copia local; ruta2: carpeta remota */
        con_subir(c, op->ruta, op->ruta2, NULL, NULL, &op->err);
        break;
    case OP_LISTAR:
        op->lista = con_listar(c, op->ruta, &op->err);
        break;
    case OP_MKDIR:
        if (op->remoto) con_mkdir(c, op->ruta, &op->err);
        else local_mkdir(op->ruta, &op->err);
        break;
    case OP_NUEVO_DOC:
        if (op->remoto) con_crear_archivo(c, op->ruta, &op->err);
        else local_crear_archivo(op->ruta, &op->err);
        break;
    case OP_RENOMBRAR:
        if (op->remoto) con_renombrar(c, op->ruta, op->ruta2, &op->err);
        else local_renombrar(op->ruta, op->ruta2, &op->err);
        break;
    case OP_BORRAR:
        for (guint i = 0; i < op->nombres->len; i++) {
            char *r = ruta_unir(op->ruta, g_ptr_array_index(op->nombres, i));
            gboolean ok = op->remoto ? con_borrar(c, r, &op->err) : local_borrar(r, &op->err);
            g_free(r);
            if (!ok) break;
        }
        break;
    case OP_COPIAR:   /* remoto = el origen es el panel remoto; sin ruta, los nombres son rutas completas */
        for (guint i = 0; i < op->nombres->len; i++) {
            const char *n = g_ptr_array_index(op->nombres, i);
            char *r = op->ruta ? ruta_unir(op->ruta, n) : g_strdup(n);
            gboolean ok = op->remoto ? con_bajar(c, r, op->ruta2, progreso, s, &op->err)
                                     : con_subir(c, r, op->ruta2, progreso, s, &op->err);
            g_free(r);
            if (!ok) break;
        }
        break;
    }
    g_task_return_boolean(t, TRUE);
}

static void op_terminada(GObject *src, GAsyncResult *res, gpointer datos);

static gboolean es_remota(Op *op)
{
    return op->remoto || op->tipo == OP_COPIAR || op->tipo == OP_ABRIR ||
           op->tipo == OP_GUARDAR_EDITADO || op->tipo == OP_VERIFICAR;
}

static void lanzar(Op *op)
{
    Sesion *s = op->s;
    if (s->cerrada) { op_free(op); return; }
    if (s->ocupado) {
        estado(s, "Espera a que termine la operación en curso");
        op_free(op);
        return;
    }
    /* sin conexión no se toca el panel remoto */
    if (op->tipo != OP_CONECTAR && op->tipo != OP_AUTENTICAR && !s->con && es_remota(op)) {
        estado(s, "Conéctate a un servidor primero");
        op_free(op);
        return;
    }
    s->ocupado = TRUE;
    g_atomic_int_set(&s->cancelar, 0);
    op->con = s->con;
    if (op->tipo == OP_CONECTAR || op->tipo == OP_AUTENTICAR) s->op_con = op;
    if (op->tipo == OP_COPIAR || op->tipo == OP_ABRIR) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->barra), 0);
        gtk_widget_set_visible(s->btn_cancelar, TRUE);
    }
    GTask *t = g_task_new(NULL, NULL, op_terminada, op);
    g_task_set_task_data(t, op, NULL);
    g_task_run_in_thread(t, op_hilo);
    g_object_unref(t);
}

/* ---------- paneles ---------- */

static int comparar_entradas(gconstpointer pa, gconstpointer pb)
{
    const Entrada *a = *(Entrada * const *)pa, *b = *(Entrada * const *)pb;
    if (a->es_dir != b->es_dir) return a->es_dir ? -1 : 1;
    return g_utf8_collate(a->nombre, b->nombre);
}

/* Filtro de la búsqueda: el nombre contiene el texto (sin distinguir mayúsculas),
 * o cumple el patrón si lleva * o ?. ".." siempre queda. */
static gboolean filtro_fn(gpointer item, gpointer d)
{
    Panel *p = d;
    FiItem *it = item;
    if (!p->patron || !*p->patron || strcmp(it->nombre, "..") == 0) return TRUE;
    char *n = g_utf8_casefold(it->nombre, -1);
    gboolean r = strpbrk(p->patron, "*?") ? g_pattern_match_simple(p->patron, n)
                                          : strstr(n, p->patron) != NULL;
    g_free(n);
    return r;
}

/* Cantidad de elementos visibles, sin contar ".." */
static guint panel_visibles(Panel *p)
{
    GListModel *m = G_LIST_MODEL(p->sel);
    guint n = g_list_model_get_n_items(m);
    if (n > 0) {
        FiItem *it = g_list_model_get_item(m, 0);
        if (strcmp(it->nombre, "..") == 0) n--;
        g_object_unref(it);
    }
    return n;
}

static void buscar_limpiar(Panel *p)
{
    g_clear_pointer(&p->patron, g_free);
    gtk_filter_changed(GTK_FILTER(p->filtro), GTK_FILTER_CHANGE_LESS_STRICT);
    if (*texto_de(p->buscar)) gtk_editable_set_text(GTK_EDITABLE(p->buscar), "");
    gtk_widget_set_visible(p->buscar, FALSE);
}

/* "/" en Linux y en remoto; "C:\" o "\\servidor\recurso\" en Windows */
static gboolean es_raiz(const char *ruta)
{
    if (strcmp(ruta, "/") == 0) return TRUE;
#ifdef _WIN32
    if (es_ruta_windows(ruta)) {
        const char *r = g_path_skip_root(ruta);
        return r && !*r;
    }
#endif
    return FALSE;
}

static void panel_cargar(Panel *p, const char *ruta, GPtrArray *lista)
{
    /* al cambiar de carpeta se quita la búsqueda */
    if (p->cwd && strcmp(p->cwd, ruta) != 0 && gtk_widget_get_visible(p->buscar)) buscar_limpiar(p);
    g_ptr_array_sort(lista, comparar_entradas);
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    if (!es_raiz(ruta)) g_ptr_array_add(items, fi_item_new("..", TRUE, 0, 0, 0));
    for (guint i = 0; i < lista->len; i++) {
        Entrada *e = g_ptr_array_index(lista, i);
        if (!p->s->app->ocultos && e->nombre[0] == '.') continue;
        g_ptr_array_add(items, fi_item_new(e->nombre, e->es_dir, e->tam, e->mtime, e->permisos));
    }
    g_list_store_splice(p->store, 0, g_list_model_get_n_items(G_LIST_MODEL(p->store)), items->pdata, items->len);
    g_ptr_array_unref(items);
    char *nueva = g_strdup(ruta);
    g_free(p->cwd);
    p->cwd = nueva;
    gtk_editable_set_text(GTK_EDITABLE(p->ruta), p->cwd);
}

static void panel_ir(Panel *p, const char *ruta)
{
    Sesion *s = p->s;
    if (!ruta || !*ruta) return;
    /* normaliza ".." y rutas relativas respecto de la carpeta actual */
    char *dest;
    if (p->remoto) {
        dest = ruta_remota_normalizar(ruta, p->cwd);
    } else {
        dest = g_canonicalize_filename(ruta, p->cwd);
#ifdef _WIN32
        /* "C:" a secas es "la carpeta actual de C:"; se quiere la raíz */
        if (g_ascii_isalpha(dest[0]) && dest[1] == ':' && !dest[2]) {
            char *r = g_strconcat(dest, "\\", NULL);
            g_free(dest);
            dest = r;
        }
#endif
    }
    if (!p->remoto) {
        GError *err = NULL;
        GPtrArray *l = local_listar(dest, &err);
        if (l) {
            panel_cargar(p, dest, l);
            g_ptr_array_unref(l);
        } else {
            estado(s, "%s", err->message);
            g_error_free(err);
            if (p->cwd) gtk_editable_set_text(GTK_EDITABLE(p->ruta), p->cwd);
        }
        g_free(dest);
        return;
    }
    if (!s->con) { estado(s, "No hay conexión"); g_free(dest); return; }
    Op *op = op_nueva(s, OP_LISTAR);
    op->p = p;
    op->remoto = TRUE;
    op->ruta = dest;
    lanzar(op);
}

static void panel_arriba(Panel *p)
{
    if (p->cwd && !es_raiz(p->cwd)) panel_ir(p, "..");
}

static void refrescar(Sesion *s)
{
    if (s->izq.cwd) panel_ir(&s->izq, s->izq.cwd);
    if (s->con && s->der.cwd) panel_ir(&s->der, s->der.cwd);
}

/* Nombres seleccionados (de lo visible), sin ".." */
static GPtrArray *panel_seleccion(Panel *p)
{
    GPtrArray *r = g_ptr_array_new_with_free_func(g_free);
    GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(p->sel));
    GtkBitsetIter it;
    guint pos;
    if (gtk_bitset_iter_init_first(&it, bs, &pos)) {
        do {
            FiItem *i = g_list_model_get_item(G_LIST_MODEL(p->sel), pos);
            if (i && strcmp(i->nombre, "..") != 0) g_ptr_array_add(r, g_strdup(i->nombre));
            if (i) g_object_unref(i);
        } while (gtk_bitset_iter_next(&it, &pos));
    }
    gtk_bitset_unref(bs);
    return r;
}

static void marcar_activo(Sesion *s)
{
    gtk_widget_remove_css_class(s->izq.marco, "activo");
    gtk_widget_remove_css_class(s->der.marco, "activo");
    gtk_widget_add_css_class(s->activo->marco, "activo");
}

/* ---------- columnas ---------- */

enum { COL_NOMBRE, COL_TAM, COL_FECHA, COL_PERM, N_COLS };

static void col_setup(GtkSignalListItemFactory *f, GObject *o, gpointer d)
{
    (void)f;
    GtkListItem *li = GTK_LIST_ITEM(o);
    int col = GPOINTER_TO_INT(d);
    GtkWidget *l = gtk_label_new(NULL), *hijo = l;
    gtk_label_set_xalign(GTK_LABEL(l), col == COL_TAM ? 1.0f : 0.0f);
    if (col == COL_NOMBRE) {
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        hijo = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(hijo), gtk_image_new());
        gtk_box_append(GTK_BOX(hijo), l);
    } else if (col == COL_PERM) {
        gtk_widget_add_css_class(l, "monospace");
    }
    /* para saber sobre qué fila se hizo clic derecho */
    g_object_set_data(G_OBJECT(hijo), "li", li);
    gtk_list_item_set_child(li, hijo);
}

static void col_bind(GtkSignalListItemFactory *f, GObject *o, gpointer d)
{
    (void)f;
    GtkListItem *li = GTK_LIST_ITEM(o);
    FiItem *it = gtk_list_item_get_item(li);
    GtkWidget *w = gtk_list_item_get_child(li);
    char *t = NULL;
    switch (GPOINTER_TO_INT(d)) {
    case COL_NOMBRE: {
        GtkWidget *img = gtk_widget_get_first_child(w);
        gtk_image_set_from_icon_name(GTK_IMAGE(img), it->es_dir ? "folder" : "text-x-generic");
        gtk_label_set_text(GTK_LABEL(gtk_widget_get_next_sibling(img)), it->nombre);
        return;
    }
    case COL_TAM:
        t = it->es_dir ? g_strdup("") : g_format_size(it->tam);
        break;
    case COL_FECHA:
        if (it->mtime > 0) {
            GDateTime *dt = g_date_time_new_from_unix_local(it->mtime);
            t = g_date_time_format(dt, "%Y-%m-%d %H:%M");
            g_date_time_unref(dt);
        }
        break;
    case COL_PERM:
        if (strcmp(it->nombre, "..") != 0) {
            const char *rwx = "rwxrwxrwx";
            t = g_malloc(11);
            t[0] = it->es_dir ? 'd' : '-';
            for (int i = 0; i < 9; i++) t[i + 1] = (it->permisos & (0400u >> i)) ? rwx[i] : '-';
            t[10] = '\0';
        }
        break;
    }
    gtk_label_set_text(GTK_LABEL(w), t ? t : "");
    g_free(t);
}

/* ---------- orden ---------- */

/* Siempre primero "..", después las carpetas y después los archivos (aunque el orden sea descendente) */
static int orden_carpetas(gconstpointer pa, gconstpointer pb, gpointer d)
{
    (void)d;
    const FiItem *a = pa, *b = pb;
    gboolean pa_ = strcmp(a->nombre, "..") == 0, pb_ = strcmp(b->nombre, "..") == 0;
    if (pa_ != pb_) return pa_ ? GTK_ORDERING_SMALLER : GTK_ORDERING_LARGER;
    if (a->es_dir != b->es_dir) return a->es_dir ? GTK_ORDERING_SMALLER : GTK_ORDERING_LARGER;
    return GTK_ORDERING_EQUAL;
}

static int cmp_num(guint64 x, guint64 y) { return x < y ? GTK_ORDERING_SMALLER : x > y ? GTK_ORDERING_LARGER : GTK_ORDERING_EQUAL; }

/* Orden de una columna (al hacer clic en su título; otro clic lo invierte) */
static int orden_columna(gconstpointer pa, gconstpointer pb, gpointer d)
{
    const FiItem *a = pa, *b = pb;
    int r = GTK_ORDERING_EQUAL;
    switch (GPOINTER_TO_INT(d)) {
    case COL_TAM:   r = cmp_num(a->tam, b->tam); break;
    case COL_FECHA: r = cmp_num((guint64)a->mtime, (guint64)b->mtime); break;
    case COL_PERM:  r = cmp_num(a->permisos & 07777, b->permisos & 07777); break;
    default: break;
    }
    if (r == GTK_ORDERING_EQUAL) {   /* a igual valor, por nombre */
        char *x = g_utf8_casefold(a->nombre, -1), *y = g_utf8_casefold(b->nombre, -1);
        int c = g_utf8_collate(x, y);
        g_free(x); g_free(y);
        r = c < 0 ? GTK_ORDERING_SMALLER : c > 0 ? GTK_ORDERING_LARGER : GTK_ORDERING_EQUAL;
    }
    return r;
}

/* ---------- diálogo para pedir un texto ---------- */

typedef void (*TextoCb)(App *a, const char *texto, gpointer data);
typedef struct {
    App *a; GtkWidget *win, *entry;
    TextoCb cb; gpointer data; GDestroyNotify destruir;
} Pedido;

static void pedido_free(gpointer p)
{
    Pedido *x = p;
    if (x->destruir) x->destruir(x->data);
    g_free(x);
}

static void pedido_ok(GtkWidget *w, gpointer d)
{
    (void)w;
    Pedido *p = d;
    const char *t = texto_de(p->entry);
    if (*t) p->cb(p->a, t, p->data);
    gtk_window_destroy(GTK_WINDOW(p->win));
}

static void pedido_cancelar(GtkWidget *w, gpointer d)
{
    (void)w;
    gtk_window_destroy(GTK_WINDOW(((Pedido *)d)->win));
}

static void cerrar_con_escape(GtkWidget *win)
{
    GtkEventController *esc = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(esc),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_named_action_new("window.close")));
    gtk_widget_add_controller(win, esc);
}

static void pedir_texto(App *a, const char *titulo, const char *inicial,
                        TextoCb cb, gpointer data, GDestroyNotify destruir)
{
    Pedido *p = g_new0(Pedido, 1);
    p->a = a; p->cb = cb; p->data = data; p->destruir = destruir;
    p->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(p->win), titulo);
    gtk_window_set_transient_for(GTK_WINDOW(p->win), GTK_WINDOW(a->win));
    gtk_window_set_modal(GTK_WINDOW(p->win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(p->win), 420, -1);
    g_object_set_data_full(G_OBJECT(p->win), "pedido", p, pedido_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    margenes(box, 14);
    p->entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(p->entry), inicial ? inicial : "");
    g_signal_connect(p->entry, "activate", G_CALLBACK(pedido_ok), p);
    GtkWidget *botones = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(botones, GTK_ALIGN_END);
    GtkWidget *cancelar = gtk_button_new_with_label("Cancelar");
    GtkWidget *aceptar = gtk_button_new_with_label("Aceptar");
    gtk_widget_add_css_class(aceptar, "suggested-action");
    g_signal_connect(cancelar, "clicked", G_CALLBACK(pedido_cancelar), p);
    g_signal_connect(aceptar, "clicked", G_CALLBACK(pedido_ok), p);
    gtk_box_append(GTK_BOX(botones), cancelar);
    gtk_box_append(GTK_BOX(botones), aceptar);
    gtk_box_append(GTK_BOX(box), p->entry);
    gtk_box_append(GTK_BOX(box), botones);
    gtk_window_set_child(GTK_WINDOW(p->win), box);
    cerrar_con_escape(p->win);
    gtk_window_present(GTK_WINDOW(p->win));

    /* seleccionar el nombre sin la extensión, como en los exploradores */
    const char *punto = inicial ? strrchr(inicial, '.') : NULL;
    int fin = (punto && punto != inicial) ? (int)g_utf8_pointer_to_offset(inicial, punto) : -1;
    gtk_editable_select_region(GTK_EDITABLE(p->entry), 0, fin);
}

/* ---------- abrir y editar archivos ---------- */

static char *tipo_de(const char *ruta)
{
    guchar buf[4096];
    gsize n = 0;
    FILE *f = g_fopen(ruta, "rb");
    if (f) { n = fread(buf, 1, sizeof buf, f); fclose(f); }
    return g_content_type_guess(ruta, n ? buf : NULL, n, NULL);
}

#ifdef _WIN32
/* Windows: el programa asociado a la extensión; si no hay (.log, .conf, .sh…), el Bloc de notas */
static GAppInfo *elegir_app(const char *ruta)
{
    char *ct = tipo_de(ruta);
    GAppInfo *app = g_app_info_get_default_for_type(ct, FALSE);
    g_free(ct);
    if (!app) app = g_app_info_create_from_commandline("notepad.exe", "Bloc de notas",
                                                       G_APP_INFO_CREATE_NONE, NULL);
    return app;
}
#else
/* Tiene la categoría y es gráfico (no vim/nano en una terminal) */
static gboolean tiene_categoria(GAppInfo *app, const char *cat)
{
    if (!G_IS_DESKTOP_APP_INFO(app)) return FALSE;
    if (g_desktop_app_info_get_boolean(G_DESKTOP_APP_INFO(app), "Terminal")) return FALSE;
    const char *c = g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(app));
    if (!c) return FALSE;
    char **v = g_strsplit(c, ";", -1);
    gboolean r = g_strv_contains((const char * const *)v, cat);
    g_strfreev(v);
    return r;
}

/* Programa para abrir el archivo: el predeterminado, salvo que para texto
 * prefiere un editor (no LibreOffice) y para imágenes un visor (no el navegador). */
static GAppInfo *elegir_app(const char *ruta)
{
    char *ct = tipo_de(ruta);
    const char *cat = g_content_type_is_a(ct, "text/plain") ? "TextEditor"
                    : g_str_has_prefix(ct, "image/") ? "Viewer" : NULL;
    GAppInfo *def = g_app_info_get_default_for_type(ct, FALSE), *res = NULL;
    if (cat && !(def && tiene_categoria(def, cat))) {
        GList *l = g_app_info_get_all_for_type(ct);
        for (GList *i = l; i && !res; i = i->next)
            if (tiene_categoria(i->data, cat)) res = g_object_ref(i->data);
        g_list_free_full(l, g_object_unref);
    }
    if (!res && def) res = g_object_ref(def);
    g_clear_object(&def);
    g_free(ct);
    return res;
}
#endif

static void abrir_con_app(Sesion *s, const char *ruta)
{
    char *base = g_path_get_basename(ruta);
    if (g_getenv("SFTPC_DEMO")) { g_free(base); return; }   /* pruebas: no lanzar programas */
    GAppInfo *app = elegir_app(ruta);
    if (!app) {
        estado(s, "No hay un programa para abrir %s", base);
    } else {
        GFile *f = g_file_new_for_path(ruta);
        GList *l = g_list_append(NULL, f);
        GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(s->app->win));
        GError *err = NULL;
        if (!g_app_info_launch(app, l, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
            estado(s, "No se pudo abrir %s: %s", base, err->message);
            g_error_free(err);
        }
        g_object_unref(ctx);
        g_list_free(l);
        g_object_unref(f);
        g_object_unref(app);
    }
    g_free(base);
}

static void edicion_free(gpointer p)
{
    Edicion *e = p;
    if (e->espera) g_source_remove(e->espera);
    if (e->monitor) { g_file_monitor_cancel(e->monitor); g_object_unref(e->monitor); }
    /* borra la copia temporal y su carpeta */
    char *dir = g_path_get_dirname(e->local);
    local_borrar(dir, NULL);
    g_free(dir);
    g_free(e->local); g_free(e->remoto); g_free(e->nombre); g_free(e->desc);
    g_free(e);
}

/* Pestaña abierta y conectada al servidor con esa descripción (usuario@host:puerto) */
static Sesion *sesion_por_desc(App *a, const char *desc)
{
    Sesion *r = NULL;
    for (guint i = 0; i < a->sesiones->len; i++) {
        Sesion *s = g_ptr_array_index(a->sesiones, i);
        if (s->con && g_strcmp0(s->desc, desc) == 0) {
            r = s;
            if (s == a->s) break;   /* preferir la visible */
        }
    }
    return r;
}

static gboolean subir_edicion(gpointer d)
{
    Edicion *e = d;
    App *a = e->a;
    e->espera = 0;
    Sesion *s = sesion_por_desc(a, e->desc);
    if (!s) {
        estado(a->s, "No se subió %s: ninguna pestaña está conectada a %s. Conéctate y vuelve a guardarlo.",
               e->nombre, e->desc);
        return G_SOURCE_REMOVE;
    }
    if (s->ocupado) {   /* hay otra operación: reintentar en un segundo */
        e->espera = g_timeout_add(1000, subir_edicion, e);
        return G_SOURCE_REMOVE;
    }
    Op *op = op_nueva(s, OP_GUARDAR_EDITADO);
    op->ruta = g_strdup(e->local);
    op->ruta2 = ruta_remota_normalizar("..", e->remoto);   /* carpeta remota del archivo */
    op->texto = g_strdup(e->nombre);
    estado(s, "Subiendo %s…", e->nombre);
    lanzar(op);
    return G_SOURCE_REMOVE;
}

static void on_edicion_cambio(GFileMonitor *m, GFile *f, GFile *otro, GFileMonitorEvent ev, gpointer d)
{
    (void)m;
    Edicion *e = d;
    GFile *cual = NULL;
    switch (ev) {
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
        cual = f; break;
    case G_FILE_MONITOR_EVENT_RENAMED:   /* editores que guardan en un temporal y renombran */
        cual = otro; break;
    default:
        return;
    }
    if (!cual) return;
    char *n = g_file_get_basename(cual);
    gboolean es = g_strcmp0(n, e->nombre) == 0;
    g_free(n);
    if (!es) return;
    /* espera a que el editor termine de escribir */
    if (e->espera) g_source_remove(e->espera);
    e->espera = g_timeout_add(700, subir_edicion, e);
}

static void edicion_nueva(Sesion *s, const char *local, const char *remoto)
{
    App *a = s->app;
    Edicion *e = g_new0(Edicion, 1);
    e->a = a;
    e->local = g_strdup(local);
    e->remoto = g_strdup(remoto);
    e->nombre = g_path_get_basename(local);
    e->desc = g_strdup(s->desc);
    char *dir = g_path_get_dirname(local);
    GFile *gd = g_file_new_for_path(dir);
    GError *err = NULL;
    e->monitor = g_file_monitor_directory(gd, G_FILE_MONITOR_WATCH_MOVES, NULL, &err);
    if (e->monitor) g_signal_connect(e->monitor, "changed", G_CALLBACK(on_edicion_cambio), e);
    else {
        estado(s, "No se podrán subir los cambios de %s: %s", e->nombre, err->message);
        g_error_free(err);
    }
    g_object_unref(gd);
    g_free(dir);
    g_ptr_array_add(a->ediciones, e);
}

static void abrir_item(Panel *p, FiItem *it)
{
    Sesion *s = p->s;
    char *ruta = ruta_unir(p->cwd, it->nombre);
    if (it->es_dir) {
        panel_ir(p, ruta);
    } else if (!p->remoto) {
        abrir_con_app(s, ruta);
    } else {
        /* baja a ~/.cache/sftp-commander/editar/XXXXXX/<nombre> y lo abre */
#ifdef _WIN32
        /* en Windows la "caché" de GLib es la de Internet Explorer: mejor %TEMP% */
        char *base = g_build_filename(g_get_tmp_dir(), "sftp-commander", "editar", NULL);
#else
        char *base = g_build_filename(g_get_user_cache_dir(), "sftp-commander", "editar", NULL);
#endif
        g_mkdir_with_parents(base, 0700);
        char *tmp = g_build_filename(base, "XXXXXX", NULL);
        if (!g_mkdtemp_full(tmp, 0700)) {
            estado(s, "No se pudo crear la carpeta temporal en %s", base);
            g_free(tmp);
        } else {
            Op *op = op_nueva(s, OP_ABRIR);
            op->ruta = g_strdup(ruta);
            op->ruta2 = tmp;
            estado(s, "Bajando %s para abrirlo…", it->nombre);
            lanzar(op);
        }
        g_free(base);
    }
    g_free(ruta);
}

/* Única fila seleccionada (sin contar ".."), o NULL */
static FiItem *panel_item_unico(Panel *p)
{
    FiItem *r = NULL;
    GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(p->sel));
    if (gtk_bitset_get_size(bs) == 1) {
        r = g_list_model_get_item(G_LIST_MODEL(p->sel), gtk_bitset_get_nth(bs, 0));
        if (r && strcmp(r->nombre, "..") == 0) g_clear_object(&r);
    }
    gtk_bitset_unref(bs);
    return r;
}

static void accion_abrir(App *a)
{
    Panel *p = a->s->activo;
    FiItem *it = panel_item_unico(p);
    if (!it) { estado(a->s, "Selecciona un solo archivo para abrirlo"); return; }
    abrir_item(p, it);
    g_object_unref(it);
}

/* ---------- copiar: confirmar y no pisar sin avisar ---------- */

static void preferencias_guardar(App *a);

/* ¿Hay algo con ese nombre en la carpeta del panel? (lo que se listó, sin filtro de búsqueda) */
static gboolean existe_en(Panel *p, const char *nombre)
{
    GListModel *m = G_LIST_MODEL(p->store);
    gboolean r = FALSE;
    for (guint i = 0, n = g_list_model_get_n_items(m); i < n && !r; i++) {
        FiItem *it = g_list_model_get_item(m, i);
#ifdef _WIN32
        /* en Windows los nombres locales no distinguen mayúsculas */
        r = !p->remoto ? g_ascii_strcasecmp(it->nombre, nombre) == 0 : strcmp(it->nombre, nombre) == 0;
#else
        r = strcmp(it->nombre, nombre) == 0;
#endif
        g_object_unref(it);
    }
    return r;
}

static char *nombres_texto(GPtrArray *v, guint max)
{
    GString *t = g_string_new(NULL);
    for (guint i = 0; i < v->len && i < max; i++) {
        char *b = g_path_get_basename(g_ptr_array_index(v, i));
        g_string_append_printf(t, "%s\n", b);
        g_free(b);
    }
    if (v->len > max) g_string_append_printf(t, "… y %u más\n", v->len - max);
    return g_string_free(t, FALSE);
}

static void copiar_lanzar(Op *op)
{
    estado(op->s, "%s %u elemento(s)…", op->remoto ? "Bajando" : "Subiendo", op->nombres->len);
    lanzar(op);
}

/* Ventana "¿Copiar?" con "No volver a preguntar" */
typedef struct { App *a; Op *op; GtkWidget *win, *check; } Confirmacion;

static void confirmacion_free(gpointer d)
{
    Confirmacion *c = d;
    if (c->op) op_free(c->op);   /* se cerró sin copiar */
    g_free(c);
}

static void confirmacion_ok(GtkWidget *w, gpointer d)
{
    (void)w;
    Confirmacion *c = d;
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(c->check))) {
        c->a->confirmar_copia = FALSE;
        preferencias_guardar(c->a);
        GAction *ac = g_action_map_lookup_action(G_ACTION_MAP(c->a->win), "confirmar-copia");
        if (ac) g_simple_action_set_state(G_SIMPLE_ACTION(ac), g_variant_new_boolean(FALSE));
    }
    Op *op = c->op;
    c->op = NULL;
    gtk_window_destroy(GTK_WINDOW(c->win));
    copiar_lanzar(op);
}

static void confirmacion_cancelar(GtkWidget *w, gpointer d)
{
    (void)w;
    gtk_window_destroy(GTK_WINDOW(((Confirmacion *)d)->win));
}

static void confirmar_copia(App *a, Op *op)
{
    Confirmacion *c = g_new0(Confirmacion, 1);
    c->a = a;
    c->op = op;
    c->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(c->win), op->remoto ? "Bajar" : "Subir");
    gtk_window_set_transient_for(GTK_WINDOW(c->win), GTK_WINDOW(a->win));
    gtk_window_set_modal(GTK_WINDOW(c->win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(c->win), FALSE);
    g_object_set_data_full(G_OBJECT(c->win), "confirmacion", c, confirmacion_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18);
    char *tit = g_strdup_printf(op->remoto ? "¿Bajar %u elemento(s) a este equipo?" : "¿Subir %u elemento(s) al servidor?",
                                op->nombres->len);
    GtkWidget *l1 = gtk_label_new(tit);
    gtk_widget_add_css_class(l1, "titulo-panel");
    gtk_label_set_xalign(GTK_LABEL(l1), 0);
    char *lista = nombres_texto(op->nombres, 8);
    char *det = g_strdup_printf("%sDestino: %s", lista, op->ruta2);
    GtkWidget *l2 = gtk_label_new(det);
    gtk_label_set_xalign(GTK_LABEL(l2), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l2), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(l2), 60);
    c->check = gtk_check_button_new_with_label("No volver a preguntar (se reactiva en el menú, arriba a la derecha)");
    GtkWidget *botones = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(botones, GTK_ALIGN_END);
    GtkWidget *cancelar = gtk_button_new_with_label("Cancelar");
    GtkWidget *ok = gtk_button_new_with_label(op->remoto ? "Bajar" : "Subir");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(cancelar, "clicked", G_CALLBACK(confirmacion_cancelar), c);
    g_signal_connect(ok, "clicked", G_CALLBACK(confirmacion_ok), c);
    gtk_box_append(GTK_BOX(botones), cancelar);
    gtk_box_append(GTK_BOX(botones), ok);
    gtk_box_append(GTK_BOX(box), l1);
    gtk_box_append(GTK_BOX(box), l2);
    gtk_box_append(GTK_BOX(box), c->check);
    gtk_box_append(GTK_BOX(box), botones);
    gtk_window_set_child(GTK_WINDOW(c->win), box);
    gtk_window_set_default_widget(GTK_WINDOW(c->win), ok);
    cerrar_con_escape(c->win);
    gtk_window_present(GTK_WINDOW(c->win));
    gtk_widget_grab_focus(ok);
    if (g_getenv("SFTPC_DEMO")) {
        g_print("CONFIRMAR: %s %u elemento(s) a %s\n", op->remoto ? "bajar" : "subir", op->nombres->len, op->ruta2);
        if (g_strcmp0(g_getenv("SFTPC_DEMO"), "confirmar") == 0) a->captura_widget = c->win;
    }
    g_free(tit); g_free(lista); g_free(det);
}

typedef struct { App *a; Op *op; GPtrArray *existen; } Choque;

static void choque_respuesta(GObject *src, GAsyncResult *res, gpointer d)
{
    Choque *ch = d;
    int r = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    Op *op = ch->op;
    if (r == 1) {   /* omitir los que ya existen */
        for (guint i = 0; i < ch->existen->len; i++)
            for (guint j = 0; j < op->nombres->len; j++) {
                char *b = g_path_get_basename(g_ptr_array_index(op->nombres, j));
                gboolean igual = strcmp(b, g_ptr_array_index(ch->existen, i)) == 0;
                g_free(b);
                if (igual) { g_ptr_array_remove_index(op->nombres, j); break; }
            }
        if (op->nombres->len == 0) { estado(op->s, "Nada que copiar: ya existen todos"); op_free(op); }
        else copiar_lanzar(op);
    } else if (r == 2) {
        copiar_lanzar(op);   /* reemplazar: ya se confirmó aquí, no se pregunta de nuevo */
    } else {
        estado(op->s, "Copia cancelada");
        op_free(op);
    }
    g_ptr_array_unref(ch->existen);
    g_free(ch);
}

/* Copia "nombres" (relativos a ruta_origen, o rutas completas si es NULL) a la carpeta del panel d.
 * Si algo ya existe en el destino, pregunta siempre; si no, pregunta según la preferencia. */
static void copiar_iniciar(Sesion *s, Panel *d, GPtrArray *nombres, const char *ruta_origen, gboolean desde_remoto)
{
    App *a = s->app;
    if (!s->con || !d->cwd) { estado(s, "Conéctate a un servidor primero"); g_ptr_array_unref(nombres); return; }
    if (nombres->len == 0) { estado(s, "Selecciona qué copiar"); g_ptr_array_unref(nombres); return; }
    if (s->ocupado) { estado(s, "Espera a que termine la operación en curso"); g_ptr_array_unref(nombres); return; }
    Op *op = op_nueva(s, OP_COPIAR);
    op->remoto = desde_remoto;
    op->ruta = g_strdup(ruta_origen);
    op->ruta2 = g_strdup(d->cwd);
    op->nombres = nombres;

    GPtrArray *existen = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < nombres->len; i++) {
        char *b = g_path_get_basename(g_ptr_array_index(nombres, i));
        if (existe_en(d, b)) g_ptr_array_add(existen, b);
        else g_free(b);
    }
    if (existen->len > 0) {
        char *lista = nombres_texto(existen, 10);
        char *det = g_strdup_printf("%s\nEn %s ya hay elementos con el mismo nombre. "
                                    "Reemplazar los sobrescribe (en las carpetas, los archivos que coincidan).",
                                    lista, d->cwd);
        GtkAlertDialog *dlg = gtk_alert_dialog_new("¿Reemplazar %u elemento(s) que ya existen?", existen->len);
        gtk_alert_dialog_set_detail(dlg, det);
        const char *botones[] = { "Cancelar", "Omitir existentes", "Reemplazar", NULL };
        gtk_alert_dialog_set_buttons(dlg, botones);
        gtk_alert_dialog_set_cancel_button(dlg, 0);
        gtk_alert_dialog_set_default_button(dlg, 0);   /* Enter no pisa nada */
        Choque *ch = g_new0(Choque, 1);
        ch->a = a; ch->op = op; ch->existen = existen;
        if (g_getenv("SFTPC_DEMO")) g_print("CONFLICTO: %u ya existen: %s", existen->len, lista);
        gtk_alert_dialog_choose(dlg, GTK_WINDOW(a->win), NULL, choque_respuesta, ch);
        g_object_unref(dlg);
        g_free(lista); g_free(det);
        return;
    }
    g_ptr_array_unref(existen);
    if (a->confirmar_copia) confirmar_copia(a, op);
    else copiar_lanzar(op);
}

/* ---------- acciones ---------- */

static void accion_copiar(App *a)
{
    Sesion *s = a->s;
    Panel *o = s->activo, *d = otro_panel(s, o);
    if (!s->con || !d->cwd || !o->cwd) { estado(s, "Conéctate a un servidor primero"); return; }
    copiar_iniciar(s, d, panel_seleccion(o), o->cwd, o->remoto);
}

static void crear_cb(App *a, const char *texto, gpointer d)
{
    Panel *p = a->panel_dialogo;
    if (strchr(texto, '/')) { estado(p->s, "El nombre no puede llevar /"); return; }
    Op *op = op_nueva(p->s, GPOINTER_TO_INT(d));
    op->remoto = p->remoto;
    op->ruta = ruta_unir(p->cwd, texto);
    lanzar(op);
}

static void pedir_creacion(App *a, TipoOp tipo)
{
    Panel *p = a->s->activo;
    if (!p->cwd) { estado(a->s, "Conéctate a un servidor primero"); return; }
    a->panel_dialogo = p;
    if (tipo == OP_MKDIR) pedir_texto(a, "Nueva carpeta", "", crear_cb, GINT_TO_POINTER(OP_MKDIR), NULL);
    else pedir_texto(a, "Nuevo documento", "nuevo.txt", crear_cb, GINT_TO_POINTER(OP_NUEVO_DOC), NULL);
}

static void accion_mkdir(App *a) { pedir_creacion(a, OP_MKDIR); }
static void accion_nuevo_doc(App *a) { pedir_creacion(a, OP_NUEVO_DOC); }

static void renombrar_cb(App *a, const char *texto, gpointer d)
{
    Panel *p = a->panel_dialogo;
    if (strcmp(texto, d) == 0) return;
    Op *op = op_nueva(p->s, OP_RENOMBRAR);
    op->remoto = p->remoto;
    op->ruta = ruta_unir(p->cwd, d);
    op->ruta2 = ruta_unir(p->cwd, texto);
    lanzar(op);
}

static void accion_renombrar(App *a)
{
    Panel *p = a->s->activo;
    GPtrArray *sel = panel_seleccion(p);
    if (sel->len != 1) estado(a->s, "Selecciona un solo elemento para renombrar");
    else {
        a->panel_dialogo = p;
        const char *n = g_ptr_array_index(sel, 0);
        pedir_texto(a, "Renombrar", n, renombrar_cb, g_strdup(n), g_free);
    }
    g_ptr_array_unref(sel);
}

static void borrar_respuesta(GObject *src, GAsyncResult *res, gpointer d)
{
    Op *op = d;
    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL) == 1) lanzar(op);
    else op_free(op);
}

static void accion_borrar(App *a)
{
    Sesion *s = a->s;
    Panel *p = s->activo;
    GPtrArray *sel = panel_seleccion(p);
    if (sel->len == 0) { estado(s, "Selecciona qué borrar"); g_ptr_array_unref(sel); return; }
    GString *det = g_string_new(NULL);
    for (guint i = 0; i < sel->len && i < 10; i++)
        g_string_append_printf(det, "%s\n", (char *)g_ptr_array_index(sel, i));
    if (sel->len > 10) g_string_append_printf(det, "… y %u más\n", sel->len - 10);
    g_string_append(det, p->remoto ? "\nSe borra del servidor, las carpetas con todo su contenido."
                                   : "\nSe borra de este equipo, las carpetas con todo su contenido.");
    Op *op = op_nueva(s, OP_BORRAR);
    op->remoto = p->remoto;
    op->ruta = g_strdup(p->cwd);
    op->nombres = sel;
    GtkAlertDialog *dlg = gtk_alert_dialog_new("¿Borrar %u elemento(s)?", sel->len);
    gtk_alert_dialog_set_detail(dlg, det->str);
    const char *botones[] = { "Cancelar", "Borrar", NULL };
    gtk_alert_dialog_set_buttons(dlg, botones);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(a->win), NULL, borrar_respuesta, op);
    g_object_unref(dlg);
    g_string_free(det, TRUE);
}

static void accion_refrescar(App *a) { refrescar(a->s); }

static void accion_ocultos(App *a)
{
    a->ocultos = !a->ocultos;
    estado(a->s, a->ocultos ? "Mostrando archivos ocultos" : "Ocultando archivos ocultos");
    refrescar(a->s);
}

static void accion_servidores(App *a) { gestor_abrir(a); }

/* Ctrl+F: busca en la carpeta del panel activo */
static void accion_buscar(App *a)
{
    Panel *p = a->s->activo;
    if (!p->cwd) { estado(a->s, "Conéctate a un servidor primero"); return; }
    gtk_widget_set_visible(p->buscar, TRUE);
    gtk_widget_grab_focus(p->buscar);
}

static void portapapeles_leido(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    char *t = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    g_print("PORTAPAPELES: %s\n", t ? t : "(vacío)");
    g_free(t);
}

/* Copia la ruta completa de lo seleccionado (una por línea), o la de la carpeta actual */
static void accion_copiar_ruta(App *a)
{
    Sesion *s = a->s;
    Panel *p = s->activo;
    if (!p->cwd) { estado(s, "Conéctate a un servidor primero"); return; }
    GPtrArray *sel = panel_seleccion(p);
    GString *t = g_string_new(NULL);
    if (sel->len == 0) g_string_append(t, p->cwd);
    for (guint i = 0; i < sel->len; i++) {
        char *r = ruta_unir(p->cwd, g_ptr_array_index(sel, i));
        if (i) g_string_append_c(t, '\n');
        g_string_append(t, r);
        g_free(r);
    }
    GdkClipboard *cb = gtk_widget_get_clipboard(a->win);
    gdk_clipboard_set_text(cb, t->str);
    if (sel->len > 1) estado(s, "Copiadas %u rutas al portapapeles", sel->len);
    else estado(s, "Ruta copiada: %s", t->str);
    if (g_strcmp0(g_getenv("SFTPC_DEMO"), "copiar-ruta") == 0)
        gdk_clipboard_read_text_async(cb, NULL, portapapeles_leido, NULL);
    g_string_free(t, TRUE);
    g_ptr_array_unref(sel);
}

static void accion_nueva_pestana(App *a)
{
    nueva_sesion(a);
    gtk_widget_grab_focus(a->servidor);
}

static void accion_cerrar_pestana(App *a) { cerrar_sesion(a->s); }

/* ---------- preferencias (~/.config/sftp-commander/preferencias.ini) ---------- */

static char *ruta_preferencias(void)
{
    return g_build_filename(g_get_user_config_dir(), "sftp-commander", "preferencias.ini", NULL);
}

static void preferencias_cargar(App *a)
{
    GKeyFile *k = g_key_file_new();
    char *r = ruta_preferencias();
    a->confirmar_copia = TRUE;   /* por defecto se pregunta */
    if (g_key_file_load_from_file(k, r, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        gboolean v = g_key_file_get_boolean(k, "general", "confirmar_copia", &e);
        if (!e) a->confirmar_copia = v;
        g_clear_error(&e);
    }
    g_free(r);
    g_key_file_free(k);
}

static void preferencias_guardar(App *a)
{
    GKeyFile *k = g_key_file_new();
    char *r = ruta_preferencias();
    g_key_file_load_from_file(k, r, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_boolean(k, "general", "confirmar_copia", a->confirmar_copia);
    char *dir = g_path_get_dirname(r);
    g_mkdir_with_parents(dir, 0700);
    g_key_file_save_to_file(k, r, NULL);
    g_free(dir);
    g_free(r);
    g_key_file_free(k);
}

/* Menú ☰ > "Preguntar antes de copiar" */
static void act_confirmar_cambio(GSimpleAction *x, GVariant *v, gpointer d)
{
    App *a = d;
    g_simple_action_set_state(x, v);
    a->confirmar_copia = g_variant_get_boolean(v);
    preferencias_guardar(a);
    estado(a->s, a->confirmar_copia ? "Se preguntará antes de copiar" : "Se copiará sin preguntar (salvo si algo ya existe)");
}

/* ---------- Acerca de ---------- */

static void accion_acerca(App *a)
{
    const char *autores[] = { "David G https://github.com/reybendg", NULL };
    GtkIconTheme *tema = gtk_icon_theme_get_for_display(gtk_widget_get_display(a->win));
    char *sistema = g_strdup_printf("SFTP Commander %s\nGTK %u.%u.%u\nlibssh %s\nGLib %u.%u.%u",
                                    VERSION, gtk_get_major_version(), gtk_get_minor_version(),
                                    gtk_get_micro_version(), con_version_libssh(),
                                    glib_major_version, glib_minor_version, glib_micro_version);
    GtkWidget *dlg = gtk_about_dialog_new();
    GtkAboutDialog *ad = GTK_ABOUT_DIALOG(dlg);
    gtk_about_dialog_set_program_name(ad, "SFTP Commander");
    gtk_about_dialog_set_version(ad, VERSION);
    gtk_about_dialog_set_logo_icon_name(ad, gtk_icon_theme_has_icon(tema, "sftp-commander")
                                            ? "sftp-commander" : "folder-remote");
    gtk_about_dialog_set_comments(ad, "Cliente SFTP de dos paneles para Linux y Windows, "
                                      "al estilo del modo Commander de WinSCP.");
    gtk_about_dialog_set_website(ad, WEB_PROYECTO);
    gtk_about_dialog_set_website_label(ad, "Proyecto en GitHub");
    gtk_about_dialog_set_authors(ad, autores);
    gtk_about_dialog_set_copyright(ad, "© 2026 David G");
    gtk_about_dialog_set_license_type(ad, GTK_LICENSE_GPL_3_0);
    gtk_about_dialog_set_system_information(ad, sistema);
    gtk_about_dialog_add_credit_section(ad, "Reportar un error o pedir una función",
                                        (const char *[]){ "Issues en GitHub " WEB_PROYECTO "/issues", NULL });
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(a->win));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_present(GTK_WINDOW(dlg));
    if (g_getenv("SFTPC_DEMO")) {
        g_print("ACERCA: SFTP Commander %s · %s\n", VERSION, WEB_PROYECTO);
        g_print("ICONO PROPIO: %s\n", gtk_icon_theme_has_icon(tema, "sftp-commander") ? "sí" : "no");
        if (g_strcmp0(g_getenv("SFTPC_DEMO"), "acerca") == 0) a->captura_widget = dlg;
    }
    g_free(sistema);
}

/* Acciones de ventana "win.*": las usan el menú contextual, los botones y los atajos */
#define ACCION(n) \
    static void act_##n(GSimpleAction *x, GVariant *v, gpointer d) { (void)x; (void)v; accion_##n(d); }
ACCION(abrir)
ACCION(copiar)
ACCION(renombrar)
ACCION(borrar)
ACCION(mkdir)
ACCION(nuevo_doc)
ACCION(refrescar)
ACCION(ocultos)
ACCION(servidores)
ACCION(buscar)
ACCION(copiar_ruta)
ACCION(nueva_pestana)
ACCION(cerrar_pestana)
ACCION(acerca)

static const GActionEntry ACCIONES[] = {
    { "abrir",          act_abrir,          NULL, NULL, NULL, { 0 } },
    { "copiar",         act_copiar,         NULL, NULL, NULL, { 0 } },
    { "renombrar",      act_renombrar,      NULL, NULL, NULL, { 0 } },
    { "borrar",         act_borrar,         NULL, NULL, NULL, { 0 } },
    { "mkdir",          act_mkdir,          NULL, NULL, NULL, { 0 } },
    { "nuevo-doc",      act_nuevo_doc,      NULL, NULL, NULL, { 0 } },
    { "refrescar",      act_refrescar,      NULL, NULL, NULL, { 0 } },
    { "ocultos",        act_ocultos,        NULL, NULL, NULL, { 0 } },
    { "servidores",     act_servidores,     NULL, NULL, NULL, { 0 } },
    { "buscar",         act_buscar,         NULL, NULL, NULL, { 0 } },
    { "copiar-ruta",    act_copiar_ruta,    NULL, NULL, NULL, { 0 } },
    { "nueva-pestana",  act_nueva_pestana,  NULL, NULL, NULL, { 0 } },
    { "cerrar-pestana", act_cerrar_pestana, NULL, NULL, NULL, { 0 } },
    { "acerca",         act_acerca,         NULL, NULL, NULL, { 0 } },
    /* sin "activate": al activarla se invierte sola y llama a change_state */
    { "confirmar-copia", NULL,              NULL, "true", act_confirmar_cambio, { 0 } },
};

/* Abrir/editar no tiene tecla de función: Enter o doble clic. F5 actualiza, como en los exploradores. */
static const struct { const char *accion; const char *teclas[3]; } ATAJOS[] = {
    { "win.copiar",         { "F4", NULL } },
    { "win.renombrar",      { "F2", NULL } },
    { "win.borrar",         { "F8", NULL } },
    { "win.mkdir",          { "F7", NULL } },
    { "win.nuevo-doc",      { "<Shift>F4", NULL } },
    { "win.refrescar",      { "F5", "<Control>r", NULL } },
    { "win.ocultos",        { "<Control>h", NULL } },
    { "win.servidores",     { "<Control>l", NULL } },
    { "win.buscar",         { "<Control>f", NULL } },
    { "win.copiar-ruta",    { "<Control><Shift>c", NULL } },
    { "win.nueva-pestana",  { "<Control>t", NULL } },
    { "win.cerrar-pestana", { "<Control>w", NULL } },
    { "win.acerca",         { "F1", NULL } },
};

static GMenuModel *crear_menu_contextual(void)
{
    GMenu *m = g_menu_new(), *s1 = g_menu_new(), *s2 = g_menu_new(), *s3 = g_menu_new(), *s4 = g_menu_new();
    g_menu_append(s1, "Abrir / editar (Enter)", "win.abrir");
    g_menu_append(s1, "Copiar al otro panel", "win.copiar");
    g_menu_append(s1, "Renombrar", "win.renombrar");
    g_menu_append(s1, "Borrar", "win.borrar");
    g_menu_append(s2, "Copiar ruta", "win.copiar-ruta");
    g_menu_append(s2, "Buscar en esta carpeta", "win.buscar");
    g_menu_append(s3, "Nueva carpeta", "win.mkdir");
    g_menu_append(s3, "Nuevo documento", "win.nuevo-doc");
    g_menu_append(s4, "Actualizar", "win.refrescar");
    g_menu_append(s4, "Mostrar u ocultar archivos ocultos", "win.ocultos");
    g_menu_append_section(m, NULL, G_MENU_MODEL(s1));
    g_menu_append_section(m, NULL, G_MENU_MODEL(s2));
    g_menu_append_section(m, NULL, G_MENU_MODEL(s3));
    g_menu_append_section(m, NULL, G_MENU_MODEL(s4));
    g_object_unref(s1); g_object_unref(s2); g_object_unref(s3); g_object_unref(s4);
    return G_MENU_MODEL(m);
}

static gboolean atajo_arriba(GtkWidget *w, GVariant *v, gpointer d)
{
    (void)w; (void)v;
    panel_arriba(d);
    return TRUE;
}

static gboolean atajo_borrar(GtkWidget *w, GVariant *v, gpointer d)
{
    (void)w; (void)v;
    accion_borrar(d);
    return TRUE;
}

static void agregar_atajo(GtkEventController *ctl, guint tecla, GtkShortcutFunc fn, gpointer d)
{
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(ctl),
        gtk_shortcut_new(gtk_keyval_trigger_new(tecla, 0), gtk_callback_action_new(fn, d, NULL)));
}

/* ---------- servidores guardados (sin contraseñas) ---------- */

static char *ruta_servidores(void)
{
    return g_build_filename(g_get_user_config_dir(), "sftp-commander", "servidores.ini", NULL);
}

static int cmp_nombres(const void *x, const void *y)
{
    return g_utf8_collate(*(char * const *)x, *(char * const *)y);
}

/* Nombres de los servidores guardados, ordenados (liberar con g_strfreev) */
static char **servidores_nombres(App *a, gsize *n)
{
    char **g = g_key_file_get_groups(a->servidores, n);
    qsort(g, *n, sizeof(char *), cmp_nombres);
    return g;
}

static char *srv_dato(App *a, const char *nombre, const char *clave)
{
    char *v = g_key_file_get_string(a->servidores, nombre, clave, NULL);
    if (v && !*v) g_clear_pointer(&v, g_free);
    return v;
}

static void servidores_menu(App *a)
{
    gsize n = 0;
    char **g = servidores_nombres(a, &n);
    guint actual = g_list_model_get_n_items(G_LIST_MODEL(a->nombres_srv));
    gtk_string_list_splice(a->nombres_srv, 1, actual - 1, (const char * const *)g);
    g_strfreev(g);
}

static void servidores_cargar(App *a)
{
    a->servidores = g_key_file_new();
    char *ruta = ruta_servidores();
    g_key_file_load_from_file(a->servidores, ruta, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_free(ruta);
    servidores_menu(a);
}

static gboolean servidores_guardar(App *a, GError **err)
{
    char *ruta = ruta_servidores();
    char *dir = g_path_get_dirname(ruta);
    g_mkdir_with_parents(dir, 0700);
    gboolean ok = g_key_file_save_to_file(a->servidores, ruta, err);
    if (ok) g_chmod(ruta, 0600);
    g_free(dir);
    g_free(ruta);
    servidores_menu(a);
    return ok;
}

/* Pasa un servidor guardado a la barra de conexión */
static void servidor_llenar(App *a, const char *nombre)
{
    char *srv = srv_dato(a, nombre, "servidor"), *pto = srv_dato(a, nombre, "puerto"),
         *usr = srv_dato(a, nombre, "usuario");
    a->llenando = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(a->servidor), srv ? srv : "");
    gtk_editable_set_text(GTK_EDITABLE(a->puerto), pto ? pto : "");
    gtk_editable_set_text(GTK_EDITABLE(a->usuario), usr ? usr : "");
    gtk_editable_set_text(GTK_EDITABLE(a->clave), "");
    a->llenando = FALSE;
    g_free(a->sel_llave);
    g_free(a->sel_carpeta);
    g_free(a->srv_nombre);
    a->sel_llave = srv_dato(a, nombre, "llave");
    a->sel_carpeta = srv_dato(a, nombre, "carpeta");
    a->srv_nombre = g_strdup(nombre);
    g_free(a->sel_salto); a->sel_salto = srv_dato(a, nombre, "salto");
    g_free(a->sel_llave_salto); a->sel_llave_salto = srv_dato(a, nombre, "llave_salto");
    g_free(srv); g_free(pto); g_free(usr);
}

/* La barra muestra los datos de la pestaña visible */
static void barra_desde_sesion(App *a, Sesion *s)
{
    a->llenando = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(a->servidor), s->c_servidor ? s->c_servidor : "");
    gtk_editable_set_text(GTK_EDITABLE(a->puerto), s->c_puerto ? s->c_puerto : "");
    gtk_editable_set_text(GTK_EDITABLE(a->usuario), s->c_usuario ? s->c_usuario : "");
    gtk_editable_set_text(GTK_EDITABLE(a->clave), "");
    a->llenando = FALSE;
    g_free(a->sel_llave); a->sel_llave = g_strdup(s->c_llave);
    g_free(a->sel_carpeta); a->sel_carpeta = g_strdup(s->c_carpeta);
    g_free(a->srv_nombre); a->srv_nombre = g_strdup(s->c_nombre);
    g_free(a->sel_salto); a->sel_salto = g_strdup(s->c_salto);
    g_free(a->sel_llave_salto); a->sel_llave_salto = g_strdup(s->c_llave_salto);
}

static void conectar_barra(App *a);

/* Conectar a un servidor guardado: si la pestaña visible ya está en uso, en una nueva */
static void conectar_a(App *a, const char *nombre)
{
    Sesion *s = a->s;
    if (s->con || s->ocupado || s->op_con) nueva_sesion(a);
    servidor_llenar(a, nombre);
    conectar_barra(a);
}

static void on_servidor_elegido(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps;
    App *a = d;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    if (i == 0 || i == GTK_INVALID_LIST_POSITION) return;
    char *nombre = g_strdup(gtk_string_list_get_string(a->nombres_srv, i));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(o), 0);
    conectar_a(a, nombre);
    g_free(nombre);
}

static void on_servidor_editado(GtkEditable *e, gpointer d)
{
    (void)e;
    App *a = d;
    if (a->llenando) return;
    /* se escribió otro servidor a mano: ya no aplica la llave ni la carpeta guardadas */
    g_clear_pointer(&a->sel_llave, g_free);
    g_clear_pointer(&a->sel_carpeta, g_free);
    g_clear_pointer(&a->srv_nombre, g_free);
    g_clear_pointer(&a->sel_salto, g_free);
    g_clear_pointer(&a->sel_llave_salto, g_free);
}

/* Ventana para administrar la lista */
typedef struct {
    App *a;
    GtkWidget *win, *lista, *nombre, *servidor, *puerto, *usuario, *llave, *carpeta, *aviso;
    GtkWidget *salto, *llave_salto, *llave_destino;   /* llave_destino: campo que llena "Buscar…" */
    char *actual;
} Gestor;

static void gestor_free(gpointer p)
{
    Gestor *g = p;
    g_free(g->actual);
    g_free(g);
}

static void gestor_poner(Gestor *g, GtkWidget *w, const char *clave)
{
    char *v = g->actual ? srv_dato(g->a, g->actual, clave) : NULL;
    gtk_editable_set_text(GTK_EDITABLE(w), v ? v : "");
    g_free(v);
}

static void gestor_lista(Gestor *g, const char *seleccionar)
{
    gtk_list_box_remove_all(GTK_LIST_BOX(g->lista));
    gsize n = 0;
    char **grupos = servidores_nombres(g->a, &n);
    for (gsize i = 0; i < n; i++) {
        GtkWidget *l = gtk_label_new(grupos[i]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        margenes(l, 6);
        gtk_list_box_append(GTK_LIST_BOX(g->lista), l);
        if (seleccionar && strcmp(grupos[i], seleccionar) == 0)
            gtk_list_box_select_row(GTK_LIST_BOX(g->lista),
                                    gtk_list_box_get_row_at_index(GTK_LIST_BOX(g->lista), (int)i));
    }
    g_strfreev(grupos);
}

static const char *nombre_fila(GtkListBoxRow *row)
{
    return gtk_label_get_text(GTK_LABEL(gtk_list_box_row_get_child(row)));
}

static void gestor_fila(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb;
    Gestor *g = d;
    if (!row) return;
    g_free(g->actual);
    g->actual = g_strdup(nombre_fila(row));
    gtk_editable_set_text(GTK_EDITABLE(g->nombre), g->actual);
    gestor_poner(g, g->servidor, "servidor");
    gestor_poner(g, g->puerto, "puerto");
    gestor_poner(g, g->usuario, "usuario");
    gestor_poner(g, g->llave, "llave");
    gestor_poner(g, g->carpeta, "carpeta");
    gestor_poner(g, g->salto, "salto");
    gestor_poner(g, g->llave_salto, "llave_salto");
    gtk_label_set_text(GTK_LABEL(g->aviso), "");
}

static void gestor_nuevo(GtkButton *b, gpointer d)
{
    (void)b;
    Gestor *g = d;
    App *a = g->a;
    gtk_list_box_unselect_all(GTK_LIST_BOX(g->lista));
    g_clear_pointer(&g->actual, g_free);
    /* parte de lo que haya en la barra de conexión */
    gtk_editable_set_text(GTK_EDITABLE(g->servidor), texto_de(a->servidor));
    gtk_editable_set_text(GTK_EDITABLE(g->puerto), texto_de(a->puerto));
    gtk_editable_set_text(GTK_EDITABLE(g->usuario), texto_de(a->usuario));
    gtk_editable_set_text(GTK_EDITABLE(g->nombre), texto_de(a->servidor));
    gtk_editable_set_text(GTK_EDITABLE(g->llave), a->sel_llave ? a->sel_llave : "");
    gtk_editable_set_text(GTK_EDITABLE(g->carpeta), a->sel_carpeta ? a->sel_carpeta : "");
    gtk_editable_set_text(GTK_EDITABLE(g->salto), a->sel_salto ? a->sel_salto : "");
    gtk_editable_set_text(GTK_EDITABLE(g->llave_salto), a->sel_llave_salto ? a->sel_llave_salto : "");
    gtk_label_set_text(GTK_LABEL(g->aviso), "Completa los datos y pulsa Guardar.");
    gtk_widget_grab_focus(g->nombre);
}

static void gestor_dato(Gestor *g, const char *nombre, const char *clave, GtkWidget *w)
{
    char *v = g_strstrip(g_strdup(texto_de(w)));
    if (*v) g_key_file_set_string(g->a->servidores, nombre, clave, v);
    else g_key_file_remove_key(g->a->servidores, nombre, clave, NULL);
    g_free(v);
}

static gboolean gestor_guardar_datos(Gestor *g)
{
    App *a = g->a;
    char *srv = g_strstrip(g_strdup(texto_de(g->servidor)));
    char *nombre = g_strstrip(g_strdup(texto_de(g->nombre)));
    const char *pto = texto_de(g->puerto);
    gboolean ok = FALSE;
    if (!*srv) { gtk_label_set_text(GTK_LABEL(g->aviso), "Falta el servidor."); goto fin; }
    if (*pto && (atoi(pto) <= 0 || atoi(pto) > 65535)) {
        gtk_label_set_text(GTK_LABEL(g->aviso), "El puerto debe ser un número entre 1 y 65535.");
        goto fin;
    }
    if (!*nombre) { g_free(nombre); nombre = g_strdup(srv); }
    if (strpbrk(nombre, "[]\n")) { gtk_label_set_text(GTK_LABEL(g->aviso), "El nombre no puede llevar [ ]."); goto fin; }
    {
        const char *st = texto_de(g->salto);
        char *su, *sh;
        int sp;
        if (*st && !destino_parsear(st, &su, &sh, &sp)) {
            gtk_label_set_text(GTK_LABEL(g->aviso), "El servidor de salto no es válido: usa usuario@servidor:puerto.");
            goto fin;
        }
        if (*st) { g_free(su); g_free(sh); }
    }
    if (g->actual && strcmp(g->actual, nombre) != 0)
        g_key_file_remove_group(a->servidores, g->actual, NULL);
    g_key_file_set_string(a->servidores, nombre, "servidor", srv);
    gestor_dato(g, nombre, "puerto", g->puerto);
    gestor_dato(g, nombre, "usuario", g->usuario);
    gestor_dato(g, nombre, "llave", g->llave);
    gestor_dato(g, nombre, "carpeta", g->carpeta);
    gestor_dato(g, nombre, "salto", g->salto);
    gestor_dato(g, nombre, "llave_salto", g->llave_salto);
    GError *err = NULL;
    if (!servidores_guardar(a, &err)) {
        gtk_label_set_text(GTK_LABEL(g->aviso), err->message);
        g_error_free(err);
        goto fin;
    }
    g_free(g->actual);
    g->actual = g_strdup(nombre);
    gestor_lista(g, nombre);
    gtk_label_set_text(GTK_LABEL(g->aviso), "Guardado.");
    ok = TRUE;
fin:
    g_free(srv);
    g_free(nombre);
    return ok;
}

static void gestor_guardar(GtkButton *b, gpointer d) { (void)b; gestor_guardar_datos(d); }

static void gestor_borrar_resp(GObject *src, GAsyncResult *res, gpointer d)
{
    Gestor *g = d;
    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL) != 1 || !g->actual) return;
    llavero_olvidar_nombre(g->actual, NULL);   /* sus contraseñas guardadas, si había */
    char *ns = g_strdup_printf("%s (salto)", g->actual);
    llavero_olvidar_nombre(ns, NULL);
    g_free(ns);
    g_key_file_remove_group(g->a->servidores, g->actual, NULL);
    servidores_guardar(g->a, NULL);
    g_clear_pointer(&g->actual, g_free);
    gestor_lista(g, NULL);
    gtk_label_set_text(GTK_LABEL(g->aviso), "Servidor quitado de la lista.");
}

static void gestor_borrar(GtkButton *b, gpointer d)
{
    (void)b;
    Gestor *g = d;
    if (!g->actual) { gtk_label_set_text(GTK_LABEL(g->aviso), "Elige un servidor de la lista."); return; }
    GtkAlertDialog *dlg = gtk_alert_dialog_new("¿Quitar \"%s\" de la lista?", g->actual);
    gtk_alert_dialog_set_detail(dlg, "Solo se borra de la lista guardada; el servidor no se toca.");
    const char *botones[] = { "Cancelar", "Quitar", NULL };
    gtk_alert_dialog_set_buttons(dlg, botones);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(g->win), NULL, gestor_borrar_resp, g);
    g_object_unref(dlg);
}

static void gestor_olvidar(GtkButton *b, gpointer d)
{
    (void)b;
    Gestor *g = d;
    if (!g->actual) { gtk_label_set_text(GTK_LABEL(g->aviso), "Elige un servidor de la lista."); return; }
    GError *err = NULL;
    gboolean habia = llavero_olvidar_nombre(g->actual, &err);
    if (!err) {   /* también la del servidor de salto */
        char *ns = g_strdup_printf("%s (salto)", g->actual);
        habia = llavero_olvidar_nombre(ns, &err) || habia;
        g_free(ns);
    }
    if (err) {
        char *t = g_strdup_printf("No se pudo usar el llavero: %s", err->message);
        gtk_label_set_text(GTK_LABEL(g->aviso), t);
        g_free(t);
        g_error_free(err);
    } else {
        gtk_label_set_text(GTK_LABEL(g->aviso), habia ? "Contraseña borrada del llavero."
                                                      : "No había contraseña guardada para este servidor.");
    }
}

static void gestor_conectar(GtkButton *b, gpointer d)
{
    (void)b;
    Gestor *g = d;
    if (!gestor_guardar_datos(g)) return;
    char *nombre = g_strdup(g->actual);
    App *a = g->a;
    gtk_window_destroy(GTK_WINDOW(g->win));   /* libera g */
    conectar_a(a, nombre);
    g_free(nombre);
}

static void gestor_fila_activada(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)row;
    gestor_conectar(NULL, d);
}

static void llave_elegida(GObject *src, GAsyncResult *res, gpointer d)
{
    Gestor *g = d;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    char *ruta = g_file_get_path(f);
    if (ruta) gtk_editable_set_text(GTK_EDITABLE(g->llave_destino), ruta);
    g_free(ruta);
    g_object_unref(f);
}

static void gestor_buscar_llave(GtkButton *b, gpointer d)
{
    Gestor *g = d;
    /* el botón sabe qué campo llenar (llave del servidor o del salto) */
    g->llave_destino = g_object_get_data(G_OBJECT(b), "campo");
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Elegir llave privada");
    char *ssh = g_build_filename(g_get_home_dir(), ".ssh", NULL);
    GFile *dir = g_file_new_for_path(ssh);
    gtk_file_dialog_set_initial_folder(fd, dir);
    gtk_file_dialog_open(fd, GTK_WINDOW(g->win), NULL, llave_elegida, g);
    g_object_unref(dir);
    g_free(ssh);
    g_object_unref(fd);
}

static GtkWidget *fila_form(GtkWidget *grid, int fila, const char *etiqueta, GtkWidget *campo)
{
    GtkWidget *l = gtk_label_new(etiqueta);
    gtk_label_set_xalign(GTK_LABEL(l), 1);
    gtk_grid_attach(GTK_GRID(grid), l, 0, fila, 1, 1);
    gtk_widget_set_hexpand(campo, TRUE);
    gtk_grid_attach(GTK_GRID(grid), campo, 1, fila, 1, 1);
    return campo;
}

static GtkWidget *entrada(const char *ayuda)
{
    GtkWidget *e = gtk_entry_new();
    if (ayuda) gtk_entry_set_placeholder_text(GTK_ENTRY(e), ayuda);
    return e;
}

/* Campo de llave privada con su botón "Buscar…" */
static GtkWidget *caja_llave(Gestor *g, GtkWidget **campo, const char *ayuda)
{
    GtkWidget *caja = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    *campo = entrada(ayuda);
    gtk_widget_set_hexpand(*campo, TRUE);
    GtkWidget *b = gtk_button_new_with_label("Buscar…");
    g_object_set_data(G_OBJECT(b), "campo", *campo);
    g_signal_connect(b, "clicked", G_CALLBACK(gestor_buscar_llave), g);
    gtk_box_append(GTK_BOX(caja), *campo);
    gtk_box_append(GTK_BOX(caja), b);
    return caja;
}

static void gestor_abrir(App *a)
{
    Gestor *g = g_new0(Gestor, 1);
    g->a = a;
    g->win = gtk_window_new();
    a->gestor_win = g->win;
    gtk_window_set_title(GTK_WINDOW(g->win), "Servidores guardados");
    gtk_window_set_transient_for(GTK_WINDOW(g->win), GTK_WINDOW(a->win));
    gtk_window_set_modal(GTK_WINDOW(g->win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(g->win), 760, 470);
    g_object_set_data_full(G_OBJECT(g->win), "gestor", g, gestor_free);

    /* izquierda: lista */
    g->lista = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g->lista), GTK_SELECTION_SINGLE);
    g_signal_connect(g->lista, "row-selected", G_CALLBACK(gestor_fila), g);
    g_signal_connect(g->lista, "row-activated", G_CALLBACK(gestor_fila_activada), g);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), g->lista);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_size_request(scroll, 220, -1);
    gtk_widget_add_css_class(scroll, "frame");
    GtkWidget *bl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b_nuevo = gtk_button_new_with_label("Nuevo");
    GtkWidget *b_borrar = gtk_button_new_with_label("Quitar");
    g_signal_connect(b_nuevo, "clicked", G_CALLBACK(gestor_nuevo), g);
    g_signal_connect(b_borrar, "clicked", G_CALLBACK(gestor_borrar), g);
    gtk_box_append(GTK_BOX(bl), b_nuevo);
    gtk_box_append(GTK_BOX(bl), b_borrar);
    GtkWidget *izq = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(izq), scroll);
    gtk_box_append(GTK_BOX(izq), bl);

    /* derecha: formulario */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    g->nombre   = fila_form(grid, 0, "Nombre", entrada("Cómo aparece en la lista"));
    g->servidor = fila_form(grid, 1, "Servidor", entrada("IP, nombre o alias de ~/.ssh/config"));
    g->puerto   = fila_form(grid, 2, "Puerto", entrada("22"));
    gtk_editable_set_width_chars(GTK_EDITABLE(g->puerto), 6);
    gtk_widget_set_hexpand(g->puerto, FALSE);
    gtk_widget_set_halign(g->puerto, GTK_ALIGN_START);
    gtk_entry_set_input_purpose(GTK_ENTRY(g->puerto), GTK_INPUT_PURPOSE_DIGITS);
    g->usuario  = fila_form(grid, 3, "Usuario", entrada("el de ~/.ssh/config o el tuyo"));
    fila_form(grid, 4, "Llave privada", caja_llave(g, &g->llave, "opcional: ~/.ssh/id_ed25519"));
    g->carpeta  = fila_form(grid, 5, "Carpeta inicial", entrada("opcional: /var/www"));
    g->salto    = fila_form(grid, 6, "Servidor de salto", entrada("opcional: usuario@bastion:22 o alias"));
    gtk_widget_set_tooltip_text(g->salto, "Servidor intermedio por el que se llega a este (como ssh -J). "
                                          "Vacío = conexión directa.");
    fila_form(grid, 7, "Llave del salto",
              caja_llave(g, &g->llave_salto, "opcional: vacío = ssh-agent y llaves de ~/.ssh"));
    GtkWidget *caja_clave = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    /* sin salto de línea: los textos con wrap dentro del grid confunden la medida de GTK */
    GtkWidget *nota = gtk_label_new("Se pide al conectar.");
    gtk_widget_add_css_class(nota, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(nota), 0);
    gtk_label_set_ellipsize(GTK_LABEL(nota), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(nota, "La contraseña se pide al conectar. Si marcas \"Recordar\", "
                                      "queda cifrada en el llavero del sistema (gnome-keyring), nunca en archivos.");
    gtk_widget_set_hexpand(nota, TRUE);
    GtkWidget *b_olvidar = gtk_button_new_with_label("Olvidar contraseña");
    gtk_widget_set_valign(b_olvidar, GTK_ALIGN_CENTER);
    g_signal_connect(b_olvidar, "clicked", G_CALLBACK(gestor_olvidar), g);
    gtk_box_append(GTK_BOX(caja_clave), nota);
    gtk_box_append(GTK_BOX(caja_clave), b_olvidar);
    fila_form(grid, 8, "Contraseña", caja_clave);
    g->aviso = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g->aviso), 0);
    gtk_label_set_ellipsize(GTK_LABEL(g->aviso), PANGO_ELLIPSIZE_END);
    gtk_grid_attach(GTK_GRID(grid), g->aviso, 1, 9, 1, 1);

    GtkWidget *bd = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bd, GTK_ALIGN_END);
    gtk_widget_set_valign(bd, GTK_ALIGN_END);
    gtk_widget_set_vexpand(bd, TRUE);
    GtkWidget *b_guardar = gtk_button_new_with_label("Guardar");
    GtkWidget *b_conectar = gtk_button_new_with_label("Guardar y conectar");
    gtk_widget_add_css_class(b_conectar, "suggested-action");
    g_signal_connect(b_guardar, "clicked", G_CALLBACK(gestor_guardar), g);
    g_signal_connect(b_conectar, "clicked", G_CALLBACK(gestor_conectar), g);
    gtk_box_append(GTK_BOX(bd), b_guardar);
    gtk_box_append(GTK_BOX(bd), b_conectar);
    GtkWidget *der = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(der, TRUE);
    gtk_box_append(GTK_BOX(der), grid);
    gtk_box_append(GTK_BOX(der), bd);

    GtkWidget *raiz = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    margenes(raiz, 14);
    gtk_box_append(GTK_BOX(raiz), izq);
    gtk_box_append(GTK_BOX(raiz), der);
    gtk_window_set_child(GTK_WINDOW(g->win), raiz);
    cerrar_con_escape(g->win);

    gestor_lista(g, NULL);
    GtkListBoxRow *primera = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g->lista), 0);
    if (primera) gtk_list_box_select_row(GTK_LIST_BOX(g->lista), primera);
    else gestor_nuevo(NULL, g);
    gtk_window_present(GTK_WINDOW(g->win));
}

/* ---------- conexión ---------- */

static void boton_con_actualizar(App *a)
{
    Sesion *s = a->s;
    if (!s) return;
    gtk_button_set_label(GTK_BUTTON(a->btn_con), s->op_con ? "Cancelar" : s->con ? "Desconectar" : "Conectar");
    char *t = s->desc ? g_strdup_printf("%s - SFTP Commander", s->desc) : g_strdup("SFTP Commander");
    gtk_window_set_title(GTK_WINDOW(a->win), t);
    g_free(t);
}

/* Texto de la pestaña según su estado */
static void sesion_titulo(Sesion *s)
{
    if (s->cerrada) return;
    char *t;
    if (s->con && s->desc) t = g_strdup(s->desc);
    else if (s->op_con) t = g_strdup_printf("Conectando a %s…", s->c_servidor ? s->c_servidor : "");
    else if (s->perdida) t = g_strdup_printf("%s (sin conexión)", s->perdida);
    else t = g_strdup("Nueva sesión");
    gtk_label_set_text(GTK_LABEL(s->etiqueta), t);
    gtk_widget_set_tooltip_text(s->etiqueta, t);
    g_free(t);
    if (s == s->app->s) boton_con_actualizar(s->app);
}

/* Aviso con spinner sobre el panel remoto mientras se conecta */
static gboolean aviso_tic(gpointer d)
{
    Sesion *s = d;
    gint64 seg = (g_get_monotonic_time() - s->aviso_desde) / G_USEC_PER_SEC;
    char *t = seg >= 3 ? g_strdup_printf("Esperando respuesta… %" G_GINT64_FORMAT " s", seg) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(s->aviso_tiempo), t);
    g_free(t);
    return G_SOURCE_CONTINUE;
}

static void aviso_mostrar(Sesion *s, const char *detalle)
{
    gtk_label_set_text(GTK_LABEL(s->aviso_detalle), detalle);
    if (!gtk_widget_get_visible(s->aviso)) {
        s->aviso_desde = g_get_monotonic_time();
        gtk_label_set_text(GTK_LABEL(s->aviso_tiempo), "");
        gtk_widget_set_visible(s->aviso, TRUE);
        gtk_spinner_start(GTK_SPINNER(s->aviso_spinner));
        if (!s->aviso_reloj) s->aviso_reloj = g_timeout_add_seconds(1, aviso_tic, s);
    }
    sesion_titulo(s);
}

static void aviso_ocultar(Sesion *s)
{
    if (s->aviso_reloj) { g_source_remove(s->aviso_reloj); s->aviso_reloj = 0; }
    if (s->cerrada) return;
    gtk_widget_set_visible(s->aviso, FALSE);
    gtk_spinner_stop(GTK_SPINNER(s->aviso_spinner));
    sesion_titulo(s);
}

static void desconectar(Sesion *s)
{
    if (s->con) { con_cerrar(s->con); s->con = NULL; }
    g_clear_pointer(&s->desc, g_free);
    g_clear_pointer(&s->der.cwd, g_free);
    g_list_store_remove_all(s->der.store);
    if (s->cerrada) return;
    if (gtk_widget_get_visible(s->der.buscar)) buscar_limpiar(&s->der);
    gtk_editable_set_text(GTK_EDITABLE(s->der.ruta), "");
    gtk_label_set_text(GTK_LABEL(s->der.titulo), s->perdida ? "Remoto (conexión perdida)" : "Remoto (sin conexión)");
    sesion_titulo(s);
}

/* Cancelar mientras se conecta: el hilo sigue hasta que libssh responda o venza el
 * tiempo, pero su resultado se descarta y la interfaz queda libre al instante. */
static void cancelar_conexion(Sesion *s)
{
    if (!s->op_con) return;
    /* si el hilo espera una respuesta del usuario, contestarle "cancelado" */
    if (s->dlg_pregunta) pregunta_cerrar(s->dlg_pregunta);
    s->op_con->abandonada = TRUE;
    s->op_con = NULL;
    s->con = NULL;          /* la conexión la cierra op_terminada */
    s->ocupado = FALSE;
    aviso_ocultar(s);
    desconectar(s);
    estado(s, "Conexión cancelada");
}

static void on_aviso_cancelar(GtkButton *b, gpointer d) { (void)b; cancelar_conexion(d); }
static gboolean demo_cancelar(gpointer d) { cancelar_conexion(((App *)d)->s); return G_SOURCE_REMOVE; }

static void lanzar_auth(Sesion *s, gboolean guardar_host, gboolean salto)
{
    Op *op = op_nueva(s, OP_AUTENTICAR);
    op->guardar_host = guardar_host;
    op->salto = salto;
    if (salto) {
        /* la contraseña del salto se guarda en el llavero como "<servidor> (salto)" */
        op->clave = s->clave_salto_pend;
        s->clave_salto_pend = NULL;
        op->recordar = s->recordar_salto;
        op->nombre_srv = s->c_nombre ? g_strdup_printf("%s (salto)", s->c_nombre) : NULL;
        s->recordar_salto = FALSE;
        estado(s, "Autenticando en el servidor de salto…");
        aviso_mostrar(s, "Iniciando sesión en el servidor de salto…");
    } else {
        op->clave = s->clave_pend;      /* la Op se queda con la contraseña y la borra al terminar */
        s->clave_pend = NULL;
        op->ruta = g_strdup(s->volver_a ? s->volver_a : s->c_carpeta);
        op->recordar = s->recordar;
        op->nombre_srv = g_strdup(s->c_nombre);
        s->recordar = FALSE;
        estado(s, "Autenticando…");
        aviso_mostrar(s, "Iniciando sesión…");
    }
    lanzar(op);
}

/* Conecta la pestaña con sus datos guardados (c_*). Sirve también para reconectar. */
static void sesion_conectar(Sesion *s)
{
    App *a = s->app;
    if (s->ocupado) { estado(s, "Espera a que termine la operación en curso"); return; }
    if (s->con) desconectar(s);
    char *usuario, *host;
    int puerto;
    if (!destino_parsear(s->c_servidor, &usuario, &host, &puerto)) {
        estado(s, "Escribe el servidor (IP, nombre o alias de ~/.ssh/config)");
        return;
    }
    /* lo escrito en "usuario@servidor:puerto" manda sobre los campos sueltos */
    if (!usuario && s->c_usuario) usuario = g_strdup(s->c_usuario);
    if (!puerto && s->c_puerto) puerto = atoi(s->c_puerto);
    /* servidor de salto: se valida antes de crear nada */
    char *su = NULL, *sh = NULL;
    int sp = 0;
    if (s->c_salto && !destino_parsear(s->c_salto, &su, &sh, &sp)) {
        estado(s, "El servidor de salto \"%s\" no es válido (usa usuario@servidor:puerto o un alias)", s->c_salto);
        g_free(usuario); g_free(host);
        return;
    }
    char *llave = expandir_home(s->c_llave ? s->c_llave : a->op_identidad);
    s->con = con_nueva(host, puerto, usuario, llave, a->op_known);
    con_preguntas(s->con, preguntas_destino, s);   /* OTP, aprobación en el navegador… */
    g_clear_pointer(&s->desc_salto, g_free);
    Op *op = op_nueva(s, OP_CONECTAR);
    char *det;
    if (sh) {
        /* primero el salto: con su propia llave, o con ssh-agent y las llaves de ~/.ssh */
        char *ls = expandir_home(s->c_llave_salto ? s->c_llave_salto : a->op_identidad);
        Conexion *j = con_nueva(sh, sp, su, ls, a->op_known);
        con_preguntas(j, preguntas_salto, s);
        con_usar_salto(s->con, j);
        g_free(ls);
        op->salto = TRUE;
        estado(s, "Conectando a %s a través de %s…", host, sh);
        det = g_strdup_printf("Estableciendo conexión con el servidor de salto %s…", sh);
    } else {
        estado(s, "Conectando a %s…", host);
        det = puerto ? g_strdup_printf("Estableciendo conexión con %s, puerto %d…", host, puerto)
                     : g_strdup_printf("Estableciendo conexión con %s…", host);
    }
    g_free(llave);
    g_free(usuario);
    g_free(host);
    g_free(su);
    g_free(sh);
    lanzar(op);
    aviso_mostrar(s, det);
    g_free(det);
    /* pruebas: capturar el aviso, o cancelar a mitad de la conexión */
    if (g_strcmp0(g_getenv("SFTPC_DEMO"), "conectando") == 0) programar_captura(a, 4500);
    if (g_strcmp0(g_getenv("SFTPC_DEMO"), "cancelar") == 0) {
        g_timeout_add(1500, demo_cancelar, a);
        programar_captura(a, 2500);
    }
}

/* Botón Conectar / Enter en la barra: actúa sobre la pestaña visible */
static void conectar_barra(App *a)
{
    Sesion *s = a->s;
    if (s->op_con) { cancelar_conexion(s); return; }   /* el botón dice "Cancelar" */
    if (s->ocupado) { estado(s, "Espera a que termine la operación en curso"); return; }
    if (s->con) {
        g_clear_pointer(&s->perdida, g_free);
        desconectar(s);
        estado(s, "Desconectado");
        return;
    }
    char *u, *h;
    int pt;
    if (!destino_parsear(texto_de(a->servidor), &u, &h, &pt)) {
        estado(s, "Escribe el servidor (IP, nombre o alias de ~/.ssh/config)");
        gtk_widget_grab_focus(a->servidor);
        return;
    }
    g_free(u); g_free(h);
    const char *ptxt = texto_de(a->puerto);
    if (*ptxt && (atoi(ptxt) <= 0 || atoi(ptxt) > 65535)) {
        estado(s, "El puerto debe ser un número entre 1 y 65535");
        return;
    }
    g_free(s->c_servidor); s->c_servidor = g_strstrip(g_strdup(texto_de(a->servidor)));
    g_free(s->c_puerto);   s->c_puerto = dup_o_null(ptxt);
    g_free(s->c_usuario);  s->c_usuario = dup_o_null(texto_de(a->usuario));
    g_free(s->c_llave);    s->c_llave = g_strdup(a->sel_llave);
    g_free(s->c_carpeta);  s->c_carpeta = g_strdup(a->sel_carpeta);
    g_free(s->c_nombre);   s->c_nombre = g_strdup(a->srv_nombre);
    g_free(s->c_salto);    s->c_salto = g_strdup(a->sel_salto ? a->sel_salto : a->op_salto);
    g_free(s->c_llave_salto); s->c_llave_salto = g_strdup(a->sel_llave_salto);
    g_clear_pointer(&s->volver_a, g_free);
    g_clear_pointer(&s->perdida, g_free);
    const char *c = texto_de(a->clave);
    if (*c) {
        if (s->clave_pend) { memset(s->clave_pend, 0, strlen(s->clave_pend)); g_free(s->clave_pend); }
        s->clave_pend = g_strdup(c);
        gtk_editable_set_text(GTK_EDITABLE(a->clave), "");
    }
    sesion_conectar(s);
}

static void on_conectar(GtkWidget *w, gpointer d) { (void)w; conectar_barra(d); }

/* Ventana "Contraseña" cuando el servidor la pide y no se escribió (o falló) */
typedef struct { Sesion *s; GtkWidget *win, *clave, *recordar; gboolean salto; } PedidoClave;

static void pedido_clave_free(gpointer d)
{
    PedidoClave *p = d;
    sesion_unref(p->s);
    g_free(p);
}

static void clave_ok(GtkWidget *w, gpointer d)
{
    (void)w;
    PedidoClave *p = d;
    Sesion *s = p->s;
    const char *c = texto_de(p->clave);
    if (!*c) return;
    if (!s->cerrada) {
        /* la del salto y la del destino se guardan aparte: se vuelve a conectar desde el principio */
        char **dest = p->salto ? &s->clave_salto_pend : &s->clave_pend;
        if (*dest) { memset(*dest, 0, strlen(*dest)); g_free(*dest); }
        *dest = g_strdup(c);
        gboolean rec = gtk_check_button_get_active(GTK_CHECK_BUTTON(p->recordar));
        if (p->salto) s->recordar_salto = rec;
        else s->recordar = rec;
        sesion_ref(s);
        gtk_window_destroy(GTK_WINDOW(p->win));   /* libera p */
        sesion_conectar(s);
        sesion_unref(s);
    } else {
        gtk_window_destroy(GTK_WINDOW(p->win));
    }
}

static void clave_cancelar(GtkWidget *w, gpointer d)
{
    (void)w;
    gtk_window_destroy(GTK_WINDOW(((PedidoClave *)d)->win));
}

static void pedir_clave(Sesion *s, const char *desc, const char *motivo, gboolean salto)
{
    App *a = s->app;
    PedidoClave *p = g_new0(PedidoClave, 1);
    p->s = sesion_ref(s);
    p->salto = salto;
    p->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(p->win), salto ? "Contraseña del servidor de salto" : "Contraseña");
    gtk_window_set_transient_for(GTK_WINDOW(p->win), GTK_WINDOW(a->win));
    gtk_window_set_modal(GTK_WINDOW(p->win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(p->win), 440, -1);
    g_object_set_data_full(G_OBJECT(p->win), "pedido", p, pedido_clave_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    margenes(box, 16);
    GtkWidget *titulo = gtk_label_new(motivo);
    gtk_widget_add_css_class(titulo, "titulo-panel");
    gtk_label_set_xalign(GTK_LABEL(titulo), 0);
    gtk_label_set_wrap(GTK_LABEL(titulo), TRUE);
    char *t = g_strdup_printf(salto ? "Contraseña del servidor de salto %s" : "Contraseña para %s",
                              desc ? desc : "");
    GtkWidget *sub = gtk_label_new(t);
    g_free(t);
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_widget_add_css_class(sub, "dim-label");
    p->clave = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(p->clave), TRUE);
    g_signal_connect(p->clave, "activate", G_CALLBACK(clave_ok), p);
    p->recordar = gtk_check_button_new_with_label("Recordar en el llavero del sistema (cifrada)");
    /* marcada por defecto si es un servidor guardado */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(p->recordar), s->c_nombre != NULL);
    GtkWidget *botones = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(botones, GTK_ALIGN_END);
    GtkWidget *cancelar = gtk_button_new_with_label("Cancelar");
    GtkWidget *aceptar = gtk_button_new_with_label("Conectar");
    gtk_widget_add_css_class(aceptar, "suggested-action");
    g_signal_connect(cancelar, "clicked", G_CALLBACK(clave_cancelar), p);
    g_signal_connect(aceptar, "clicked", G_CALLBACK(clave_ok), p);
    gtk_box_append(GTK_BOX(botones), cancelar);
    gtk_box_append(GTK_BOX(botones), aceptar);
    gtk_box_append(GTK_BOX(box), titulo);
    gtk_box_append(GTK_BOX(box), sub);
    gtk_box_append(GTK_BOX(box), p->clave);
    gtk_box_append(GTK_BOX(box), p->recordar);
    gtk_box_append(GTK_BOX(box), botones);
    gtk_window_set_child(GTK_WINDOW(p->win), box);
    cerrar_con_escape(p->win);
    gtk_window_present(GTK_WINDOW(p->win));
    gtk_widget_grab_focus(p->clave);
    const char *demo = g_getenv("SFTPC_DEMO");
    if (g_strcmp0(demo, "clave") == 0 || g_strcmp0(demo, "clave-salto") == 0) a->captura_widget = p->win;
    if (demo) g_print("VENTANA CLAVE\n");   /* pruebas: se abrió la ventana de contraseña */
}

static void host_respuesta(GObject *src, GAsyncResult *res, gpointer d)
{
    Sesion *s = d;
    int r = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    if (!s->cerrada) {
        if (r == 1 && s->con) lanzar_auth(s, TRUE, s->preg_salto);
        else { desconectar(s); estado(s, "Conexión cancelada"); }
    }
    sesion_unref(s);
}

static void preguntar_host(Sesion *s, const char *huella, gboolean salto)
{
    s->preg_salto = salto;
    char *det = g_strdup_printf("Es la primera vez que te conectas a este %s.\n\n"
                                "Huella de su llave:\n%s\n\n"
                                "Si confías en él, se guarda en ~/.ssh/known_hosts.",
                                salto ? "servidor de salto" : "servidor", huella);
    GtkAlertDialog *d = gtk_alert_dialog_new(salto ? "Servidor de salto desconocido" : "Servidor desconocido");
    gtk_alert_dialog_set_detail(d, det);
    const char *botones[] = { "Cancelar", "Confiar y conectar", NULL };
    gtk_alert_dialog_set_buttons(d, botones);
    gtk_alert_dialog_set_cancel_button(d, 0);
    gtk_alert_dialog_set_default_button(d, 1);
    gtk_alert_dialog_choose(d, GTK_WINDOW(s->app->win), NULL, host_respuesta, sesion_ref(s));
    g_object_unref(d);
    g_free(det);
}

/* ---------- preguntas del servidor: OTP, aprobación en el navegador ---------- */

/* Lo que espera el hilo de trabajo: vive en su pila mientras espera la respuesta */
typedef struct {
    GMutex m;
    GCond cond;
    gboolean listo, ok, salto;
    Sesion *s;
    const char *nombre, *instr;
    int n;
    const char * const *prompts;
    const gboolean *eco;
    char **resp;
} Pregunta;

struct DlgPregunta {
    Pregunta  *p;
    Sesion    *s;
    GtkWidget *win;
    GPtrArray *campos;    /* un campo por pregunta, o NULL si solo hay que confirmar */
    gboolean   hecho;
};

/* Devuelve la respuesta al hilo de trabajo (una sola vez). Después, p ya no es nuestro. */
static void pregunta_responder(DlgPregunta *d, gboolean ok)
{
    if (d->hecho) return;
    d->hecho = TRUE;
    if (d->s->dlg_pregunta == d) d->s->dlg_pregunta = NULL;
    Pregunta *p = d->p;
    if (ok)
        for (guint i = 0; i < d->campos->len; i++) {
            GtkWidget *e = g_ptr_array_index(d->campos, i);
            p->resp[i] = g_strdup(e ? texto_de(e) : "");
        }
    g_mutex_lock(&p->m);
    p->ok = ok;
    p->listo = TRUE;
    g_cond_signal(&p->cond);
    g_mutex_unlock(&p->m);
}

static void dlg_pregunta_free(gpointer x)
{
    DlgPregunta *d = x;
    pregunta_responder(d, FALSE);   /* cerrar la ventana = cancelar */
    sesion_unref(d->s);
    g_ptr_array_unref(d->campos);
    g_free(d);
}

static void pregunta_cerrar(DlgPregunta *d) { gtk_window_destroy(GTK_WINDOW(d->win)); }

static void pregunta_continuar(GtkWidget *w, gpointer x)
{
    (void)w;
    DlgPregunta *d = x;
    pregunta_responder(d, TRUE);
    gtk_window_destroy(GTK_WINDOW(d->win));
}

static void pregunta_cancelar(GtkWidget *w, gpointer x) { (void)w; pregunta_cerrar(x); }

static void abrir_enlace(GtkWidget *w, gpointer x)
{
    GtkUriLauncher *l = gtk_uri_launcher_new(x);
    gtk_uri_launcher_launch(l, GTK_WINDOW(gtk_widget_get_root(w)), NULL, NULL, NULL);
    g_object_unref(l);
}

/* Texto del servidor como markup: escapado, sin las líneas de guiones decorativas
 * y con los enlaces clicables. *primer_url queda con el primer enlace (o NULL). */
static char *texto_con_enlaces(const char *t, char **primer_url)
{
    *primer_url = NULL;
    GString *limpio = g_string_new(NULL);
    char **lineas = g_strsplit(t, "\n", -1);
    for (char **l = lineas; *l; l++) {
        char *x = g_strstrip(g_strdup(*l));
        if (*x && strspn(x, "-=_*") == strlen(x)) { g_free(x); continue; }   /* "-----" */
        if (*x || (limpio->len && limpio->str[limpio->len - 1] != '\n')) {
            g_string_append(limpio, x);
            g_string_append_c(limpio, '\n');
        }
        g_free(x);
    }
    g_strfreev(lineas);
    while (limpio->len && limpio->str[limpio->len - 1] == '\n') g_string_truncate(limpio, limpio->len - 1);

    GString *out = g_string_new(NULL);
    GRegex *re = g_regex_new("https?://[^\\s<>\"]+", 0, 0, NULL);
    GMatchInfo *mi;
    int desde = 0;
    g_regex_match(re, limpio->str, 0, &mi);
    while (g_match_info_matches(mi)) {
        int a, b;
        g_match_info_fetch_pos(mi, 0, &a, &b);
        char *antes = g_markup_escape_text(limpio->str + desde, a - desde);
        char *url = g_strndup(limpio->str + a, b - a);
        char *ue = g_markup_escape_text(url, -1);
        g_string_append_printf(out, "%s<a href=\"%s\">%s</a>", antes, ue, ue);
        if (!*primer_url) *primer_url = g_strdup(url);
        g_free(antes); g_free(url); g_free(ue);
        desde = b;
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(re);
    char *resto = g_markup_escape_text(limpio->str + desde, -1);
    g_string_append(out, resto);
    g_free(resto);
    g_string_free(limpio, TRUE);
    return g_string_free(out, FALSE);
}

static gboolean contiene_ci(const char *t, const char *que)
{
    char *x = g_utf8_strdown(t ? t : "", -1);
    gboolean r = strstr(x, que) != NULL;
    g_free(x);
    return r;
}

static gboolean demo_responder(gpointer x)
{
    DlgPregunta *d = x;
    const char *r = g_getenv("SFTPC_RESPUESTA");
    for (guint i = 0; i < d->campos->len; i++) {
        GtkWidget *e = g_ptr_array_index(d->campos, i);
        if (e) gtk_editable_set_text(GTK_EDITABLE(e), r ? r : "");
    }
    pregunta_continuar(NULL, d);
    return G_SOURCE_REMOVE;
}

/* En el hilo principal: arma la ventana con lo que pregunta el servidor */
static gboolean pregunta_mostrar(gpointer x)
{
    Pregunta *p = x;
    Sesion *s = p->s;
    App *a = s->app;
    if (s->cerrada || !s->op_con) {   /* conexión cancelada mientras tanto */
        g_mutex_lock(&p->m);
        p->ok = FALSE;
        p->listo = TRUE;
        g_cond_signal(&p->cond);
        g_mutex_unlock(&p->m);
        return G_SOURCE_REMOVE;
    }
    DlgPregunta *d = g_new0(DlgPregunta, 1);
    d->p = p;
    d->s = sesion_ref(s);
    d->campos = g_ptr_array_new();
    d->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(d->win), "Inicio de sesión");
    gtk_window_set_transient_for(GTK_WINDOW(d->win), GTK_WINDOW(a->win));
    gtk_window_set_modal(GTK_WINDOW(d->win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(d->win), 480, -1);
    g_object_set_data_full(G_OBJECT(d->win), "dlg", d, dlg_pregunta_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    margenes(box, 16);
    const char *nom = p->nombre && *p->nombre ? p->nombre : "El servidor pide más datos";
    char *t = p->salto ? g_strdup_printf("Servidor de salto: %s", nom) : g_strdup(nom);
    GtkWidget *titulo = gtk_label_new(t);
    g_free(t);
    gtk_widget_add_css_class(titulo, "titulo-panel");
    gtk_label_set_xalign(GTK_LABEL(titulo), 0);
    gtk_label_set_wrap(GTK_LABEL(titulo), TRUE);
    gtk_box_append(GTK_BOX(box), titulo);

    char *url = NULL;
    GtkWidget *ins = NULL;
    if (p->instr && *p->instr) {
        char *mk = texto_con_enlaces(p->instr, &url);
        ins = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(ins), mk);
        g_free(mk);
        gtk_label_set_xalign(GTK_LABEL(ins), 0);
        gtk_label_set_wrap(GTK_LABEL(ins), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(ins), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(ins), 60);
        gtk_label_set_selectable(GTK_LABEL(ins), TRUE);   /* para copiar el código o el enlace */
        gtk_box_append(GTK_BOX(box), ins);
    }
    if (url) {   /* aprobación en el navegador: botón directo */
        GtkWidget *b = gtk_button_new_with_label("Abrir en el navegador");
        gtk_widget_set_halign(b, GTK_ALIGN_START);
        g_object_set_data_full(G_OBJECT(b), "url", url, g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(abrir_enlace), url);
        gtk_box_append(GTK_BOX(box), b);
    }

    GtkWidget *primero = NULL;
    GString *log = g_string_new(NULL);
    for (int i = 0; i < p->n; i++) {
        char *pr = g_strstrip(g_strdup(p->prompts[i]));
        g_string_append_printf(log, "%s%s", i ? " / " : "", pr);
        GtkWidget *l = gtk_label_new(pr);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_box_append(GTK_BOX(box), l);
        GtkWidget *e = NULL;
        if (contiene_ci(pr, "press enter") || contiene_ci(pr, "pulsa enter") || contiene_ci(pr, "presiona enter")) {
            /* solo hay que confirmar: el botón Continuar hace de "Enter" */
            gtk_label_set_text(GTK_LABEL(l), url ? "Cuando lo apruebes en el navegador, pulsa Continuar."
                                                 : "Pulsa Continuar para seguir.");
        } else if (p->eco[i]) {
            e = gtk_entry_new();
            if (contiene_ci(pr, "one-time") || contiene_ci(pr, "otp") || contiene_ci(pr, "code") ||
                contiene_ci(pr, "código"))
                gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_DIGITS);
        } else {
            e = gtk_password_entry_new();
            gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(e), TRUE);
        }
        if (e) {
            g_signal_connect(e, "activate", G_CALLBACK(pregunta_continuar), d);
            gtk_box_append(GTK_BOX(box), e);
            if (!primero) primero = e;
        }
        g_ptr_array_add(d->campos, e);
        g_free(pr);
    }

    GtkWidget *botones = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(botones, GTK_ALIGN_END);
    GtkWidget *cancelar = gtk_button_new_with_label("Cancelar");
    GtkWidget *seguir = gtk_button_new_with_label("Continuar");
    gtk_widget_add_css_class(seguir, "suggested-action");
    g_signal_connect(cancelar, "clicked", G_CALLBACK(pregunta_cancelar), d);
    g_signal_connect(seguir, "clicked", G_CALLBACK(pregunta_continuar), d);
    gtk_box_append(GTK_BOX(botones), cancelar);
    gtk_box_append(GTK_BOX(botones), seguir);
    gtk_box_append(GTK_BOX(box), botones);
    gtk_window_set_child(GTK_WINDOW(d->win), box);
    cerrar_con_escape(d->win);
    gtk_window_set_default_widget(GTK_WINDOW(d->win), seguir);
    gtk_window_present(GTK_WINDOW(d->win));
    gtk_widget_grab_focus(primero ? primero : seguir);
    if (ins) gtk_label_select_region(GTK_LABEL(ins), 0, 0);   /* un label seleccionable abre todo marcado */
    s->dlg_pregunta = d;
    aviso_mostrar(s, "Esperando tu respuesta en la ventana de inicio de sesión…");

    /* pruebas: mostrar qué se preguntó; "otp" contesta solo, "preguntas" captura la ventana */
    const char *demo = g_getenv("SFTPC_DEMO");
    if (demo) g_print("PREGUNTA: %s | %s%s\n", nom, log->str, url ? " | con enlace" : "");
    if (g_strcmp0(demo, "otp") == 0) g_timeout_add(300, demo_responder, d);
    if (g_strcmp0(demo, "preguntas") == 0) {
        a->captura_widget = d->win;
        programar_captura(a, 1000);
    }
    g_string_free(log, TRUE);
    return G_SOURCE_REMOVE;
}

/* En el hilo de trabajo: pasa la pregunta a la interfaz y espera la respuesta */
static gboolean preguntas_ui(const char *nombre, const char *instr, int n, const char * const *prompts,
                             const gboolean *eco, char **resp, Sesion *s, gboolean salto)
{
    Pregunta p = { 0 };
    g_mutex_init(&p.m);
    g_cond_init(&p.cond);
    p.s = s; p.salto = salto; p.nombre = nombre; p.instr = instr;
    p.n = n; p.prompts = prompts; p.eco = eco; p.resp = resp;
    g_idle_add(pregunta_mostrar, &p);
    g_mutex_lock(&p.m);
    while (!p.listo) g_cond_wait(&p.cond, &p.m);
    g_mutex_unlock(&p.m);
    g_cond_clear(&p.cond);
    g_mutex_clear(&p.m);
    return p.ok;
}

static gboolean preguntas_destino(const char *nombre, const char *instr, int n, const char * const *prompts,
                                  const gboolean *eco, char **resp, gpointer data)
{
    return preguntas_ui(nombre, instr, n, prompts, eco, resp, data, FALSE);
}

static gboolean preguntas_salto(const char *nombre, const char *instr, int n, const char * const *prompts,
                                const gboolean *eco, char **resp, gpointer data)
{
    return preguntas_ui(nombre, instr, n, prompts, eco, resp, data, TRUE);
}

/* ---------- conexión perdida ---------- */

/* Se cortó la red o el servidor cerró la conexión: limpiar y avisar */
static void conexion_perdida(Sesion *s)
{
    if (s->cerrada) return;
    char *desc = g_strdup(s->desc ? s->desc : s->c_servidor);
    if (s->der.cwd) { g_free(s->volver_a); s->volver_a = g_strdup(s->der.cwd); }
    g_free(s->perdida);
    s->perdida = desc;
    desconectar(s);
    estado(s, "Se perdió la conexión con %s", desc);
    /* el diálogo sale ahora si la pestaña está a la vista, o al volver a ella */
    if (s == s->app->s) dialogo_perdida(s);
    else s->perdida_pendiente = TRUE;
}

typedef struct { Sesion *s; GtkWidget *win; } DlgPerdida;

static void dlg_perdida_free(gpointer d)
{
    DlgPerdida *x = d;
    x->s->dialogo_abierto = FALSE;
    sesion_unref(x->s);
    g_free(x);
}

static void perdida_reconectar(GtkButton *b, gpointer d)
{
    (void)b;
    DlgPerdida *x = d;
    Sesion *s = sesion_ref(x->s);
    gtk_window_destroy(GTK_WINDOW(x->win));   /* libera x */
    if (!s->cerrada) sesion_conectar(s);
    sesion_unref(s);
}

static void perdida_cerrar(GtkButton *b, gpointer d)
{
    (void)b;
    DlgPerdida *x = d;
    Sesion *s = sesion_ref(x->s);
    gtk_window_destroy(GTK_WINDOW(x->win));
    if (!s->cerrada) cerrar_sesion(s);
    sesion_unref(s);
}

static void perdida_despues(GtkButton *b, gpointer d)
{
    (void)b;
    gtk_window_destroy(GTK_WINDOW(((DlgPerdida *)d)->win));
}

static gboolean demo_reconectar(gpointer d)
{
    DlgPerdida *x = d;
    perdida_reconectar(NULL, x);
    return G_SOURCE_REMOVE;
}

static void dialogo_perdida(Sesion *s)
{
    App *a = s->app;
    s->perdida_pendiente = FALSE;
    if (s->dialogo_abierto) return;
    s->dialogo_abierto = TRUE;
    DlgPerdida *x = g_new0(DlgPerdida, 1);
    x->s = sesion_ref(s);
    x->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(x->win), "Conexión perdida");
    gtk_window_set_transient_for(GTK_WINDOW(x->win), GTK_WINDOW(a->win));
    gtk_window_set_modal(GTK_WINDOW(x->win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(x->win), 440, -1);
    g_object_set_data_full(G_OBJECT(x->win), "dlg", x, dlg_perdida_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    margenes(box, 16);
    char *t = g_strdup_printf("Se perdió la conexión con %s", s->perdida ? s->perdida : "el servidor");
    GtkWidget *titulo = gtk_label_new(t);
    g_free(t);
    gtk_widget_add_css_class(titulo, "titulo-panel");
    gtk_label_set_xalign(GTK_LABEL(titulo), 0);
    gtk_label_set_wrap(GTK_LABEL(titulo), TRUE);
    GtkWidget *sub = gtk_label_new(s->volver_a
        ? "¿Quieres reconectar? Se vuelve a abrir la misma carpeta remota."
        : "¿Quieres reconectar o cerrar esta pestaña?");
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_label_set_wrap(GTK_LABEL(sub), TRUE);
    gtk_widget_add_css_class(sub, "dim-label");
    GtkWidget *botones = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(botones, GTK_ALIGN_END);
    GtkWidget *b_cerrar = gtk_button_new_with_label("Cerrar pestaña");
    GtkWidget *b_luego = gtk_button_new_with_label("Ahora no");
    GtkWidget *b_recon = gtk_button_new_with_label("Reconectar");
    gtk_widget_add_css_class(b_cerrar, "destructive-action");
    gtk_widget_add_css_class(b_recon, "suggested-action");
    g_signal_connect(b_cerrar, "clicked", G_CALLBACK(perdida_cerrar), x);
    g_signal_connect(b_luego, "clicked", G_CALLBACK(perdida_despues), x);
    g_signal_connect(b_recon, "clicked", G_CALLBACK(perdida_reconectar), x);
    gtk_box_append(GTK_BOX(botones), b_cerrar);
    gtk_box_append(GTK_BOX(botones), b_luego);
    gtk_box_append(GTK_BOX(botones), b_recon);
    gtk_box_append(GTK_BOX(box), titulo);
    gtk_box_append(GTK_BOX(box), sub);
    gtk_box_append(GTK_BOX(box), botones);
    gtk_window_set_child(GTK_WINDOW(x->win), box);
    cerrar_con_escape(x->win);
    gtk_window_set_default_widget(GTK_WINDOW(x->win), b_recon);
    gtk_window_present(GTK_WINDOW(x->win));
    gtk_widget_grab_focus(b_recon);
    const char *demo = g_getenv("SFTPC_DEMO");
    if (g_strcmp0(demo, "perdida") == 0) a->captura_widget = x->win;
    if (g_strcmp0(demo, "reconectar") == 0) g_timeout_add(300, demo_reconectar, x);
    g_print("DIALOGO: Se perdió la conexión con %s\n", s->perdida ? s->perdida : "?");
}

/* Al volver a una pestaña: avisar si se perdió, o comprobar que la conexión sigue viva */
static void sesion_verificar(Sesion *s)
{
    if (s->cerrada) return;
    if (s->perdida_pendiente) { dialogo_perdida(s); return; }
    if (s->con && !s->ocupado) lanzar(op_nueva(s, OP_VERIFICAR));
}

static gboolean verificar_idle(gpointer d)
{
    sesion_verificar(d);
    sesion_unref(d);
    return G_SOURCE_REMOVE;
}

/* ---------- pestañas ---------- */

static void programar_captura(App *a, guint ms)
{
    if (a->op_captura && !a->captura_hecha) {
        a->captura_hecha = TRUE;
        g_timeout_add(ms, capturar, a);
    }
}

static Sesion *sesion_de_pagina(GtkWidget *w) { return g_object_get_data(G_OBJECT(w), "sesion"); }

static void on_pestana_cambiada(GtkNotebook *nb, GtkWidget *pagina, guint n, gpointer d)
{
    (void)nb; (void)n;
    App *a = d;
    Sesion *s = sesion_de_pagina(pagina);
    if (!s || s->cerrada) return;
    a->s = s;
    barra_desde_sesion(a, s);
    boton_con_actualizar(a);
    /* comprobar después de mostrarla, no durante el cambio de página */
    g_idle_add(verificar_idle, sesion_ref(s));
}

static void cerrar_sesion(Sesion *s)
{
    App *a = s->app;
    if (s->cerrada) return;
    if (s->op_con) cancelar_conexion(s);
    else if (s->ocupado) { estado(s, "Espera a que termine la operación, o cancélala, antes de cerrar la pestaña"); return; }
    if (s->con) desconectar(s);
    if (s->aviso_reloj) { g_source_remove(s->aviso_reloj); s->aviso_reloj = 0; }
    /* los menús cuelgan de las vistas: soltarlos antes de destruirlas */
    g_clear_pointer(&s->izq.menu, gtk_widget_unparent);
    g_clear_pointer(&s->der.menu, gtk_widget_unparent);
    s->cerrada = TRUE;
    sesion_ref(s);   /* que viva hasta que GTK termine de destruir sus widgets */
    int n = gtk_notebook_page_num(GTK_NOTEBOOK(a->tabs), s->pagina);
    if (a->s == s) a->s = NULL;
    g_ptr_array_remove(a->sesiones, s);   /* suelta la referencia de la lista */
    if (a->sesiones->len == 0) nueva_sesion(a);   /* siempre queda una pestaña */
    if (n >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(a->tabs), n);
    sesion_unref(s);
    if (!a->s) {
        GtkWidget *p = gtk_notebook_get_nth_page(GTK_NOTEBOOK(a->tabs),
                                                 gtk_notebook_get_current_page(GTK_NOTEBOOK(a->tabs)));
        a->s = sesion_de_pagina(p);
        barra_desde_sesion(a, a->s);
        boton_con_actualizar(a);
    }
}

static void on_cerrar_pestana(GtkButton *b, gpointer d) { (void)b; cerrar_sesion(d); }

/* ---------- fin de una operación (hilo principal) ---------- */

static void demo(App *a);
static gboolean demo_cb(gpointer d) { demo(d); return G_SOURCE_REMOVE; }

static void op_terminada(GObject *src, GAsyncResult *res, gpointer datos)
{
    (void)src; (void)res;
    Op *op = datos;
    Sesion *s = op->s;
    App *a = s->app;
    if (op->abandonada || s->cerrada) {   /* conexión cancelada o pestaña cerrada: solo limpiar */
        if (op->abandonada && op->con) con_cerrar(op->con);
        op_free(op);
        return;
    }
    if (s->op_con == op) s->op_con = NULL;
    s->ocupado = FALSE;
    gtk_widget_set_visible(s->btn_cancelar, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->barra), 0);

    /* una operación remota que falló con la conexión caída: avisar para reconectar */
    if (op->err && op->con && op->tipo != OP_CONECTAR && op->tipo != OP_AUTENTICAR &&
        (op->tipo == OP_VERIFICAR || !con_conectada(op->con))) {
        if (op->tipo == OP_ABRIR) local_borrar(op->ruta2, NULL);
        conexion_perdida(s);
        op_free(op);
        return;
    }

    switch (op->tipo) {
    case OP_CONECTAR: {
        const char *quien = op->salto ? "Servidor de salto: " : "";
        if (op->est != HOST_OK || op->err) aviso_ocultar(s);
        if (op->err) {
            estado(s, "%s%s", quien, op->err->message);
            desconectar(s);
            programar_captura(a, 500);
        } else if (op->est == HOST_OK) {
            lanzar_auth(s, FALSE, op->salto);
        } else if (op->est == HOST_CAMBIADO) {
            char *det = g_strdup_printf("La huella actual es:\n%s\n\nPuede ser una reinstalación del servidor "
                                        "o un ataque. No se conectó. Revisa ~/.ssh/known_hosts.", op->huella);
            GtkAlertDialog *d = gtk_alert_dialog_new(op->salto ? "La llave del servidor de salto cambió"
                                                               : "La llave del servidor cambió");
            gtk_alert_dialog_set_detail(d, det);
            gtk_alert_dialog_show(d, GTK_WINDOW(a->win));
            g_object_unref(d);
            g_free(det);
            desconectar(s);
            estado(s, "%sConexión rechazada: la llave del servidor cambió", quien);
        } else {
            estado(s, "%sServidor desconocido: confirma su huella", quien);
            preguntar_host(s, op->huella, op->salto);
            programar_captura(a, 1500);
        }
        break;
    }

    case OP_AUTENTICAR:
        if (op->err) {
            gboolean pedir = g_error_matches(op->err, CON_ERROR, CON_ERR_AUTH);
            gboolean habia_clave = op->clave && *op->clave;
            estado(s, "%s%s", op->salto ? "Servidor de salto: " : "", op->err->message);
            desconectar(s);
            aviso_ocultar(s);
            if (pedir)
                pedir_clave(s, op->desc,
                            op->de_llavero ? "La contraseña guardada en el llavero no sirvió. Escríbela de nuevo."
                            : habia_clave  ? "Contraseña incorrecta. Prueba otra vez."
                            : op->salto    ? "El servidor de salto pide contraseña."
                                           : "El servidor pide contraseña.",
                            op->salto);
        } else if (op->salto) {
            /* ya dentro del salto y con el túnel abierto: ahora el servidor de destino */
            g_free(s->desc_salto);
            s->desc_salto = g_strdup(op->desc);
            if (op->aviso) estado(s, "Servidor de salto: %s", op->aviso);
            char *u, *h, *pt;
            con_datos(s->con, &u, &h, &pt);
            char *det = g_strdup_printf("Conectando con %s a través de %s…", h, op->desc);
            Op *sig = op_nueva(s, OP_CONECTAR);
            lanzar(sig);
            aviso_mostrar(s, det);
            g_free(det); g_free(u); g_free(h); g_free(pt);
            op_free(op);
            return;
        } else {
            char *d = con_descripcion(s->con);
            g_free(s->desc);
            s->desc = s->desc_salto ? g_strdup_printf("%s vía %s", d, s->desc_salto) : g_strdup(d);
            g_free(d);
            g_clear_pointer(&s->perdida, g_free);
            g_clear_pointer(&s->volver_a, g_free);
            char *t = g_strdup_printf("Remoto: %s", s->desc);
            gtk_label_set_text(GTK_LABEL(s->der.titulo), t);
            g_free(t);
            aviso_ocultar(s);
            panel_cargar(&s->der, op->texto, op->lista);
            if (op->aviso) estado(s, "Conectado a %s (%s)", s->desc, op->aviso);
            else if (op->de_llavero) estado(s, "Conectado a %s con la contraseña del llavero", s->desc);
            else estado(s, "Conectado a %s", s->desc);
            if (g_getenv("SFTPC_DEMO") && !a->demo_hecho) {
                a->demo_hecho = TRUE;
                g_timeout_add(300, demo_cb, a);
            }
        }
        {
            const char *m = g_getenv("SFTPC_DEMO");
            guint ms = g_strcmp0(m, "editar") == 0 ? 4000
                     : (g_strcmp0(m, "pestanas") == 0 || g_strcmp0(m, "perdida") == 0 ||
                        g_strcmp0(m, "reconectar") == 0) ? 3000 : 1200;
            programar_captura(a, ms);
        }
        break;

    case OP_VERIFICAR:
        break;   /* sigue viva: nada que hacer */

    case OP_ABRIR:
        if (op->err) {
            estado(s, "%s", op->err->message);
            local_borrar(op->ruta2, NULL);
        } else {
            char *base = g_path_get_basename(op->ruta);
            char *local = g_build_filename(op->ruta2, base, NULL);
            edicion_nueva(s, local, op->ruta);
            estado(s, "Abierto %s. Al guardarlo en el programa, los cambios se suben solos al servidor.", base);
            abrir_con_app(s, local);
            if (g_strcmp0(g_getenv("SFTPC_DEMO"), "editar") == 0) {
                /* la prueba simula al editor guardando el archivo */
                g_file_set_contents(local, "editado por la prueba\n", -1, NULL);
            }
            g_free(local);
            g_free(base);
        }
        break;

    case OP_GUARDAR_EDITADO:
        if (op->err) estado(s, "No se pudo subir %s: %s", op->texto, op->err->message);
        else {
            GDateTime *ahora = g_date_time_new_now_local();
            char *h = g_date_time_format(ahora, "%H:%M:%S");
            estado(s, "Guardado en el servidor: %s (%s)", op->texto, h);
            g_free(h);
            g_date_time_unref(ahora);
            if (s->der.cwd && g_strcmp0(s->der.cwd, op->ruta2) == 0) panel_ir(&s->der, s->der.cwd);
        }
        break;

    case OP_LISTAR:
        if (op->err) {
            estado(s, "%s", op->err->message);
            if (op->p->cwd) gtk_editable_set_text(GTK_EDITABLE(op->p->ruta), op->p->cwd);
        } else {
            panel_cargar(op->p, op->ruta, op->lista);
        }
        break;

    default:
        if (op->err) estado(s, "%s", op->err->message);
        else if (op->tipo == OP_COPIAR) estado(s, "Copia terminada: %u elemento(s)", op->nombres->len);
        else if (op->tipo == OP_NUEVO_DOC) estado(s, "Documento creado");
        else if (op->tipo == OP_MKDIR) estado(s, "Carpeta creada");
        else estado(s, "Listo");
        refrescar(s);
        break;
    }
    op_free(op);
}

/* ---------- señales de los paneles ---------- */

static void on_fila_activada(GtkColumnView *cv, guint pos, gpointer d)
{
    (void)cv;
    Panel *p = d;
    FiItem *it = g_list_model_get_item(G_LIST_MODEL(p->sel), pos);
    if (!it) return;
    if (strcmp(it->nombre, "..") == 0) panel_arriba(p);
    else abrir_item(p, it);   /* carpeta: entra; archivo: lo abre */
    g_object_unref(it);
}

static void on_ruta_activada(GtkEntry *e, gpointer d)
{
    Panel *p = d;
    panel_ir(p, gtk_editable_get_text(GTK_EDITABLE(e)));
}

static void on_foco(GtkEventControllerFocus *c, gpointer d)
{
    (void)c;
    Panel *p = d;
    p->s->activo = p;
    marcar_activo(p->s);
}

/* Clic derecho: selecciona la fila bajo el puntero (si no lo estaba) y abre el menú */
static void on_clic_derecho(GtkGestureClick *gc, int n, double x, double y, gpointer d)
{
    (void)n;
    Panel *p = d;
    Sesion *s = p->s;
    gtk_gesture_set_state(GTK_GESTURE(gc), GTK_EVENT_SEQUENCE_CLAIMED);
    s->activo = p;
    marcar_activo(s);
    GtkSelectionModel *sel = GTK_SELECTION_MODEL(p->sel);
    GtkListItem *li = NULL;
    for (GtkWidget *w = gtk_widget_pick(p->vista, x, y, GTK_PICK_DEFAULT); w && w != p->vista;
         w = gtk_widget_get_parent(w))
        if ((li = g_object_get_data(G_OBJECT(w), "li")) != NULL) break;
    if (li) {
        guint pos = gtk_list_item_get_position(li);
        if (pos != GTK_INVALID_LIST_POSITION && !gtk_selection_model_is_selected(sel, pos))
            gtk_selection_model_select_item(sel, pos, TRUE);
    } else {
        gtk_selection_model_unselect_all(sel);   /* clic en vacío: solo crear/actualizar */
    }
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(p->menu), &r);
    gtk_popover_popup(GTK_POPOVER(p->menu));
}

static void on_cancelar(GtkButton *b, gpointer d)
{
    (void)b;
    Sesion *s = d;
    g_atomic_int_set(&s->cancelar, 1);
    estado(s, "Cancelando…");
}

/* Búsqueda: filtra mientras se escribe */
static void on_buscar_cambio(GtkSearchEntry *e, gpointer d)
{
    Panel *p = d;
    const char *t = texto_de(GTK_WIDGET(e));
    g_free(p->patron);
    p->patron = *t ? g_utf8_casefold(t, -1) : NULL;
    gtk_filter_changed(GTK_FILTER(p->filtro), GTK_FILTER_CHANGE_DIFFERENT);
    if (p->patron) {
        guint n = panel_visibles(p);
        estado(p->s, "%u coincidencia(s) con \"%s\" en %s", n, t, p->cwd ? p->cwd : "");
    }
}

/* Esc: quita la búsqueda y vuelve a la lista */
static void on_buscar_fin(GtkSearchEntry *e, gpointer d)
{
    (void)e;
    Panel *p = d;
    buscar_limpiar(p);
    gtk_widget_grab_focus(p->vista);
}

/* Enter: va a la primera coincidencia */
static void on_buscar_enter(GtkSearchEntry *e, gpointer d)
{
    (void)e;
    Panel *p = d;
    GListModel *m = G_LIST_MODEL(p->sel);
    guint n = g_list_model_get_n_items(m), pos = 0;
    if (n > 1) {
        FiItem *it = g_list_model_get_item(m, 0);
        if (strcmp(it->nombre, "..") == 0) pos = 1;
        g_object_unref(it);
    }
    if (pos < n) {
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(p->sel), pos, TRUE);
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->vista), pos, NULL, GTK_LIST_SCROLL_FOCUS, NULL);
    }
    gtk_widget_grab_focus(p->vista);
}

/* Lupa: abre o cierra la búsqueda de ese panel */
static void on_lupa(GtkButton *b, gpointer d)
{
    (void)b;
    Panel *p = d;
    p->s->activo = p;
    marcar_activo(p->s);
    if (gtk_widget_get_visible(p->buscar)) { buscar_limpiar(p); gtk_widget_grab_focus(p->vista); }
    else accion_buscar(p->s->app);
}

/* ---------- arrastrar y soltar ---------- */

/* Fila (GtkListItem) bajo el punto x,y de la vista, o NULL */
static GtkListItem *fila_en(Panel *p, double x, double y)
{
    for (GtkWidget *w = gtk_widget_pick(p->vista, x, y, GTK_PICK_DEFAULT); w && w != p->vista;
         w = gtk_widget_get_parent(w)) {
        GtkListItem *li = g_object_get_data(G_OBJECT(w), "li");
        if (li) return li;
    }
    return NULL;
}

/* Al presionar el botón izquierdo se guarda la selección: el clic en una fila puede dejar solo esa
 * seleccionada antes de que empiece el arrastre */
static gboolean on_presion(GtkEventControllerLegacy *c, GdkEvent *ev, gpointer d)
{
    (void)c;
    Panel *p = d;
    if (gdk_event_get_event_type(ev) == GDK_BUTTON_PRESS && gdk_button_event_get_button(ev) == GDK_BUTTON_PRIMARY
        && !(gdk_event_get_modifier_state(ev) & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
        GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(p->sel));
        g_clear_pointer(&p->sel_previa, gtk_bitset_unref);
        p->sel_previa = gtk_bitset_copy(bs);   /* copia: el modelo cambia el suyo en el lugar */
        gtk_bitset_unref(bs);
    }
    return FALSE;   /* el clic sigue su curso normal */
}

/* Si se arrastra una fila que era parte de una selección de varias, se recupera la selección completa */
static void arrastre_recuperar(Panel *p, guint pos)
{
    if (!p->sel_previa || gtk_bitset_get_size(p->sel_previa) < 2 || !gtk_bitset_contains(p->sel_previa, pos)) return;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->sel));
    if (gtk_bitset_get_maximum(p->sel_previa) >= n) return;   /* la lista cambió entre medio */
    GtkBitset *todo = gtk_bitset_new_range(0, n);
    gtk_selection_model_set_selection(GTK_SELECTION_MODEL(p->sel), p->sel_previa, todo);
    gtk_bitset_unref(todo);
}

/* Salto por letras: teclear el comienzo de un nombre selecciona el primero que empiece así
 * (si ninguno, el primero que lo contenga). Las letras seguidas (menos de 1 s) se suman;
 * repetir la misma letra pasa al siguiente que empiece con ella, como en el Explorador. */
static gboolean panel_saltar(Panel *p, gunichar c)
{
    gint64 ahora = g_get_monotonic_time();
    if (!p->teclas) p->teclas = g_string_new(NULL);
    if (ahora - p->teclas_t > G_USEC_PER_SEC) g_string_truncate(p->teclas, 0);
    p->teclas_t = ahora;
    g_string_append_unichar(p->teclas, c);
    char *pref = g_utf8_casefold(p->teclas->str, -1);
    GListModel *m = G_LIST_MODEL(p->sel);
    guint n = g_list_model_get_n_items(m), desde = 0;
    /* ¿todas las letras iguales ("mmm")? entonces se busca "m" a partir del actual */
    gboolean repetida = g_utf8_strlen(pref, -1) > 1;
    gunichar c0 = g_utf8_get_char(pref);
    for (const char *q = pref; *q && repetida; q = g_utf8_next_char(q))
        if (g_utf8_get_char(q) != c0) repetida = FALSE;
    if (repetida) {
        *g_utf8_next_char(pref) = '\0';
        GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(p->sel));
        if (!gtk_bitset_is_empty(bs)) desde = gtk_bitset_get_minimum(bs) + 1;
        gtk_bitset_unref(bs);
    }
    guint hallado = GTK_INVALID_LIST_POSITION;
    for (int pasada = 0; pasada < 2 && hallado == GTK_INVALID_LIST_POSITION; pasada++) {
        if (pasada == 1 && repetida) break;   /* "mm" no busca por contenido */
        for (guint i = 0; i < n && hallado == GTK_INVALID_LIST_POSITION; i++) {
            guint k = n ? (desde + i) % n : 0;
            FiItem *it = g_list_model_get_item(m, k);
            if (strcmp(it->nombre, "..") != 0) {
                char *nom = g_utf8_casefold(it->nombre, -1);
                if (pasada == 0 ? g_str_has_prefix(nom, pref) : strstr(nom, pref) != NULL) hallado = k;
                g_free(nom);
            }
            g_object_unref(it);
        }
    }
    if (hallado != GTK_INVALID_LIST_POSITION) {
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->vista), hallado, NULL,
                                  GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT, NULL);
        estado(p->s, "Ir a: %s", p->teclas->str);
    } else {
        estado(p->s, "Ningún nombre coincide con \"%s\" (Ctrl+F busca en toda la carpeta)", p->teclas->str);
    }
    g_free(pref);
    return hallado != GTK_INVALID_LIST_POSITION;
}

static gboolean on_tecla(GtkEventControllerKey *k, guint kv, guint code, GdkModifierType st, gpointer d)
{
    (void)k; (void)code;
    Panel *p = d;
    if (st & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return FALSE;
    gunichar c = gdk_keyval_to_unicode(kv);
    if (!c || !g_unichar_isprint(c)) return FALSE;
    /* el espacio solo cuenta en medio de un nombre ("mis docs") */
    if (c == ' ' && (!p->teclas || p->teclas->len == 0 || g_get_monotonic_time() - p->teclas_t > G_USEC_PER_SEC))
        return FALSE;
    panel_saltar(p, c);
    return TRUE;
}

/* Empieza un arrastre: lleva la selección de este panel (si se arrastra una fila sin seleccionar, solo esa) */
static GdkContentProvider *on_arrastre_preparar(GtkDragSource *src, double x, double y, gpointer d)
{
    (void)src;
    Panel *p = d;
    if (!p->cwd || p->s->ocupado || (p->remoto && !p->s->con)) return NULL;
    GtkListItem *li = fila_en(p, x, y);
    if (!li) return NULL;
    FiItem *it = gtk_list_item_get_item(li);
    guint pos = gtk_list_item_get_position(li);
    if (!it || strcmp(it->nombre, "..") == 0 || pos == GTK_INVALID_LIST_POSITION) return NULL;
    GtkSelectionModel *sel = GTK_SELECTION_MODEL(p->sel);
    arrastre_recuperar(p, pos);
    if (!gtk_selection_model_is_selected(sel, pos)) gtk_selection_model_select_item(sel, pos, TRUE);
    p->s->activo = p;
    marcar_activo(p->s);
    p->s->app->arrastre = p;
    /* adentro de la app basta una marca: el destino lee la selección del panel de origen */
    return gdk_content_provider_new_typed(G_TYPE_STRING, "sftp-commander");
}

static void on_arrastre_fin(GtkDragSource *src, GdkDrag *drag, gboolean borrar, gpointer d)
{
    (void)src; (void)drag; (void)borrar;
    Panel *p = d;
    if (p->s->app->arrastre == p) p->s->app->arrastre = NULL;
}

/* Soltar: desde el otro panel (copia) o archivos del explorador del sistema (solo al panel remoto: sube) */
static gboolean on_soltar(GtkDropTarget *t, const GValue *v, double x, double y, gpointer d)
{
    (void)t; (void)x; (void)y;
    Panel *p = d;
    Sesion *s = p->s;
    App *a = s->app;
    if (G_VALUE_HOLDS(v, G_TYPE_STRING)) {
        Panel *o = a->arrastre;
        if (!o || o->s != s || o == p) return FALSE;   /* sobre sí mismo o desde otra pestaña: nada */
        copiar_iniciar(s, p, panel_seleccion(o), o->cwd, o->remoto);
        return TRUE;
    }
    if (G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST)) {
        if (!p->remoto) { estado(s, "Suelta los archivos en el panel del servidor para subirlos"); return FALSE; }
        GSList *l = gdk_file_list_get_files(g_value_get_boxed(v));
        GPtrArray *rutas = g_ptr_array_new_with_free_func(g_free);
        for (GSList *i = l; i; i = i->next) {
            char *r = g_file_get_path(i->data);
            if (r) g_ptr_array_add(rutas, r);
        }
        g_slist_free(l);
        s->activo = p;
        marcar_activo(s);
        copiar_iniciar(s, p, rutas, NULL, FALSE);
        return TRUE;
    }
    return FALSE;
}

#ifdef _WIN32
static void on_unidad(GtkButton *b, gpointer d)
{
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    panel_ir(d, gtk_button_get_label(b));
}

/* Se arma cada vez que se abre: así aparecen los pendrives recién conectados */
static void unidades_popup(GtkMenuButton *mb, gpointer d)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    DWORD m = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(m & (1u << i))) continue;
        char r[4] = { (char)('A' + i), ':', '\\', 0 };
        GtkWidget *b = gtk_button_new_with_label(r);
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        g_signal_connect(b, "clicked", G_CALLBACK(on_unidad), d);
        gtk_box_append(GTK_BOX(box), b);
    }
    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_menu_button_set_popover(mb, pop);
}
#endif

static GtkWidget *panel_crear(Sesion *s, Panel *p, gboolean remoto)
{
    App *a = s->app;
    p->s = s;
    p->remoto = remoto;
    p->store = g_list_store_new(FI_TYPE_ITEM);
    /* store -> orden (columna elegida) -> filtro (búsqueda) -> selección */
    p->orden = gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(p->store)), NULL);
    p->filtro = gtk_custom_filter_new(filtro_fn, p, NULL);
    GtkFilterListModel *fm = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(p->orden)),
                                                       GTK_FILTER(p->filtro));   /* fm se queda con filtro */
    p->sel = gtk_multi_selection_new(G_LIST_MODEL(fm));
    p->vista = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(p->sel)));
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(p->vista), TRUE);

    static const char *titulos[N_COLS] = { "Nombre", "Tamaño", "Modificado", "Permisos" };
    GtkColumnViewColumn *col_nombre = NULL;
    for (int i = 0; i < N_COLS; i++) {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(col_setup), GINT_TO_POINTER(i));
        g_signal_connect(f, "bind", G_CALLBACK(col_bind), GINT_TO_POINTER(i));
        GtkColumnViewColumn *col = gtk_column_view_column_new(titulos[i], f);
        gtk_column_view_column_set_resizable(col, TRUE);
        /* clic en el título: ordena por esa columna; otro clic invierte el orden */
        GtkSorter *so = GTK_SORTER(gtk_custom_sorter_new(orden_columna, GINT_TO_POINTER(i), NULL));
        gtk_column_view_column_set_sorter(col, so);
        g_object_unref(so);
        if (i == COL_NOMBRE) { gtk_column_view_column_set_expand(col, TRUE); col_nombre = col; }
        gtk_column_view_append_column(GTK_COLUMN_VIEW(p->vista), col);
        g_object_unref(col);
    }
    /* ".." y carpetas primero, después lo que diga la columna */
    GtkMultiSorter *ms = gtk_multi_sorter_new();
    gtk_multi_sorter_append(ms, GTK_SORTER(gtk_custom_sorter_new(orden_carpetas, NULL, NULL)));
    gtk_multi_sorter_append(ms, g_object_ref(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(p->vista))));
    gtk_sort_list_model_set_sorter(p->orden, GTK_SORTER(ms));
    g_object_unref(ms);
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(p->vista), col_nombre, GTK_SORT_ASCENDING);
    g_signal_connect(p->vista, "activate", G_CALLBACK(on_fila_activada), p);

    /* arrastrar desde este panel y soltar en el otro, o soltar archivos del explorador */
    GtkDragSource *ds = gtk_drag_source_new();
    gtk_drag_source_set_actions(ds, GDK_ACTION_COPY);
    g_signal_connect(ds, "prepare", G_CALLBACK(on_arrastre_preparar), p);
    g_signal_connect(ds, "drag-end", G_CALLBACK(on_arrastre_fin), p);
    gtk_widget_add_controller(p->vista, GTK_EVENT_CONTROLLER(ds));
    GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
    gtk_drop_target_set_gtypes(dt, (GType[]){ GDK_TYPE_FILE_LIST, G_TYPE_STRING }, 2);
    g_signal_connect(dt, "drop", G_CALLBACK(on_soltar), p);
    gtk_widget_add_controller(p->vista, GTK_EVENT_CONTROLLER(dt));
    GtkEventController *pres = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(pres, GTK_PHASE_CAPTURE);
    g_signal_connect(pres, "event", G_CALLBACK(on_presion), p);
    gtk_widget_add_controller(p->vista, pres);

    /* teclear letras en el panel salta al archivo que empiece así */
    GtkEventController *letras = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(letras, GTK_PHASE_CAPTURE);
    g_signal_connect(letras, "key-pressed", G_CALLBACK(on_tecla), p);
    gtk_widget_add_controller(p->vista, letras);

    GtkEventController *foco = gtk_event_controller_focus_new();
    g_signal_connect(foco, "enter", G_CALLBACK(on_foco), p);
    gtk_widget_add_controller(p->vista, foco);

    GtkEventController *atajos = gtk_shortcut_controller_new();
    agregar_atajo(atajos, GDK_KEY_BackSpace, atajo_arriba, p);
    agregar_atajo(atajos, GDK_KEY_Delete, atajo_borrar, a);
    gtk_widget_add_controller(p->vista, atajos);

    GtkGesture *clic = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(clic), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(clic), GTK_PHASE_CAPTURE);
    g_signal_connect(clic, "pressed", G_CALLBACK(on_clic_derecho), p);
    gtk_widget_add_controller(p->vista, GTK_EVENT_CONTROLLER(clic));

    p->menu = gtk_popover_menu_new_from_model(a->menu_ctx);
    gtk_widget_set_parent(p->menu, p->vista);
    gtk_popover_set_has_arrow(GTK_POPOVER(p->menu), FALSE);
    gtk_widget_set_halign(p->menu, GTK_ALIGN_START);

    p->titulo = gtk_label_new(remoto ? "Remoto (sin conexión)" : "Local");
    gtk_label_set_xalign(GTK_LABEL(p->titulo), 0);
    gtk_widget_add_css_class(p->titulo, "titulo-panel");
    p->ruta = gtk_entry_new();
    g_signal_connect(p->ruta, "activate", G_CALLBACK(on_ruta_activada), p);
    p->buscar = gtk_search_entry_new();
    g_object_set(p->buscar, "placeholder-text", "Buscar en esta carpeta (admite * y ?)", NULL);
    gtk_widget_set_visible(p->buscar, FALSE);
    g_signal_connect(p->buscar, "search-changed", G_CALLBACK(on_buscar_cambio), p);
    g_signal_connect(p->buscar, "stop-search", G_CALLBACK(on_buscar_fin), p);
    g_signal_connect(p->buscar, "activate", G_CALLBACK(on_buscar_enter), p);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->vista);
    gtk_widget_set_vexpand(scroll, TRUE);

    p->marco = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(p->marco, "panel");
    gtk_box_append(GTK_BOX(p->marco), p->titulo);
    /* fila de la ruta: [Unidad ▾ (Windows, local)] ruta [lupa] */
    GtkWidget *fila = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
#ifdef _WIN32
    if (!remoto) {
        GtkWidget *mb = gtk_menu_button_new();
        gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), "Unidad");
        gtk_widget_set_tooltip_text(mb, "Unidades");
        gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(mb), unidades_popup, p, NULL);
        gtk_box_append(GTK_BOX(fila), mb);
    }
#endif
    gtk_widget_set_hexpand(p->ruta, TRUE);
    gtk_box_append(GTK_BOX(fila), p->ruta);
    p->lupa = gtk_button_new_from_icon_name("system-search-symbolic");
    gtk_widget_set_tooltip_text(p->lupa, "Buscar en esta carpeta (Ctrl+F)");
    g_signal_connect(p->lupa, "clicked", G_CALLBACK(on_lupa), p);
    gtk_box_append(GTK_BOX(fila), p->lupa);
    gtk_box_append(GTK_BOX(p->marco), fila);
    gtk_box_append(GTK_BOX(p->marco), p->buscar);
    gtk_box_append(GTK_BOX(p->marco), scroll);
    return p->marco;
}

/* Los menús cuelgan de las vistas: soltarlos antes de destruir la ventana */
static void on_ventana_destruida(GtkWidget *w, gpointer d)
{
    (void)w;
    App *a = d;
    for (guint i = 0; i < a->sesiones->len; i++) {
        Sesion *s = g_ptr_array_index(a->sesiones, i);
        g_clear_pointer(&s->izq.menu, gtk_widget_unparent);
        g_clear_pointer(&s->der.menu, gtk_widget_unparent);
        if (s->aviso_reloj) { g_source_remove(s->aviso_reloj); s->aviso_reloj = 0; }
        s->cerrada = TRUE;   /* los hilos que vuelvan ya no tocan widgets */
    }
}

static void boton_accion(GtkWidget *caja, const char *texto, const char *accion)
{
    GtkWidget *b = gtk_button_new_with_label(texto);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), accion);
    gtk_widget_set_hexpand(b, TRUE);   /* reparten el ancho sin imponer un mínimo grande */
    gtk_box_append(GTK_BOX(caja), b);
}

/* ---------- captura para pruebas sin pantalla (--captura archivo.png) ---------- */

static gboolean capturar(gpointer d)
{
    App *a = d;
    GtkWidget *w = a->captura_widget ? a->captura_widget : a->win;
    int ancho = gtk_widget_get_width(w), alto = gtk_widget_get_height(w);
    GdkPaintable *pt = gtk_widget_paintable_new(w);
    GtkSnapshot *sn = gtk_snapshot_new();
    gdk_paintable_snapshot(pt, GDK_SNAPSHOT(sn), ancho, alto);
    GskRenderNode *n = gtk_snapshot_free_to_node(sn);
    if (n) {
        graphene_rect_t r = GRAPHENE_RECT_INIT(0, 0, ancho, alto);
        GdkTexture *t = gsk_renderer_render_texture(gtk_native_get_renderer(gtk_widget_get_native(w)), n, &r);
        if (gdk_texture_save_to_png(t, a->op_captura)) g_print("Captura guardada en %s\n", a->op_captura);
        g_object_unref(t);
        gsk_render_node_unref(n);
    }
    g_object_unref(pt);
    g_print("Pestañas: %u\n", a->sesiones->len);
    g_print("Estado: %s\n", gtk_label_get_text(GTK_LABEL(a->s->estado)));
    g_application_quit(G_APPLICATION(a->gapp));
    return G_SOURCE_REMOVE;
}

/* ---------- pruebas con captura de pantalla (SFTPC_DEMO=...) ---------- */

static FiItem *buscar_item(Panel *p, const char *nombre, guint *pos)
{
    GListModel *m = G_LIST_MODEL(p->sel);
    for (guint i = 0; i < g_list_model_get_n_items(m); i++) {
        FiItem *it = g_list_model_get_item(m, i);
        if (strcmp(it->nombre, nombre) == 0) { if (pos) *pos = i; return it; }
        g_object_unref(it);
    }
    return NULL;
}

static gboolean demo_volver_tab1(gpointer d)
{
    App *a = d;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(a->tabs), 0);
    return G_SOURCE_REMOVE;
}

static void demo(App *a)
{
    const char *m = g_getenv("SFTPC_DEMO");
    Sesion *s = a->s;
    if (g_strcmp0(m, "menu") == 0) {
        Panel *p = &s->der;
        s->activo = p;
        marcar_activo(s);
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(p->sel), 2, TRUE);
        GdkRectangle r = { 160, 150, 1, 1 };
        gtk_popover_set_pointing_to(GTK_POPOVER(p->menu), &r);
        gtk_popover_popup(GTK_POPOVER(p->menu));
        a->captura_widget = p->menu;
    } else if (g_strcmp0(m, "servidores") == 0) {
        gestor_abrir(a);
        a->captura_widget = a->gestor_win;
    } else if (g_strcmp0(m, "editar") == 0) {
        /* abre "nota.txt" del panel remoto como si se hubiera hecho doble clic */
        FiItem *it = buscar_item(&s->der, "nota.txt", NULL);
        if (it) { abrir_item(&s->der, it); g_object_unref(it); }
    } else if (g_strcmp0(m, "pestanas") == 0) {
        /* segunda pestaña con otro servidor guardado de la lista */
        const char *otro = g_getenv("SFTPC_PESTANA");
        conectar_a(a, otro ? otro : "Prueba local");
    } else if (g_strcmp0(m, "perdida") == 0 || g_strcmp0(m, "reconectar") == 0) {
        /* abre otra pestaña, corta la red de la primera y vuelve a ella */
        Sesion *primera = s;
        nueva_sesion(a);
        con_cortar(primera->con);
        g_timeout_add(400, demo_volver_tab1, a);
    } else if (g_strcmp0(m, "buscar") == 0) {
        s->activo = &s->der;
        marcar_activo(s);
        accion_buscar(a);
        gtk_editable_set_text(GTK_EDITABLE(s->der.buscar), "log");
        g_print("FILTRO: buscar \"log\"\n");
    } else if (g_strcmp0(m, "copiar-ruta") == 0) {
        guint pos;
        FiItem *it = buscar_item(&s->der, "nota.txt", &pos);
        if (it) {
            s->activo = &s->der;
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(s->der.sel), pos, TRUE);
            g_object_unref(it);
            accion_copiar_ruta(a);
        }
    } else if (g_strcmp0(m, "acerca") == 0) {
        accion_acerca(a);
    } else if (g_strcmp0(m, "ordenar") == 0) {
        /* clic en "Tamaño" dos veces: de mayor a menor; ".." y carpetas siguen arriba */
        GListModel *cols = gtk_column_view_get_columns(GTK_COLUMN_VIEW(s->der.vista));
        GtkColumnViewColumn *c = g_list_model_get_item(cols, COL_TAM);
        gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(s->der.vista), c, GTK_SORT_DESCENDING);
        g_object_unref(c);
        GString *t = g_string_new("ORDEN:");
        GListModel *m2 = G_LIST_MODEL(s->der.sel);
        for (guint i = 0; i < g_list_model_get_n_items(m2); i++) {
            FiItem *it = g_list_model_get_item(m2, i);
            g_string_append_printf(t, " %s", it->nombre);
            g_object_unref(it);
        }
        g_print("%s\n", t->str);
        g_string_free(t, TRUE);
    } else if (g_strcmp0(m, "conflicto") == 0 || g_strcmp0(m, "confirmar") == 0) {
        /* copia nota.txt del remoto a una carpeta local: la misma (ya existe) o una vacía */
        char *tmp = NULL;
        if (g_strcmp0(m, "conflicto") == 0) panel_ir(&s->izq, s->der.cwd);
        else { tmp = g_dir_make_tmp("sftpc-XXXXXX", NULL); panel_ir(&s->izq, tmp); }
        guint pos;
        FiItem *it = buscar_item(&s->der, "nota.txt", &pos);
        if (it) {
            s->activo = &s->der;
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(s->der.sel), pos, TRUE);
            g_object_unref(it);
            accion_copiar(a);
        }
        if (tmp) { g_rmdir(tmp); g_free(tmp); }
    } else if (g_strcmp0(m, "saltar") == 0) {
        /* teclear "no", luego "grande" (por contenido) y "zz" (misma letra) */
        Panel *p = &s->der;
        const char *pruebas[] = { "no", "grande", "zz", NULL };
        GString *t = g_string_new("SALTO:");
        for (int i = 0; pruebas[i]; i++) {
            p->teclas_t = 0;   /* como si hubiera pasado más de 1 s */
            for (const char *q = pruebas[i]; *q; q = g_utf8_next_char(q)) panel_saltar(p, g_utf8_get_char(q));
            GPtrArray *sel = panel_seleccion(p);
            g_string_append_printf(t, " %s=%s", pruebas[i], sel->len ? (char *)g_ptr_array_index(sel, 0) : "-");
            g_ptr_array_unref(sel);
        }
        g_print("%s\n", t->str);
        g_string_free(t, TRUE);
    } else if (g_strcmp0(m, "varios") == 0) {
        /* 3 seleccionados, clic sin Ctrl en uno (queda 1) y arrastre: deben volver los 3 */
        Panel *p = &s->der;
        GtkSelectionModel *sel = GTK_SELECTION_MODEL(p->sel);
        gtk_selection_model_select_range(sel, 1, 3, TRUE);
        g_clear_pointer(&p->sel_previa, gtk_bitset_unref);
        GtkBitset *bs = gtk_selection_model_get_selection(sel);
        p->sel_previa = gtk_bitset_copy(bs);
        gtk_bitset_unref(bs);
        gtk_selection_model_select_item(sel, 2, TRUE);
        arrastre_recuperar(p, 2);
        char *tmp = g_dir_make_tmp("sftpc-XXXXXX", NULL);
        panel_ir(&s->izq, tmp);
        s->activo = p;
        accion_copiar(a);
        g_rmdir(tmp);
        g_free(tmp);
    } else if (g_strcmp0(m, "soltar") == 0) {
        /* como si se soltara un archivo del explorador sobre el panel remoto */
        char *f = g_build_filename(g_get_tmp_dir(), "sftpc-soltar.txt", NULL);
        g_file_set_contents(f, "soltado\n", -1, NULL);
        GFile *gf = g_file_new_for_path(f);
        GSList *l = g_slist_prepend(NULL, gf);
        GdkFileList *fl = gdk_file_list_new_from_list(l);
        GValue v = G_VALUE_INIT;
        g_value_init(&v, GDK_TYPE_FILE_LIST);
        g_value_take_boxed(&v, fl);
        on_soltar(NULL, &v, 0, 0, &s->der);
        g_value_unset(&v);
        g_slist_free(l);
        g_object_unref(gf);
        g_remove(f);
        g_free(f);
    }
}

/* ---------- ventana ---------- */

static const char *CSS =
    ".panel { border: 2px solid transparent; border-radius: 6px; padding: 4px; }"
    ".panel.activo { border-color: #3584e4; }"
    ".titulo-panel { font-weight: bold; }"
    ".aviso-conexion { background: alpha(@theme_bg_color, 0.96); border: 1px solid alpha(currentColor, 0.15);"
    "  border-radius: 12px; padding: 24px 32px; box-shadow: 0 4px 16px alpha(black, 0.25); }"
    ".aviso-titulo { font-size: 1.3em; font-weight: bold; }"
    ".cerrar-pestana { min-width: 20px; min-height: 20px; padding: 0; }";

/* Tarjeta "Conectando…" que se superpone al panel remoto */
static GtkWidget *aviso_crear(Sesion *s, GtkWidget *panel)
{
    GtkWidget *ov = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(ov), panel);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "aviso-conexion");
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    s->aviso_spinner = gtk_spinner_new();
    gtk_widget_set_size_request(s->aviso_spinner, 40, 40);
    GtkWidget *titulo = gtk_label_new("Conectando, espera…");
    gtk_widget_add_css_class(titulo, "aviso-titulo");
    s->aviso_detalle = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(s->aviso_detalle), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(s->aviso_detalle), 40);
    gtk_label_set_justify(GTK_LABEL(s->aviso_detalle), GTK_JUSTIFY_CENTER);
    s->aviso_tiempo = gtk_label_new("");
    gtk_widget_add_css_class(s->aviso_tiempo, "dim-label");
    GtkWidget *cancelar = gtk_button_new_with_label("Cancelar");
    gtk_widget_set_halign(cancelar, GTK_ALIGN_CENTER);
    g_signal_connect(cancelar, "clicked", G_CALLBACK(on_aviso_cancelar), s);
    gtk_box_append(GTK_BOX(box), s->aviso_spinner);
    gtk_box_append(GTK_BOX(box), titulo);
    gtk_box_append(GTK_BOX(box), s->aviso_detalle);
    gtk_box_append(GTK_BOX(box), s->aviso_tiempo);
    gtk_box_append(GTK_BOX(box), cancelar);
    gtk_widget_set_visible(box, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), box);
    s->aviso = box;
    return ov;
}

/* Pestaña nueva: panel local en la carpeta de inicio y remoto sin conexión. Queda a la vista. */
static Sesion *nueva_sesion(App *a)
{
    Sesion *s = g_new0(Sesion, 1);
    s->refs = 1;   /* la de a->sesiones */
    s->app = a;

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), panel_crear(s, &s->izq, FALSE));
    gtk_paned_set_end_child(GTK_PANED(paned), aviso_crear(s, panel_crear(s, &s->der, TRUE)));
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 592);
    gtk_widget_set_vexpand(paned, TRUE);

    /* botones de acciones (actúan sobre la pestaña visible) */
    GtkWidget *acc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    boton_accion(acc, "F2 Renombrar", "win.renombrar");
    boton_accion(acc, "F4 Copiar", "win.copiar");
    boton_accion(acc, "F5 Actualizar", "win.refrescar");
    boton_accion(acc, "F7 Nueva carpeta", "win.mkdir");
    boton_accion(acc, "Mayús+F4 Nuevo documento", "win.nuevo-doc");
    boton_accion(acc, "F8 Borrar", "win.borrar");
    boton_accion(acc, "Ctrl+F Buscar", "win.buscar");

    /* estado propio de la pestaña */
    GtkWidget *eb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    s->estado = gtk_label_new("Listo");
    gtk_label_set_xalign(GTK_LABEL(s->estado), 0);
    gtk_label_set_ellipsize(GTK_LABEL(s->estado), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(s->estado, TRUE);
    s->barra = gtk_progress_bar_new();
    gtk_widget_set_size_request(s->barra, 220, -1);
    gtk_widget_set_valign(s->barra, GTK_ALIGN_CENTER);
    s->btn_cancelar = gtk_button_new_with_label("Cancelar");
    gtk_widget_set_visible(s->btn_cancelar, FALSE);
    g_signal_connect(s->btn_cancelar, "clicked", G_CALLBACK(on_cancelar), s);
    gtk_box_append(GTK_BOX(eb), s->estado);
    gtk_box_append(GTK_BOX(eb), s->barra);
    gtk_box_append(GTK_BOX(eb), s->btn_cancelar);

    s->pagina = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(s->pagina, 6);
    gtk_box_append(GTK_BOX(s->pagina), paned);
    gtk_box_append(GTK_BOX(s->pagina), acc);
    gtk_box_append(GTK_BOX(s->pagina), eb);
    g_object_set_data(G_OBJECT(s->pagina), "sesion", s);

    /* pestaña: texto + botón de cerrar */
    GtkWidget *cab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    s->etiqueta = gtk_label_new("Nueva sesión");
    gtk_label_set_ellipsize(GTK_LABEL(s->etiqueta), PANGO_ELLIPSIZE_END);
    /* con ellipsize, el mínimo sería "…": se fija un ancho mínimo legible */
    gtk_label_set_width_chars(GTK_LABEL(s->etiqueta), 22);
    gtk_label_set_max_width_chars(GTK_LABEL(s->etiqueta), 30);
    GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(x, "flat");
    gtk_widget_add_css_class(x, "cerrar-pestana");
    gtk_widget_set_tooltip_text(x, "Cerrar pestaña (Ctrl+W)");
    g_signal_connect(x, "clicked", G_CALLBACK(on_cerrar_pestana), s);
    gtk_box_append(GTK_BOX(cab), s->etiqueta);
    gtk_box_append(GTK_BOX(cab), x);

    g_ptr_array_add(a->sesiones, s);
    s->activo = &s->izq;
    marcar_activo(s);
    panel_ir(&s->izq, g_get_home_dir());
    int n = gtk_notebook_append_page(GTK_NOTEBOOK(a->tabs), s->pagina, cab);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(a->tabs), s->pagina, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(a->tabs), n);   /* dispara on_pestana_cambiada */
    a->s = s;
    gtk_widget_grab_focus(s->izq.vista);
    return s;
}

static void on_activar(GtkApplication *g, gpointer d)
{
    App *a = d;
    a->gapp = g;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    a->win = gtk_application_window_new(g);
    gtk_window_set_title(GTK_WINDOW(a->win), "SFTP Commander");
    /* ícono propio (share/icons/hicolor/…/apps/sftp-commander); sin instalar, el genérico */
    gtk_window_set_default_icon_name("sftp-commander");
    gtk_window_set_default_size(GTK_WINDOW(a->win), 1200, 720);
    preferencias_cargar(a);
    g_action_map_add_action_entries(G_ACTION_MAP(a->win), ACCIONES, G_N_ELEMENTS(ACCIONES), a);
    g_simple_action_set_state(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(a->win), "confirmar-copia")),
                              g_variant_new_boolean(a->confirmar_copia));
    for (gsize i = 0; i < G_N_ELEMENTS(ATAJOS); i++)
        gtk_application_set_accels_for_action(g, ATAJOS[i].accion, ATAJOS[i].teclas);
    a->menu_ctx = crear_menu_contextual();
    g_signal_connect(a->win, "destroy", G_CALLBACK(on_ventana_destruida), a);

    GtkWidget *raiz = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(raiz, 8); gtk_widget_set_margin_bottom(raiz, 6);
    gtk_widget_set_margin_start(raiz, 8); gtk_widget_set_margin_end(raiz, 8);

    /* barra de conexión: [Servidores ▾] servidor  puerto  usuario  contraseña  [Conectar] [Administrar] */
    GtkWidget *barra = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char *inicial[] = { "Servidores", NULL };
    a->nombres_srv = gtk_string_list_new(inicial);
    a->dd_srv = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(a->nombres_srv)), NULL);
    gtk_widget_set_tooltip_text(a->dd_srv, "Conectar a un servidor guardado (en una pestaña nueva si esta ya está conectada)");
    a->servidor = entrada("Servidor: IP, nombre, alias o usuario@servidor:puerto");
    gtk_widget_set_hexpand(a->servidor, TRUE);
    gtk_editable_set_width_chars(GTK_EDITABLE(a->servidor), 16);
    a->puerto = entrada("Puerto");
    gtk_editable_set_width_chars(GTK_EDITABLE(a->puerto), 6);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(a->puerto), 6);
    gtk_entry_set_input_purpose(GTK_ENTRY(a->puerto), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_tooltip_text(a->puerto, "Puerto SSH (vacío = 22 o el de ~/.ssh/config)");
    a->usuario = entrada("Usuario");
    gtk_editable_set_width_chars(GTK_EDITABLE(a->usuario), 12);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(a->usuario), 12);
    a->clave = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(a->clave), TRUE);
    g_object_set(a->clave, "placeholder-text", "Contraseña (opcional)", NULL);
    gtk_editable_set_width_chars(GTK_EDITABLE(a->clave), 16);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(a->clave), 16);
    a->btn_con = gtk_button_new_with_label("Conectar");
    gtk_widget_add_css_class(a->btn_con, "suggested-action");
    GtkWidget *b_admin = gtk_button_new_with_label("Administrar…");
    gtk_widget_set_tooltip_text(b_admin, "Guardar y editar servidores (Ctrl+L)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b_admin), "win.servidores");
    for (GtkWidget **w = (GtkWidget *[]){ a->servidor, a->puerto, a->usuario, a->clave, NULL }; *w; w++)
        g_signal_connect(*w, "activate", G_CALLBACK(on_conectar), a);
    g_signal_connect(a->btn_con, "clicked", G_CALLBACK(on_conectar), a);
    g_signal_connect(a->dd_srv, "notify::selected", G_CALLBACK(on_servidor_elegido), a);
    g_signal_connect(a->servidor, "changed", G_CALLBACK(on_servidor_editado), a);
    gtk_box_append(GTK_BOX(barra), a->dd_srv);
    gtk_box_append(GTK_BOX(barra), a->servidor);
    gtk_box_append(GTK_BOX(barra), a->puerto);
    gtk_box_append(GTK_BOX(barra), a->usuario);
    gtk_box_append(GTK_BOX(barra), a->clave);
    gtk_box_append(GTK_BOX(barra), a->btn_con);
    gtk_box_append(GTK_BOX(barra), b_admin);
    /* menú ☰: preferencias y "Acerca de" */
    GMenu *mp = g_menu_new(), *mp1 = g_menu_new(), *mp2 = g_menu_new();
    g_menu_append(mp1, "Preguntar antes de copiar", "win.confirmar-copia");
    g_menu_append(mp1, "Mostrar archivos ocultos (Ctrl+H)", "win.ocultos");
    g_menu_append(mp2, "Acerca de SFTP Commander (F1)", "win.acerca");
    g_menu_append_section(mp, NULL, G_MENU_MODEL(mp1));
    g_menu_append_section(mp, NULL, G_MENU_MODEL(mp2));
    GtkWidget *b_menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(b_menu), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(b_menu), G_MENU_MODEL(mp));
    gtk_widget_set_tooltip_text(b_menu, "Menú");
    g_object_unref(mp1); g_object_unref(mp2); g_object_unref(mp);
    gtk_box_append(GTK_BOX(barra), b_menu);

    /* pestañas: una sesión por pestaña, "+" para abrir otra */
    a->tabs = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(a->tabs), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(a->tabs), FALSE);
    gtk_widget_set_vexpand(a->tabs, TRUE);
    GtkWidget *mas = gtk_button_new_from_icon_name("tab-new-symbolic");
    gtk_widget_add_css_class(mas, "flat");
    gtk_widget_set_tooltip_text(mas, "Nueva pestaña (Ctrl+T)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(mas), "win.nueva-pestana");
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(a->tabs), mas, GTK_PACK_END);
    g_signal_connect(a->tabs, "switch-page", G_CALLBACK(on_pestana_cambiada), a);

    gtk_box_append(GTK_BOX(raiz), barra);
    gtk_box_append(GTK_BOX(raiz), a->tabs);
    gtk_window_set_child(GTK_WINDOW(a->win), raiz);

    servidores_cargar(a);
    a->ediciones = g_ptr_array_new_with_free_func(edicion_free);
    a->sesiones = g_ptr_array_new_with_free_func((GDestroyNotify)sesion_unref);
    nueva_sesion(a);
    gtk_window_present(GTK_WINDOW(a->win));
    gtk_widget_grab_focus(a->s->izq.vista);

    if (a->op_destino) {
        /* un nombre de la lista guardada o un destino escrito */
        if (g_key_file_has_group(a->servidores, a->op_destino)) servidor_llenar(a, a->op_destino);
        else gtk_editable_set_text(GTK_EDITABLE(a->servidor), a->op_destino);
        /* solo pruebas: la contraseña de prueba llega por el entorno */
        if (g_getenv("SFTPC_DEMO") && g_getenv("SFTPC_CLAVE"))
            gtk_editable_set_text(GTK_EDITABLE(a->clave), g_getenv("SFTPC_CLAVE"));
        conectar_barra(a);
    }
    if (a->op_captura) g_timeout_add(10000, capturar, a);   /* por si nada la dispara antes */
}

static void uso(void)
{
    g_print("sftp-commander %s\n"
            "Uso: sftp-commander [servidor guardado | usuario@servidor[:puerto] | sftp://… | alias] [-i llave]\n"
            "                    [-J usuario@salto[:puerto]]   (pasar por un servidor de salto, como ssh -J)\n"
            "     sftp-commander --prueba usuario@servidor[:puerto] [-J salto] [-i llave] [--known-hosts f] [--aceptar-host]\n"
            "Opciones de prueba: --known-hosts archivo, --captura archivo.png\n", VERSION);
}

int main(int argc, char **argv)
{
    App *a = g_new0(App, 1);
    gboolean prueba = FALSE, aceptar = FALSE;
    for (int i = 1; i < argc; i++) {
        const char *x = argv[i];
        if (!strcmp(x, "--prueba")) prueba = TRUE;
        else if (!strcmp(x, "--aceptar-host")) aceptar = TRUE;
        else if (!strcmp(x, "-i") && i + 1 < argc) a->op_identidad = argv[++i];
        else if (!strcmp(x, "-J") && i + 1 < argc) a->op_salto = argv[++i];
        else if (!strcmp(x, "--known-hosts") && i + 1 < argc) a->op_known = argv[++i];
        else if (!strcmp(x, "--captura") && i + 1 < argc) a->op_captura = argv[++i];
        else if (!strcmp(x, "--version")) { g_print("sftp-commander %s\n", VERSION); return 0; }
        else if (!strcmp(x, "--que-app") && i + 1 < argc) {   /* diagnóstico: qué programa abriría */
            char *ct = tipo_de(argv[i + 1]);
            GAppInfo *app = elegir_app(argv[i + 1]);
            g_print("%s  [%s]  ->  %s\n", argv[i + 1], ct, app ? g_app_info_get_id(app) : "(ninguno)");
            g_free(ct);
            g_clear_object(&app);
            return 0;
        }
        else if (x[0] != '-') a->op_destino = argv[i];
        else { uso(); return 2; }
    }
    con_init();
    /* escribir en una conexión cortada no debe matar el programa (SIGPIPE) */
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    if (prueba) return prueba_ejecutar(a->op_destino, a->op_identidad, a->op_known, aceptar, a->op_salto);

    GtkApplication *g = gtk_application_new("io.github.sftpcommander", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(g, "activate", G_CALLBACK(on_activar), a);
    char *args[] = { argv[0], NULL };
    int st = g_application_run(G_APPLICATION(g), 1, args);
    if (a->ediciones) g_ptr_array_unref(a->ediciones);   /* borra las copias temporales */
    if (a->sesiones) g_ptr_array_unref(a->sesiones);     /* cierra las conexiones */
    g_object_unref(g);
    return st;
}
