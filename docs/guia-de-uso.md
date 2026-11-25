# Guía de uso de SFTP Commander

[User guide in English](user-guide.md)

## La ventana

- **Barra de conexión** (arriba): desplegable de servidores guardados, servidor, puerto, usuario, contraseña, *Conectar* y *Administrar…*.
- **Pestañas**: cada pestaña es una sesión independiente. La barra muestra siempre los datos de la pestaña visible.
- **Dos paneles**: el equipo local a la izquierda y el servidor a la derecha. El panel activo tiene borde azul y las acciones se aplican a él.
- **Botones de acciones** y **barra de estado** abajo. La barra de estado es de la pestaña: una copia en curso en una pestaña no tapa los mensajes de otra.

## Conectar

Escribe el servidor y pulsa Enter o *Conectar*. El campo de servidor acepta:

- una IP o un nombre: `192.168.1.10`, `archivos.ejemplo.com`;
- un alias de `~/.ssh/config` (se usan su usuario, puerto y llave);
- `usuario@servidor:puerto` o `sftp://usuario@servidor:puerto`, que mandan sobre los campos sueltos.

Puerto vacío = 22, o el del alias de `~/.ssh/config`.

Mientras conecta, una tarjeta sobre el panel remoto muestra cada paso y el tiempo de espera. *Cancelar* deja de esperar en el momento. Si el servidor no responde en 15 segundos, el intento falla con un error.

### Primera conexión a un servidor

La primera vez aparece la huella SHA256 del servidor. Si confías en él, se guarda en `~/.ssh/known_hosts`, el mismo archivo que usa `ssh`. Si un servidor conocido presenta una llave **distinta**, SFTP Commander no conecta. Puede ser una reinstalación del servidor o alguien interceptando la conexión. Revísalo y, si es legítimo, borra la línea vieja con `ssh-keygen -R servidor`.

### Autenticación

Prueba en este orden:

1. La llave privada del servidor guardado, o las llaves del ssh-agent y de `~/.ssh`.
2. La contraseña escrita en la barra.
3. La contraseña guardada en el llavero para ese servidor, si hay.

Si aun así el servidor pide contraseña, se abre la ventana *Contraseña*. Marca *Recordar en el llavero del sistema* para guardarla cifrada; la próxima vez conecta sin preguntar. Si la guardada deja de servir, te pide una nueva.

Las contraseñas nunca se escriben en archivos. Quedan en el llavero Secret Service (GNOME Keyring en la mayoría de los escritorios). Se ven en *Contraseñas y claves* (Seahorse) como "SFTP Commander: usuario@servidor:puerto", y se borran con *Olvidar contraseña* en el administrador de servidores.

## Servidores guardados

*Administrar…* (Ctrl+L) abre la lista. Cada servidor tiene:

| Campo | Qué es |
|---|---|
| Nombre | Cómo aparece en la lista |
| Servidor | Nombre, IP o alias de `~/.ssh/config` |
| Puerto | Puerto SSH (opcional) |
| Usuario | Usuario (opcional) |
| Llave privada | Una llave específica (opcional) |
| Carpeta inicial | Carpeta remota que se abre al conectar (opcional) |
| Servidor de salto | Servidor intermedio por el que se pasa, `usuario@servidor:puerto` o un alias (opcional) |
| Llave del salto | Llave privada para el servidor de salto (opcional; vacío = ssh-agent y llaves de `~/.ssh`) |

*Guardar y conectar* guarda y conecta. *Quitar* lo borra de la lista y también olvida su contraseña. La lista se guarda en `~/.config/sftp-commander/servidores.ini` con permisos 600 y nunca tiene contraseñas.

Para conectar, elige el servidor en el desplegable *Servidores*. Si la pestaña visible ya está conectada, se abre en una pestaña nueva.

## Servidor de salto (bastión)

Cuando a un servidor solo se llega pasando por otro (un bastión), completa *Servidor de salto* en ese servidor guardado. Funciona como `ssh -J`:

1. SFTP Commander se conecta al servidor de salto, verifica su llave y entra (con *Llave del salto*, o con el ssh-agent y tus llaves de `~/.ssh`, o con contraseña).
2. Abre un túnel por el salto hacia el destino. El nombre del destino lo resuelve el salto, así que sirven los nombres y direcciones de su red interna.
3. Se conecta al destino por ese túnel, con su propia verificación de llave e inicio de sesión.

La tarjeta de conexión muestra cada paso. El servidor de salto solo necesita SSH, no SFTP.

Si el salto pide contraseña, una ventana aparte la pide (*Contraseña del servidor de salto*). Con *Recordar* marcado, se guarda en el llavero como "<nombre del servidor> (salto)", separada de la del destino. *Olvidar contraseña* y *Quitar* borran las dos.

La pestaña y el panel remoto muestran el destino como `usuario@destino vía usuario@salto`. La conexión perdida y *Reconectar* funcionan igual que en una conexión directa: al reconectar se vuelve a pasar por el salto.

Desde la terminal: `sftp-commander usuario@interno -J usuario@bastion:22`.

El campo de contraseña de la barra de conexión es para el destino, no para el salto.

## Warpgate y bastiones con código de un solo uso o aprobación en el navegador

[Warpgate](https://warpgate.null.page/) no es un servidor de salto al estilo `ssh -J`: es un proxy SSH al que te conectas directo, indicando en el usuario a qué destino quieres ir. Deja vacío *Servidor de salto* y completa:

| Campo | Valor |
|---|---|
| Servidor | el host de Warpgate |
| Puerto | el puerto SSH de Warpgate (2222 por defecto) |
| Usuario | `tu_usuario_de_warpgate:nombre_del_destino`, con dos puntos |

O escribe todo junto en el campo del servidor: `david:servidor-web@warpgate.ejemplo.com:2222`.

Según la política de tu usuario en Warpgate:

- **Contraseña**: se pide en la ventana de contraseña de siempre y se puede recordar en el llavero.
- **Llave pública**: agrega tu llave pública a tu usuario en la interfaz web de Warpgate.
- **Código de un solo uso (TOTP)**: después de la contraseña, una ventana pide el código de tu aplicación de autenticación.
- **Aprobación en el navegador**: una ventana muestra el enlace de aprobación (también hay un botón *Abrir en el navegador*) y el código de seguridad. Comprueba que el código sea el mismo que ves en el navegador, aprueba ahí y pulsa *Continuar*.

La misma ventana muestra cualquier otra pregunta que haga un servidor al iniciar sesión (keyboard-interactive). SFTP Commander nunca repite una respuesta por su cuenta: con un código mal escrito hay un solo intento fallido, y el servidor vuelve a preguntar o reconectas. Esto importa porque los bastiones bloquean la cuenta tras varios fallos.

**Primera vez con un destino:** Warpgate quiere que confirmes la llave del servidor de destino, y solo puede preguntarlo en una terminal. Por SFTP corta la conexión y SFTP Commander te lo avisa. Conéctate una vez con `ssh -p 2222 usuario:destino@host-de-warpgate` en una terminal y contesta `y`, o pide al administrador de Warpgate que confíe en esa llave desde su interfaz web. Después, SFTP funciona.

## Pestañas

- **Ctrl+T** o el botón a la derecha de la barra de pestañas: pestaña nueva.
- **Ctrl+W** o la × de la pestaña: cerrarla. No se puede cerrar mientras copia (primero cancela); si está conectando, cancela el intento.
- Ctrl+Av Pág / Ctrl+Re Pág cambian de pestaña. Se pueden reordenar arrastrándolas.

Cada pestaña tiene su propia conexión: puedes copiar en una mientras navegas otro servidor en la otra.

### Conexión perdida

Al volver a una pestaña, SFTP Commander comprueba que su conexión siga viva. Si la red se cortó o el servidor cerró la conexión, pregunta:

- *Reconectar*: vuelve a conectar con los mismos datos y a la misma carpeta remota.
- *Ahora no*: deja la pestaña desconectada; puedes usar *Conectar* después.
- *Cerrar pestaña*.

La misma ventana aparece si una operación falla porque la conexión ya no está.

## Trabajar con archivos

| Acción | Teclas | Notas |
|---|---|---|
| Copiar al otro panel | F5 | Sube o baja; las carpetas se copian enteras. Los archivos que ya existen se sobrescriben sin preguntar. |
| Renombrar | F2 | |
| Nueva carpeta | F7 | |
| Nuevo documento vacío | Mayús+F4 | Nunca pisa un archivo existente |
| Borrar | F8 o Supr | Pregunta antes; las carpetas se borran con todo su contenido |
| Actualizar | Ctrl+R | |
| Archivos ocultos | Ctrl+H | |

El menú del clic derecho tiene las mismas acciones. Un clic derecho en una zona vacía quita la selección, útil para *Nueva carpeta*.

Las copias largas muestran una barra de progreso y un botón *Cancelar* en la barra de estado de esa pestaña.

### Buscar

**Ctrl+F** (o *Buscar en esta carpeta* en el clic derecho) muestra un campo de búsqueda en el panel activo y filtra la carpeta actual mientras escribes:

- no distingue mayúsculas: `log` encuentra `Servicio.LOG`;
- `*` y `?` son comodines: `*.conf`, `backup-2026-??-*`;
- Enter selecciona la primera coincidencia; Esc quita la búsqueda;
- se quita sola al cambiar de carpeta.

Busca solo en la carpeta actual, no en las subcarpetas.

### Copiar ruta

**Ctrl+Mayús+C** (o *Copiar ruta*) copia al portapapeles la ruta completa de lo seleccionado, una por línea. Sin selección, copia la ruta de la carpeta actual.

### Abrir y editar archivos remotos

Doble clic sobre un archivo remoto (o Enter o F4):

1. Se baja a `~/.cache/sftp-commander/editar/<aleatorio>/`.
2. Se abre con un programa local: para texto, logs y configuraciones, un editor gráfico; para imágenes, un visor; para lo demás, el predeterminado del sistema.
3. **Cada vez que guardas en ese programa, el archivo se sube solo al servidor** y la barra de estado dice "Guardado en el servidor" con la hora.
4. Las copias temporales se borran al cerrar SFTP Commander.

La subida va por una pestaña conectada al mismo servidor. Si no hay ninguna, te avisa que te conectes y vuelvas a guardar.

Para ver qué programa usaría con un archivo: `sftp-commander --que-app archivo.txt`.

Límite: si alguien cambia el archivo en el servidor mientras lo editas, tu versión pisa ese cambio.

## Archivos y ubicaciones

| Qué | Dónde |
|---|---|
| Servidores guardados | `~/.config/sftp-commander/servidores.ini` (permisos 600) |
| Contraseñas | Llavero del sistema (Secret Service), esquema `io.github.sftpcommander.Clave` |
| Servidores conocidos | `~/.ssh/known_hosts` |
| Copias en edición | `~/.cache/sftp-commander/editar/` (se borran al salir) |
| Programa tras `make install` | `~/.local/bin/sftp-commander` |

En Windows:

| Qué | Dónde |
|---|---|
| Servidores guardados | `%LOCALAPPDATA%\sftp-commander\servidores.ini` |
| Contraseñas | Administrador de credenciales, *Credenciales de Windows*, "SFTP Commander: usuario@servidor:puerto" |
| Llaves, config y servidores conocidos | `%USERPROFILE%\.ssh\` |
| Copias en edición | `%TEMP%\sftp-commander\editar\` (se borran al salir) |
| Cambiar de unidad | Botón *Unidad* del panel local, o escribir `D:\` en la ruta |

## Solución de problemas

| Mensaje o problema | Qué hacer |
|---|---|
| *La llave del servidor cambió* | El servidor presenta otra llave que la guardada. Averigua por qué. Si es legítimo: `ssh-keygen -R servidor` y vuelve a conectar. |
| *El servidor pide contraseña* | No aceptó tus llaves. Escribe la contraseña, o agrega tu llave pública al `~/.ssh/authorized_keys` del servidor. |
| *No se pudo guardar la contraseña en el llavero* | No hay un Secret Service corriendo (pasa en escritorios mínimos o por SSH). Instala y arranca `gnome-keyring`, o sigue escribiendo la contraseña. |
| *Este servidor no acepta contraseña* | El servidor solo acepta llave o preguntas (código 2FA, aprobación). Deja la contraseña vacía. En Warpgate, la web y SSH pueden tener reglas distintas: aunque en la web entres con contraseña, por SSH quizá solo pida el código. Para ver qué acepta sin gastar un intento: `ssh -o PreferredAuthentications=none -o PubkeyAuthentication=no -p 2222 usuario@servidor`. |
| *El servidor no ofrece SFTP* | El servidor SSH no tiene el subsistema SFTP. En OpenSSH, activa `Subsystem sftp` en `sshd_config`. |
| Tarda y después falla | El servidor no está a la vista o un firewall descarta el tráfico. Se rinde a los 15 segundos; *Cancelar* corta antes. |
| `sftp-commander: orden no encontrada` después de `make install` | Agrega `~/.local/bin` al `PATH` (la mayoría de las distribuciones lo hace al volver a iniciar sesión). |
