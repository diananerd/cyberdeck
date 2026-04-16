# APPS.md — App Catalog

Carta de diseño de las apps que deben existir en CyberDeck. Cada app define qué servicios del OS
consume, qué almacena y dónde, y qué experiencia de usuario ofrece. Sin detalles de implementación.

---

## Índice

1. [Settings](#1-settings)
2. [Files](#2-files)
3. [Notes](#3-notes)
4. [Tasks](#4-tasks)
5. [Music](#5-music)
6. [Podcasts](#6-podcasts)
7. [Calculator](#7-calculator)
8. [Books](#8-books)
9. [Bluesky](#9-bluesky)
10. [Mail](#10-mail)
11. [Bitchat](#11-bitchat)
12. [Meshtastic](#12-meshtastic)

---

## 1. Settings

**ID:** `APP_ID_SETTINGS` (builtin)
**Icono:** `ST`
**Permisos:** `WIFI | SD | SETTINGS`

### Qué hace

La app de configuración del OS. Controla todo lo que el sistema expone como ajustable: display,
audio, red, seguridad, tiempo, almacenamiento, y sistema. Es la interfaz entre el usuario y los
servicios del OS — no tiene lógica propia, solo traduce acciones del usuario en llamadas a
`os_settings_set_*` y `svc_*`.

### Pantallas

- **Menú principal** — Lista de categorías
- **Display** — Brillo, tema (Green/Amber/Neon), rotación, timeout de pantalla
- **Audio** — Volumen maestro, dispositivo Bluetooth paired
- **WiFi** — Lista de redes disponibles, conectar, olvidar, ver detalles (IP, RSSI, canal)
- **Time** — Zona horaria, sincronización SNTP manual, display de hora actual
- **Security** — PIN on/off, cambio de PIN
- **Storage** — Uso de SD card, apps instaladas, limpiar cache por app
- **Bluetooth** — Scan de dispositivos, pair, connect, disconnect
- **About** — Versión de firmware, modelo, número de serie, MAC, créditos, OTA update

### Storage

- **NVS global** (`svc_settings`) — toda la configuración del OS
- **Sin SD card** — Settings funciona sin SD montada

### Servicios consumidos

`svc_wifi` · `svc_time` · `svc_battery` · `svc_ota` · `os_settings` · `ui_theme`

---

## 2. Files

**ID:** `APP_ID_FILES`
**Icono:** `FL`
**Permisos:** `SD`

### Qué hace

Explorador de archivos para la SD card. Navega directorios, visualiza archivos de texto, copia,
mueve, renombra y elimina. Es la ventana directa al filesystem — sin base de datos, sin metadatos
extra, sin parsers especiales.

### Pantallas

- **Explorador** — Lista de archivos y carpetas del directorio actual, con tamaño y fecha
- **Vista previa de texto** — Para `.txt`, `.md`, `.json`, `.lua`, `.py` y similares, solo lectura
- **Detalles** — Nombre, ruta, tamaño, fecha de modificación
- **Acciones** — Renombrar, eliminar (con confirm dialog), mover (copiar + eliminar origen)

### Navegación

Breadcrumb en el statusbar. Botón BACK navega al directorio padre. Punto de entrada: `/sdcard/`.
Directorios protegidos del OS (`/sdcard/apps/*/cache/`) listados pero no modificables.

### Storage

- **FS puro** — Solo operaciones POSIX sobre `/sdcard/`
- **Sin NVS, sin SQLite** — La app no guarda nada propio

### Servicios consumidos

`os_storage` · `EVT_SDCARD_MOUNTED` · `EVT_SDCARD_UNMOUNTED`

---

## 3. Notes

**ID:** `APP_ID_NOTES`
**Icono:** `NT`
**Permisos:** `SD`

### Qué hace

Editor de notas en Markdown. Escribe, edita y organiza notas en archivos `.md` en la SD card.
El editor es inline — lo que se ve es lo que hay, con renderizado básico de Markdown (negritas,
cursivas, encabezados, listas, bloques de código). Sin carpetas ni jerarquía compleja: las notas
son archivos planos en un directorio, ordenadas por fecha de modificación.

### Pantallas

- **Lista de notas** — Título (primera línea H1 o nombre de archivo) + fecha + preview de primeras líneas
- **Editor** — Área de texto con renderizado inline: H1-H3 en tamaño mayor, `**bold**` renderizado,
  `*italic*`, `` `code` `` en monospace, `- item` como lista, `---` como divider
- **Búsqueda** — Búsqueda por contenido (grep sobre archivos) y por título

### Capacidades del editor

- Creación de nota nueva (nombre auto-generado por timestamp, editable)
- Edición con keyboard virtual
- Guardado automático al salir (`on_pause` / `on_destroy`)
- Eliminar nota con confirm dialog
- Compartir / exportar no previsto en v1

### Storage

- **FS** — `/sdcard/apps/notes/files/*.md`
- **Sin SQLite** — El índice se construye en runtime leyendo el directorio
- **Sin NVS** — No hay preferencias que persistan

### Servicios consumidos

`os_storage` · `ui_keyboard`

### Extensión futura

Books extiende esta app para lectura de grandes archivos con metadatos. El parser Markdown de
Notes es la base reutilizable.

---

## 4. Tasks

**ID:** `APP_ID_TASKS`
**Icono:** `TK`
**Permisos:** `SD`

### Qué hace

Lista de tareas con soporte de proyectos, prioridades, fechas límite y estados. Todo almacenado en
SQLite. Es la app "GTD mínimo viable" del cyberdeck — sin sync, sin colaboración, sin notificaciones
push. Local-first.

### Modelo de datos

```
Project { id, name, color, created_at }
Task    { id, project_id, title, notes, priority, due_date, done, created_at, updated_at }
```

### Pantallas

- **Inbox** — Tareas sin proyecto asignado, ordenadas por fecha de creación
- **Proyectos** — Lista de proyectos; tap → lista de tareas del proyecto
- **Lista de tareas** — Filtrable por estado (todas / pendientes / completadas), ordenable
- **Detalle de tarea** — Título, notas (texto libre), proyecto, prioridad, fecha límite, toggle done
- **Nueva tarea** — Form rápido: título obligatorio, resto opcional
- **Búsqueda** — SQL `LIKE` sobre título y notas

### Interacción

Swipe horizontal en un item de tarea → mark done / delete (con undo toast de 3s).
Long press → opciones contextuales (mover a proyecto, cambiar prioridad).

### Storage

- **SQLite** — `/sdcard/apps/tasks/tasks.db`
- **Migraciones** — versioning de schema desde v1
- **Sin NVS, sin FS adicional**

### Servicios consumidos

`os_app_storage` · `os_db`

---

## 5. Music

**ID:** `APP_ID_MUSIC`
**Icono:** `MU`
**Permisos:** `SD | BLUETOOTH`

### Qué hace

Reproductor de música local. Lee archivos de audio desde SD card, extrae metadatos (ID3 para MP3,
Vorbis comments para FLAC/OGG), construye una biblioteca indexada en SQLite, y transmite audio vía
módulo Bluetooth externo (UART1, auto-detectado en boot).

No reproduce audio por el ESP32-S3 directamente (no tiene DAC de audio ni parlante). El módulo BT
externo (A2DP sink conectado a parlante o auriculares) es el destino de la salida.

### Modelo de datos

```
Track  { id, path, title, artist, album, duration_s, track_no, year, indexed_at }
Album  { id, artist, name, year, cover_path }
Queue  { position, track_id }
```

### Pantallas

- **Biblioteca** — Tabs: Artistas / Álbumes / Canciones / Listas
- **Álbum** — Cover (si existe) + lista de pistas
- **Now Playing** — Cover, título, artista, álbum, barra de progreso, controles (prev/play/pause/next), volumen
- **Cola** — Lista reordenable de próximas canciones
- **Configuración de salida** — Selector de dispositivo BT, volumen

### Indexación

Al montar SD card, la app escanea `/sdcard/Music/` (y subcarpetas) en background task,
extrae metadatos y actualiza la base de datos. Un indicador en la pantalla muestra el progreso.
Re-indexa solo archivos nuevos o modificados (compara `mtime`).

### Storage

- **SQLite** — `/sdcard/apps/music/music.db` (índice de biblioteca)
- **FS** — Archivos de audio en `/sdcard/Music/` (no mueve ni copia archivos)
- **NVS** — Última posición de reproducción, volumen, modo shuffle/repeat

### Servicios consumidos

`os_app_storage` · `os_db` · `os_app_nvs` · `os_task` (background indexer) ·
`EVT_SDCARD_MOUNTED` · `EVT_AUDIO_STATE_CHANGED` · `EVT_BLUETOOTH_CONNECTED`

---

## 6. Podcasts

**ID:** `APP_ID_PODCASTS`
**Icono:** `PC`
**Permisos:** `SD | WIFI | NETWORK | BLUETOOTH`

### Qué hace

Cliente de podcasts completo. Suscripción a feeds RSS/Atom, descarga de episodios al SD card,
reproducción vía módulo BT externo. Soporta escucha offline (episodios descargados), streaming
directo (si WiFi disponible), y sincronización de posición de reproducción.

### Modelo de datos

```
Show    { id, title, author, description, feed_url, artwork_url, artwork_path, subscribed_at }
Episode { id, show_id, title, description, audio_url, duration_s, pub_date,
          download_path, download_state, played, position_s }
```

### Pantallas

- **Suscripciones** — Grid de shows con artwork
- **Show** — Artwork grande, descripción, lista de episodios (newest first)
- **Episodio** — Descripción, estado de descarga, botón play/download
- **Now Playing** — Artwork, título, show, barra de progreso, ±30s skip, velocidad de reproducción
- **Cola de descarga** — Progreso de descargas activas y pendientes
- **Buscar shows** — Búsqueda en directorio de podcasts (API externa, ej. Podcast Index)
- **Agregar por URL** — Input de URL de feed RSS directo

### Flujo de refresh

Al abrir la app (o manualmente): fetch del RSS feed para cada suscripción → parsear XML →
insertar episodios nuevos en SQLite → mostrar badge de nuevos episodios.

### Descarga

`svc_http_download` con resume. El episodio se descarga a `/sdcard/apps/podcasts/files/{episode_id}.mp3`.
Progreso visible en pantalla de episodio y en la cola de descargas.

### Storage

- **SQLite** — `/sdcard/apps/podcasts/podcasts.db`
- **FS** — `/sdcard/apps/podcasts/files/` (episodios descargados + artworks)
- **NVS** — Posición de reproducción del episodio actual, velocidad de reproducción

### Servicios consumidos

`os_app_storage` · `os_db` · `os_app_nvs` · `svc_http` (feed fetch + streaming) ·
`svc_http_download` (episodios) · `EVT_WIFI_CONNECTED` · `EVT_DOWNLOAD_PROGRESS` ·
`EVT_AUDIO_STATE_CHANGED` · `EVT_BLUETOOTH_CONNECTED`

---

## 7. Calculator

**ID:** `APP_ID_CALC`
**Icono:** `CL`
**Permisos:** ninguno

### Qué hace

Calculadora científica con historial de operaciones. Sin red, sin storage en disco (salvo historial
en NVS), sin servicios externos. Toda la lógica vive en la app: un parser de expresiones matemáticas
que evalúa strings como `"sin(45) * (3 + 2^4) / sqrt(2)"`.

### Capacidades

- Operaciones básicas: `+ - * / ^ %`
- Funciones: `sin cos tan asin acos atan sqrt log ln abs floor ceil round`
- Constantes: `pi e`
- Paréntesis anidados
- Notación científica: `1.5e3`
- Modo deg/rad para funciones trigonométricas

### Pantallas

- **Principal** — Display de expresión actual + resultado, teclado numérico + operadores
- **Historial** — Lista de las últimas 50 operaciones con sus resultados (tap para reusar)
- **Científica** — Panel extendido con funciones trigonométricas y constantes

### Storage

- **NVS** (`os_app_nvs`) — Historial de últimas 50 operaciones, modo deg/rad
- **Sin SQLite, sin SD**

### Servicios consumidos

`os_app_nvs`

---

## 8. Books

**ID:** `APP_ID_BOOKS`
**Icono:** `BK`
**Permisos:** `SD | WIFI | NETWORK`

### Qué hace

Lector de libros y documentos largos. Extiende el parser Markdown de Notes pero para uso de solo
lectura, con paginación por capítulos, marcadores, posición recordada y soporte de imágenes inline.
Soporta descarga de libros desde fuentes externas (Project Gutenberg, Standard Ebooks, URLs directas).

Formatos soportados: Markdown (`.md`), texto plano (`.txt`), y en el futuro EPUB simplificado.

### Modelo de datos

```
Book      { id, title, author, description, cover_path, file_path, format,
            total_chars, added_at, source_url }
Bookmark  { id, book_id, position, label, created_at }
Progress  { book_id, position, last_read_at }
```

### Pantallas

- **Biblioteca** — Grid de libros con cover y título. Filtro por autor / leídos / sin terminar.
- **Libro** — Tabla de contenidos (si el archivo tiene headers H1/H2), lista de capítulos
- **Lector** — Texto renderizado con Markdown (H1-H3 como títulos, negritas, cursivas,
  imágenes inline cargadas desde SD, bloques de código en monospace). Scroll continuo.
  Tap izquierda / derecha para cambiar capítulo. Barra de progreso discreta.
- **Marcadores** — Lista de bookmarks del libro actual, tap para saltar
- **Buscar en libro** — Búsqueda de texto dentro del archivo
- **Descargar libro** — Input de URL o búsqueda en catálogos (Project Gutenberg API)

### Imágenes

Las imágenes referenciadas en Markdown (`![alt](path)`) se buscan relativas al archivo del libro.
Se renderizan inline usando LVGL canvas, escaladas a ancho disponible. Solo JPEG y PNG pequeños
(< 256KB descomprimidos) para no agotar PSRAM.

### Descarga

`svc_http_download` descarga el archivo a `/sdcard/apps/books/files/{id}.md`. El cover se descarga
por separado a `/sdcard/apps/books/files/{id}_cover.jpg`.

### Storage

- **SQLite** — `/sdcard/apps/books/books.db`
- **FS** — `/sdcard/apps/books/files/` (textos + covers + imágenes)
- **NVS** — Última posición de lectura (posición en chars), preferencias de fuente/tamaño

### Servicios consumidos

`os_app_storage` · `os_db` · `os_app_nvs` · `svc_http` · `svc_http_download` ·
`EVT_WIFI_CONNECTED`

---

## 9. Bluesky

**ID:** `APP_ID_BLUESKY`
**Icono:** `BS`
**Permisos:** `WIFI | NETWORK | SD`

### Qué hace

Cliente nativo de Bluesky para el cyberdeck. Implementa el protocolo AT (ATProto) para autenticación,
lectura de feeds, publicación de posts, manejo de notificaciones, y visualización de perfiles.
Diseñado para uso cómodo en la pantalla táctil de 800x480 con el lenguaje visual del cyberdeck.

### ATProto — Servicios necesarios

- **Autenticación** — `com.atproto.server.createSession` (handle + app password → JWT tokens)
- **Refresh de token** — `com.atproto.server.refreshSession` automático en 401
- **Feed** — `app.bsky.feed.getTimeline`, `app.bsky.feed.getFeedGenerators`
- **Posts** — `app.bsky.feed.getPostThread`, `com.atproto.repo.createRecord` (crear post)
- **Notificaciones** — `app.bsky.notification.listNotifications`, `updateSeen`
- **Perfiles** — `app.bsky.actor.getProfile`, `app.bsky.graph.follow`
- **Imágenes** — Descarga desde CDN via `svc_http` + cache local en SD
- **Blobs** — `com.atproto.repo.uploadBlob` para adjuntar imágenes a posts

### Pantallas

- **Login** — Handle + App Password (nunca la contraseña real). Explica qué es un App Password.
- **Timeline** — Feed infinito de posts. Avatar (circular, cargado desde CDN), handle, texto,
  timestamp relativo ("2m", "1h", "3d"). Imágenes en posts: thumbnail tappable → vista completa.
- **Hilo** — Post raíz + respuestas en árbol. Scroll continuo.
- **Composer** — Editor de post (máx 300 chars con contador), adjuntar imagen desde SD card,
  responder a un post, quote post.
- **Notificaciones** — Lista de likes, reposts, follows, menciones, respuestas. Badge en launcher.
- **Perfil** — Avatar, display name, handle, bio, contadores (follows/followers/posts),
  lista de posts del perfil.
- **Búsqueda** — Búsqueda de usuarios y posts. Sugerencias de follows.
- **Configuración de cuenta** — Ver cuenta activa, cerrar sesión, cambiar cuenta.

### Imágenes y media

Avatares e imágenes de posts se descargan via `svc_http_get_async` y se cachean en
`/sdcard/apps/bluesky/cache/` con nombre hash de la URL. El cache tiene límite de tamaño
(configurable, default 50MB) con evicción LRU.

### Storage

- **SQLite** — Posts cacheados del timeline, notificaciones no vistas, perfiles visitados
- **FS** — `/sdcard/apps/bluesky/cache/` (imágenes, avatares)
- **NVS** — Access token, refresh token, DID, handle, PDS URL, última posición de timeline

### Servicios consumidos

`svc_http` (session ATProto con Bearer + refresh hook) · `svc_http_download` (imágenes) ·
`os_app_storage` · `os_db` · `os_app_nvs` · `EVT_WIFI_CONNECTED`

---

## 10. Mail

**ID:** `APP_ID_MAIL`
**Icono:** `ML`
**Permisos:** `WIFI | NETWORK | SD`

### Qué hace

Cliente de correo electrónico completo con soporte IMAP (recepción) y SMTP (envío). Renderiza
emails HTML como Markdown simplificado para mostrarse en el display del cyberdeck, y convierte
los borradores escritos en Markdown a HTML para el envío. Local-first: sincroniza los últimos
N emails por carpeta y los guarda en SQLite.

### Modelo de datos

```
Account  { id, email, display_name, imap_host, imap_port, smtp_host, smtp_port,
           imap_user, smtp_user, last_sync_at }
Folder   { id, account_id, name, uidvalidity, uid_next, unseen_count }
Message  { id, folder_id, uid, subject, from_name, from_addr, to_addrs, cc_addrs,
           date, body_text, body_html, body_md, has_attachments, seen, flagged,
           local_only }
Attachment { id, message_id, filename, mime_type, size, local_path }
```

### Pantallas

- **Cuentas** — Lista de cuentas configuradas. Tap → bandeja de entrada de esa cuenta.
- **Bandeja de entrada** — Lista de mensajes: remitente, asunto, preview de primeras líneas,
  timestamp, badge de no leído. Pull-to-refresh (swipe down en lista).
- **Carpetas** — Inbox, Sent, Drafts, Spam, Trash + carpetas personalizadas del servidor.
- **Mensaje** — Remitente (con avatar generado por iniciales), destinatarios, asunto, fecha,
  cuerpo renderizado (HTML → Markdown → LVGL). Adjuntos listados con nombre y tamaño,
  descargables a `/sdcard/`.
- **Composer** — Editor Markdown. Para: (autocompletado desde contactos recientes), Asunto,
  Cuerpo. Adjuntar archivo desde Files. Enviar convierte MD → HTML para el servidor SMTP.
- **Borradores** — Lista de mensajes guardados localmente pero no enviados.
- **Buscar** — Búsqueda IMAP server-side (IMAP SEARCH) + local en SQLite.
- **Configuración de cuenta** — Agregar cuenta (IMAP/SMTP host, puerto, usuario, contraseña),
  editar, eliminar.

### IMAP y SMTP

La app requiere dos servicios del OS que no existen todavía:

- **`svc_imap`** — Cliente IMAP4rev1 con TLS. Capacidades: `LOGIN`, `SELECT`, `FETCH`,
  `SEARCH`, `STORE` (mark seen/flagged), `EXPUNGE`, `IDLE` (notificaciones push de nuevo mail).
  Sync incremental usando UID y UIDVALIDITY.

- **`svc_smtp`** — Cliente SMTP con STARTTLS o TLS directo. Capacidades: `AUTH LOGIN` /
  `AUTH PLAIN`, envío de mensajes MIME multipart (texto plano + HTML + adjuntos),
  codificación base64 de adjuntos.

Ambos servicios son compartidos (una instancia del OS, instanciables por cuenta como `svc_http`).

### HTML ↔ Markdown

- **Inbound (HTML → Markdown):** El parser extrae texto de los tags HTML, convierte `<b>` → `**`,
  `<i>` → `*`, `<a href>` → `[text](url)`, `<ul><li>` → `- item`, elimina estilos CSS y scripts.
  Imágenes inline (`<img src>`) se descargan y muestran como en Books.

- **Outbound (Markdown → HTML):** El renderer convierte el Markdown del compositor a HTML básico
  antes de enviarlo via SMTP. Usa plantilla de email simple para que el HTML sea válido y
  compatible con clientes de escritorio.

### Storage

- **SQLite** — Mensajes, carpetas, cuentas, adjuntos indexados
- **FS** — `/sdcard/apps/mail/files/` (adjuntos descargados, imágenes inline cacheadas)
- **NVS** — Contraseñas de cuentas (cifradas o al menos no en plain text en SQLite),
  cuenta activa, carpeta activa, timestamp de último sync

### Servicios consumidos

`svc_imap` (nuevo) · `svc_smtp` (nuevo) · `svc_http` (imágenes inline) · `os_app_storage` ·
`os_db` · `os_app_nvs` · `EVT_WIFI_CONNECTED`

---

## 11. Bitchat

**ID:** `APP_ID_BITCHAT`
**Icono:** `BC`
**Permisos:** ninguno (BLE no requiere permiso especial en este modelo)

### Qué hace

Chat de malla descentralizado sobre Bluetooth Low Energy. Sin internet, sin servidor, sin cuenta.
Los nodos (otros cyberdecks o dispositivos con Bitchat) se descubren automáticamente y retransmiten
mensajes entre sí formando una red de malla. Inspirado en [bitchat](https://github.com/permadao/bitchat)
y conceptos de LoRa mesh chat.

**El ESP32-S3 tiene BLE nativo** — esta app no requiere hardware adicional.

### Protocolo de malla BLE

- **Advertising** — Cada nodo anuncia su presencia con UUID de servicio Bitchat + nombre de usuario
- **Scanning** — Descubrimiento continuo de nodos vecinos
- **GATT** — Servicio de mensajería: characteristic de escritura para recibir mensajes,
  characteristic de notificación para enviar
- **Flooding con TTL** — Cada mensaje incluye un ID único y TTL (default: 7 hops). Los nodos
  retransmiten mensajes que no han visto, decrementando TTL.
- **Deduplicación** — Cache de IDs de mensajes recibidos (ventana de 5 minutos)

### Modelo de datos

```
Peer    { id, name, rssi, last_seen_at, is_online }
Room    { id, name, is_private, created_at }
Message { id, room_id, sender_id, text, timestamp, local_id, hops_remaining, received_at }
```

### Pantallas

- **Sala global** — Chat de broadcast visible para todos los nodos en rango (o en la malla)
- **Salas** — Lista de salas activas descubiertas en la red + crear sala nueva
- **Chat de sala** — Mensajes en orden cronológico, input de texto, send button
- **Nodos** — Lista de peers descubiertos, RSSI, nombre, último visto. Indicador de calidad de malla.
- **Perfil** — Nombre de usuario (almacenado en NVS, editable), ID de nodo (generado en primer boot)
- **Configuración** — TTL por defecto, modo de scan (agresivo / conservador para batería)

### Canales y privacidad

- **Canal global** — Sin encriptación, todos los nodos lo ven
- **Canales nombrados** — Identificados por nombre (hash como room_id), accesibles a cualquier nodo
- **Mensajes directos** — Encriptación end-to-end con curva25519 entre dos nodos (opcional en v1)

### Storage

- **SQLite** — Historial de mensajes (últimas 500 entradas por sala), peers conocidos
- **NVS** — Nombre de usuario, clave privada para E2E, última sala activa

### Servicios consumidos

Servicio BLE nativo del OS (nuevo: `svc_ble`) · `os_app_storage` · `os_db` · `os_app_nvs`

### Servicio nuevo requerido

**`svc_ble`** — Servicio del OS para BLE: advertising, scanning, GATT server/client, callbacks
de conexión y escritura. Instanciable por apps como `svc_http`.

---

## 12. Meshtastic

**ID:** `APP_ID_MESHTASTIC`
**Icono:** `MS`
**Permisos:** ninguno

### Qué hace

Cliente Meshtastic para el cyberdeck. Se conecta a un radio LoRa externo (módulo Meshtastic
compatible, por ejemplo EBYTE E22 o Heltec LoRa32) via SPI o UART, y actúa como interfaz gráfica
para la red de malla LoRa. Sin el radio, la app puede conectarse a un nodo Meshtastic externo
via BLE y actuar como pantalla remota.

Meshtastic provee: chat de texto en malla, posicionamiento GPS compartido, telemetría de nodos
(batería, temperatura, presión), y canales cifrados.

### Modos de conexión

- **LoRa directo (SPI/UART)** — Módulo LoRa conectado al bus SPI o UART disponible del ESP32-S3.
  La app actúa como nodo Meshtastic completo. Requiere expansión de hardware.
- **BLE bridge** — La app conecta vía BLE a un nodo Meshtastic externo (otro dispositivo)
  y actúa como pantalla / teclado remoto. No requiere hardware adicional.

### Protocolo

Meshtastic usa Protobuf sobre LoRa con cifrado AES-256-CTR por canal. La app implementa
el parser de Protobuf de Meshtastic (`mesh.proto`) para leer y generar paquetes.

### Modelo de datos

```
Node      { id, long_name, short_name, lat, lon, alt, battery_pct,
            last_heard_at, snr, hops_away }
Channel   { id, name, psk_hash, is_primary }
Message   { id, channel_id, from_node_id, text, timestamp, rx_snr, hop_limit }
Waypoint  { id, lat, lon, name, description, icon, expire_at }
```

### Pantallas

- **Chat** — Mensajes del canal primario. Selector de canal en la parte superior.
- **Nodos** — Lista de nodos conocidos: nombre, distancia estimada (si hay GPS), RSSI/SNR,
  batería, último contacto. Tap → detalle del nodo.
- **Mapa** — Vista 2D de nodos con posición GPS en cuadrícula. Sin tiles de mapa externo
  (solo nodos como puntos sobre fondo negro con coordenadas). Si hay GPS local, muestra
  posición propia.
- **Telemetría** — Gráficas de batería, temperatura, presión por nodo (últimas 24h)
- **Canales** — Lista de canales configurados, agregar por QR-code string o manual
- **Configuración** — Nombre del nodo, preset de LoRa (LongFast / MediumSlow / ShortTurbo),
  frecuencia (región), modos de sleep
- **Diagnóstico** — Log de paquetes raw: tipo, origen, destino, SNR, hop count

### GPS

Si el ESP32-S3 tiene un módulo GPS conectado (via UART), la app lo usa para reportar
posición propia a la malla. Si no hay GPS, el campo de posición queda vacío.

### Storage

- **SQLite** — Historial de mensajes por canal (últimas 1000 entradas), nodos conocidos,
  waypoints, historial de telemetría
- **NVS** — Nombre del nodo, configuración de radio, canales (PSK), región de frecuencia,
  modo de conexión activo

### Servicios consumidos

`svc_ble` (modo bridge) · nuevo `svc_lora` (modo directo) · `os_app_storage` · `os_db` ·
`os_app_nvs` · `os_poller` (telemetría periódica)

### Servicios nuevos requeridos

**`svc_lora`** — Driver para módulo LoRa externo vía SPI/UART. Abstrae el chip (SX1276/SX1262/SX1268)
y expone: send packet, receive callback, RSSI/SNR, configuración de parámetros LoRa
(SF, BW, CR, potencia).

---

## Resumen de servicios nuevos requeridos

| Servicio | Requerido por | Descripción |
|---|---|---|
| `svc_http` | Podcasts, Books, Bluesky, Mail | HTTP client con session, auth, async, JSON |
| `os_app_nvs` | Calculator, Music, Podcasts, Books, Bluesky, Mail, Bitchat, Meshtastic | NVS privado por app |
| `svc_imap` | Mail | Cliente IMAP4rev1 con TLS e IDLE |
| `svc_smtp` | Mail | Cliente SMTP con STARTTLS y MIME multipart |
| `svc_ble` | Bitchat, Meshtastic | BLE advertising, scanning, GATT server/client |
| `svc_lora` | Meshtastic | Driver para módulo LoRa externo vía SPI/UART |

## Resumen de storage por app

| App | SQLite | FS (SD) | NVS privado | Sin storage |
|---|---|---|---|---|
| Settings | — | — | — | usa NVS global |
| Files | — | FS puro | — | — |
| Notes | — | `.md` files | — | — |
| Tasks | ✅ | — | — | — |
| Music | ✅ (índice) | audio files | ✅ (posición, prefs) | — |
| Podcasts | ✅ | episodios + covers | ✅ (posición, vel.) | — |
| Calculator | — | — | ✅ (historial, modo) | — |
| Books | ✅ | textos + covers | ✅ (posición, fuente) | — |
| Bluesky | ✅ (cache) | imágenes cache | ✅ (tokens, handle) | — |
| Mail | ✅ (mensajes) | adjuntos + imgs | ✅ (contraseñas) | — |
| Bitchat | ✅ (historial) | — | ✅ (nombre, clave) | — |
| Meshtastic | ✅ (todo) | — | ✅ (config radio) | — |
