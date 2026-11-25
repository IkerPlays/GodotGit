# Sincronización con GitHub en tiempo real (integrada en el editor)

Esta carpeta es código nativo (C++) del editor: no hay que instalar ningún plugin.
Cualquier proyecto abierto con este motor puede conectarse a un repositorio de GitHub.

- **Creas, guardas, renombras o borras** un archivo → se sube solo a GitHub (un commit por tanda de cambios).
- **Un compañero sube algo** → aparece en tu proyecto en pocos segundos; las escenas abiertas se recargan solas.
- **Varios colaboradores a la vez** sobre el mismo repo: los cambios se mezclan archivo a archivo.
- Panel **GitHub** (abajo en el editor): estado, actividad del equipo, registro e invitaciones.
- Botón de estado en la barra superior del editor (`○` sin conectar, `◐` sincronizando, `●` al día, `▲` error).

## Uso

### Al crear un proyecto
En **Crear nuevo proyecto** hay un campo **GitHub Repository (optional)**:

1. Escribe `usuario/repositorio` o pega la URL.
2. Deja marcado **Create it if missing (private)** si quieres que se cree (privado) cuando no exista.
3. La primera vez te pide el **token** (ver abajo); se guarda y no vuelve a pedirse.

Al abrir el proyecto, el editor se conecta solo, crea el repo si hace falta y sube el proyecto.

### En un proyecto ya abierto
Panel **GitHub** → pega el token y:
- **Connect**: `usuario/repo` (o URL) y rama opcional. Si el repo ya tiene contenido, se descarga.
- **Create and connect**: crea un repo nuevo en tu cuenta.

### Colaborar
Usa **Invite** (hace falta ser admin del repo) o invita desde GitHub. Tu compañero crea/abre un proyecto,
se conecta al mismo repo y recibe todo. Cada uno usa su propio token.

## Token

Clásico con permiso `repo` (<https://github.com/settings/tokens/new?scopes=repo>), o fine-grained con
**Contents: Read & write** sobre el repo (para crear repos hace falta el clásico).

- Se guarda en `<carpeta de config del editor>/github_token` con permisos `600` (solo tu usuario).
  Nunca va dentro del proyecto ni a GitHub.
- También se lee de la variable de entorno `GITHUB_TOKEN`.
- `GODOT_GITHUB_API_URL` cambia la raíz de la API (GitHub Enterprise: `https://HOST/api/v3`).

## Cómo funciona

Cada pasada compara tres estados por archivo (hash de blob de git): **base** (última sincronización),
**local** (disco) y **remoto** (rama de GitHub).

| local vs base | remoto vs base | acción |
|---|---|---|
| igual | cambió | se descarga / borra en local |
| cambió | igual | se sube / borra en GitHub |
| cambió | cambió | **conflicto** |

- **Subida**: un solo commit atómico con la API Git Data (blobs → árbol → commit → mover la rama sin forzar).
  Si alguien se adelantó (422), se vuelve a sincronizar y se reintenta.
- **Tiempo real**: se consulta la rama cada 4 s (configurable, 2–60 s) con `ETag`; las respuestas
  `304 Not Modified` no gastan cuota de la API. Los cambios locales se detectan al guardar (antirrebote de
  1,5 s) y revisando el disco en cada ciclo (con caché por fecha de modificación).
- **Conflictos** (mismo archivo cambiado en ambos lados): gana tu versión y la remota se guarda en
  `.godotgit/conflicts/<fecha>/remote/…` (además queda en el historial de git). En la **primera conexión** gana
  la del repo y la tuya queda en `…/local/…`, para no pisar un repo existente.
- **Escenas abiertas**: si cambian en GitHub se recargan; si tienes cambios sin guardar en esa escena no se
  tocan y el registro avisa.
- **Reintentos**: sin red o con error, la comprobación se espacia con espera exponencial (hasta 60 s).
- **Ignorados**: `.godot/`, `.git/`, `.godotgit/`, temporales y lo que diga el `.gitignore`
  (patrones simples: `*.ext`, `carpeta/`, `/ruta`; sin negaciones `!`). Si no hay `.gitignore` se crea uno.
- **Estado local**: `res://.godotgit/` (`config.cfg` con el repo, `state.json`, `conflicts/`), con `.gdignore`
  y añadido al `.gitignore`. Para desconectar: botón **Disconnect**.

## Límites conocidos

- Se sincroniza lo que está **guardado en disco** (Ctrl+S).
- No hay fusión línea a línea: dos personas editando el mismo `.tscn`/`.gd` a la vez se resuelve por conflicto
  de archivo. Para escenas, mejor repartir quién toca qué.
- Archivos de más de 50 MB no se sincronizan (usa Git LFS o exclúyelos).
- No se conservan permisos de ejecución ni symlinks.
- La interfaz está en inglés (cadenas `TTR`, listas para traducir en los `.po`).

## Archivos

| archivo | contenido |
|---|---|
| `github_client.*` | cliente asíncrono mínimo de la API REST de GitHub |
| `github_sync.*` | motor de sincronización (3 vías, push/pull, sondeo con ETag, ignorados, persistencia) |
| `github_sync_dock.*` | panel «GitHub» del editor |
| `github_sync_plugin.*` | `EditorPlugin` integrado: panel inferior + botón de estado |
| `../project_manager/project_dialog.*` | campo de repo/token al crear un proyecto |
