#include "llavero.h"
#include <string.h>

#ifdef _WIN32
/* Windows: Administrador de credenciales (credenciales genéricas, cifradas por el
 * sistema con la cuenta del usuario). Destino "SFTP Commander: usuario@servidor:puerto";
 * el nombre del servidor guardado va en el comentario para poder olvidarla por nombre. */
#include <gio/gio.h>
#include <windows.h>
#include <wincred.h>

static gunichar2 *destino(const char *servidor, const char *puerto, const char *usuario)
{
    char *t = g_strdup_printf("SFTP Commander: %s@%s:%s", usuario, servidor, puerto);
    gunichar2 *w = g_utf8_to_utf16(t, -1, NULL, NULL, NULL);
    g_free(t);
    return w;
}

static void error_win(GError **err, const char *que)
{
    char *m = g_win32_error_message(GetLastError());
    g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", que, m);
    g_free(m);
}

char *llavero_buscar(const char *servidor, const char *puerto, const char *usuario, GError **err)
{
    gunichar2 *w = destino(servidor, puerto, usuario);
    PCREDENTIALW cred = NULL;
    char *clave = NULL;
    if (CredReadW((LPCWSTR)w, CRED_TYPE_GENERIC, 0, &cred)) {
        clave = g_malloc0(cred->CredentialBlobSize + 1);
        memcpy(clave, cred->CredentialBlob, cred->CredentialBlobSize);
        SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
        CredFree(cred);
    } else if (GetLastError() != ERROR_NOT_FOUND) {
        error_win(err, "No se pudo leer el Administrador de credenciales");
    }
    g_free(w);
    return clave;
}

gboolean llavero_guardar(const char *nombre, const char *servidor, const char *puerto,
                         const char *usuario, const char *clave, GError **err)
{
    gunichar2 *w = destino(servidor, puerto, usuario);
    gunichar2 *wu = g_utf8_to_utf16(usuario ? usuario : "", -1, NULL, NULL, NULL);
    gunichar2 *wn = nombre ? g_utf8_to_utf16(nombre, -1, NULL, NULL, NULL) : NULL;
    CREDENTIALW c = { 0 };
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = (LPWSTR)w;
    c.UserName = (LPWSTR)wu;
    c.Comment = (LPWSTR)wn;
    c.CredentialBlobSize = (DWORD)strlen(clave);
    c.CredentialBlob = (LPBYTE)clave;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    gboolean ok = CredWriteW(&c, 0);
    if (!ok) error_win(err, "No se pudo guardar en el Administrador de credenciales");
    g_free(w); g_free(wu); g_free(wn);
    return ok;
}

gboolean llavero_olvidar_nombre(const char *nombre, GError **err)
{
    DWORD n = 0;
    PCREDENTIALW *lista = NULL;
    gboolean borro = FALSE;
    if (!CredEnumerateW(L"SFTP Commander: *", 0, &n, &lista)) {
        if (GetLastError() != ERROR_NOT_FOUND) error_win(err, "No se pudo leer el Administrador de credenciales");
        return FALSE;
    }
    for (DWORD i = 0; i < n; i++) {
        if (!lista[i]->Comment) continue;
        char *com = g_utf16_to_utf8((gunichar2 *)lista[i]->Comment, -1, NULL, NULL, NULL);
        if (g_strcmp0(com, nombre) == 0 && CredDeleteW(lista[i]->TargetName, CRED_TYPE_GENERIC, 0))
            borro = TRUE;
        g_free(com);
    }
    CredFree(lista);
    return borro;
}

void llavero_liberar(char *clave)
{
    if (!clave) return;
    SecureZeroMemory(clave, strlen(clave));   /* la borra de la memoria antes de liberar */
    g_free(clave);
}

#else
#include <libsecret/secret.h>

static const SecretSchema ESQUEMA = {
    "io.github.sftpcommander.Clave", SECRET_SCHEMA_NONE,
    {
        { "servidor", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "puerto",   SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "usuario",  SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "nombre",   SECRET_SCHEMA_ATTRIBUTE_STRING },
        { NULL, 0 },
    }
};

static GHashTable *atributos(const char *servidor, const char *puerto, const char *usuario)
{
    GHashTable *h = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(h, "servidor", (gpointer)servidor);
    g_hash_table_insert(h, "puerto", (gpointer)puerto);
    g_hash_table_insert(h, "usuario", (gpointer)usuario);
    return h;
}

char *llavero_buscar(const char *servidor, const char *puerto, const char *usuario, GError **err)
{
    GHashTable *h = atributos(servidor, puerto, usuario);
    char *c = secret_password_lookupv_sync(&ESQUEMA, h, NULL, err);
    g_hash_table_unref(h);
    return c;
}

gboolean llavero_guardar(const char *nombre, const char *servidor, const char *puerto,
                         const char *usuario, const char *clave, GError **err)
{
    GHashTable *h = atributos(servidor, puerto, usuario);
    if (nombre) g_hash_table_insert(h, "nombre", (gpointer)nombre);
    char *etiqueta = g_strdup_printf("SFTP Commander: %s@%s:%s", usuario, servidor, puerto);
    gboolean ok = secret_password_storev_sync(&ESQUEMA, h, SECRET_COLLECTION_DEFAULT,
                                              etiqueta, clave, NULL, err);
    g_free(etiqueta);
    g_hash_table_unref(h);
    return ok;
}

gboolean llavero_olvidar_nombre(const char *nombre, GError **err)
{
    GHashTable *h = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(h, "nombre", (gpointer)nombre);
    gboolean ok = secret_password_clearv_sync(&ESQUEMA, h, NULL, err);
    g_hash_table_unref(h);
    return ok;
}

void llavero_liberar(char *clave)
{
    if (clave) secret_password_free(clave);   /* la borra de la memoria antes de liberar */
}
#endif
