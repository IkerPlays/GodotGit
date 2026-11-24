# GodotGit — editor de Godot con GitHub en tiempo real

Plugin de editor (GDScript, sin compilar nada) que conecta un proyecto de Godot con un
repositorio de GitHub:

- **Creas, guardas, renombras o borras** un archivo → se sube solo a GitHub (un commit por tanda de cambios).
- **Tu compañero sube algo** → aparece en tu proyecto en pocos segundos, y las escenas abiertas se recargan solas.
- **Varios colaboradores a la vez**: todos editan el mismo repo; el plugin mezcla los cambios archivo a archivo.
- Panel **GitHub** (abajo en el editor): estado, actividad del equipo, registro, invitar colaboradores.

Pensado para Godot **4.4 o superior** (usa solo API estable; las funciones se comprobaron contra el código de este repo, 4.8-beta).

> **Estado:** los scripts pasan el parser de GDScript (`gdparse`), pero **todavía no se han ejecutado dentro de un editor ni contra GitHub** (se escribieron en un entorno sin Godot ni acceso a una cuenta). Pruébalo primero con un repo de pruebas y reporta cualquier fallo.

## Instalación

### Proyecto nuevo (ya conectado a un repo)

```bash
python3 godotgit/godotgit_setup.py ~/juegos/mi-juego --repo usuario/mi-juego
```

Crea la carpeta con `project.godot`, copia el addon, lo activa y deja apuntado el repo.
Ábrelo con Godot, pega tu token en el panel **GitHub** y listo.
Sin `--repo` crea el proyecto igual y conectas/creas el repo desde el panel.

### Proyecto que ya tienes

```bash
python3 godotgit/godotgit_setup.py /ruta/a/tu/proyecto
```

o copia a mano `godotgit/addons/github_sync` en `tu_proyecto/addons/` y actívalo en
*Proyecto → Configuración del proyecto → Plugins*.

## Conectar con GitHub

1. Crea un token: <https://github.com/settings/tokens/new?scopes=repo> (clásico, permiso `repo`).
   Con un token *fine-grained* basta con **Contents: Read & write** sobre el repo
   (para «Crear repo nuevo» hace falta el clásico con `repo`).
2. En el panel **GitHub** pega el token y:
   - **Conectar**: escribe `usuario/repo` (o pega la URL), rama opcional.
   - **Crear y conectar**: crea un repo nuevo (privado por defecto) en tu cuenta.
3. El token se guarda en la configuración del editor de **tu usuario** (nunca dentro del proyecto
   ni en GitHub). También se lee de la variable de entorno `GITHUB_TOKEN`.

Para trabajar en equipo: usa **Invitar** (necesitas ser admin del repo) o invita desde GitHub.
Tu compañero abre un proyecto vacío con el addon, se conecta al mismo repo y recibe todo el proyecto.

## Cómo funciona

Cada pasada compara tres estados por archivo (hash de blob de git):

| local vs base | remoto vs base | acción |
|---|---|---|
| igual | cambió | se descarga / borra en local |
| cambió | igual | se sube / borra en GitHub |
| cambió | cambió | **conflicto** (ver abajo) |

- **Subida**: un solo commit atómico con la API Git Data (blobs → árbol → commit → mover la rama
  sin forzar). Si alguien se adelantó, vuelve a sincronizar y reintenta.
- **Tiempo real**: consulta la rama cada 4 s (configurable 2–60 s) con `ETag`; las respuestas
  `304 Not Modified` no gastan cuota de la API. Los cambios locales se detectan al guardar
  (con 1,5 s de antirrebote) y revisando el disco en cada ciclo.
- **Conflictos** (mismo archivo cambiado por dos personas entre sincronizaciones): gana tu versión y la
  remota se guarda en `.godotgit/conflicts/<fecha>/remote/…` (nada se pierde, también queda en el
  historial de git). En la **primera conexión** gana la del repo y la tuya queda en `…/local/…`.
- **Escenas abiertas**: si cambian en GitHub se recargan; si tienes cambios sin guardar en esa
  escena no se tocan y el registro te avisa.
- **Ignorados**: `.godot/`, `.git/`, `.godotgit/`, temporales, y lo que diga tu `.gitignore`
  (patrones simples: `*.ext`, `carpeta/`, `/ruta`; sin negaciones `!`). Si no hay `.gitignore` se crea uno.
- **Límite**: archivos de más de 50 MB no se sincronizan (usa Git LFS o exclúyelos).

## Límites conocidos

- Los cambios se sincronizan **al guardar** (Ctrl+S): Godot no escribe a disco lo que aún no has guardado.
- No hay fusión línea a línea: dos personas editando el mismo `.tscn`/`.gd` a la vez no se mezclan,
  se resuelve por conflicto de archivo (arriba). Para escenas, mejor repartir quién toca qué escena.
- No se guardan permisos de ejecución ni symlinks.
- Las integraciones de GitHub Enterprise Server (URL propia) no están soportadas, solo github.com.
- El plugin está escrito para la API pública de GitHub; si tu red filtra `api.github.com` no funcionará.

## Archivos

```
godotgit/
├── godotgit_setup.py            # crea/instala proyectos con el addon (+ repo preconfigurado)
└── addons/github_sync/
    ├── plugin.cfg
    ├── plugin.gd                # EditorPlugin: panel inferior + botón de estado en la barra
    ├── sync_dock.gd             # interfaz del panel «GitHub»
    ├── sync_manager.gd          # motor de sincronización (3 vías, push/pull, sondeo, ignorados)
    └── github_api.gd            # cliente mínimo de la API REST de GitHub
```
