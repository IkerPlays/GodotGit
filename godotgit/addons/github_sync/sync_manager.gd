@tool
extends Node
## Motor de sincronización proyecto <-> GitHub.
##
## Modelo (igual que un "merge de 3 vías" a nivel de archivo):
##   base   = estado de cada archivo (SHA de blob de git) en la última sincronización
##   local  = estado actual en disco (res://)
##   remote = estado actual en la rama de GitHub
##
## Para cada ruta:
##   local == remote                  -> nada que hacer
##   local == base  (cambió remoto)   -> se descarga / se borra en local
##   remote == base (cambió local)    -> se sube / se borra en GitHub
##   ambos cambiaron                  -> conflicto: gana lo local (la versión remota se guarda en
##                                       .godotgit/conflicts/) — salvo en la primera conexión,
##                                       donde gana lo remoto para no pisar el repo existente.
##
## Los cambios locales se suben en UN solo commit (API Git Data) y se mueve la rama con
## avance rápido; si alguien empujó mientras tanto, se vuelve a sincronizar y se reintenta.
## El "tiempo real" se consigue consultando la rama con ETag (las respuestas 304 no gastan cuota).

const GitHubApi := preload("github_api.gd")

signal status_changed(text: String, state: int)
signal log_message(text: String, level: int)
signal activity_updated(entries: Array)
signal connection_changed(now_connected: bool)

enum State { DISCONNECTED, IDLE, SYNCING, ERROR }
enum Level { INFO, WARN, ERROR }

const DATA_DIR := "res://.godotgit"
const CONFIG_PATH := DATA_DIR + "/config.cfg"
const STATE_PATH := DATA_DIR + "/state.json"
const CONFLICTS_DIR := DATA_DIR + "/conflicts"

const SETTING_TOKEN := "godotgit/github_token"
const DEBOUNCE_SECONDS := 1.5
const MAX_FILE_BYTES := 50 * 1024 * 1024
const PARALLEL_REQUESTS := 6
const MAX_ATTEMPTS := 4

const ALWAYS_IGNORED_DIRS: Array[String] = [".godot", ".git", ".godotgit", ".import", ".mono", ".vs", ".idea"]
const ALWAYS_IGNORED_FILES: Array[String] = [".DS_Store", "Thumbs.db", "desktop.ini"]
const DEFAULT_GITIGNORE := "# Godot 4+\n.godot/\n/android/\n\n# Estado local de GodotGit\n.godotgit/\n\n# Temporales\n*.tmp\n"

var api: GitHubApi

var repo_owner: String = ""
var repo: String = ""
var branch: String = ""
var user_login: String = ""
var connected: bool = false
var busy: bool = false
var auto_sync: bool = true
var poll_seconds: float = 4.0
var last_sync_text: String = ""

var base_commit: String = ""
var base: Dictionary = {}    # ruta relativa -> SHA de blob
var cache: Dictionary = {}   # ruta relativa -> [mtime, tamaño, sha]
var ref_etag: String = ""

var _poll_timer: Timer
var _debounce_timer: Timer
var _again: bool = false
var _error_streak: int = 0
var _activity_dirty: bool = true
var _pending_refresh: Array = []
var _ignore_rules: Array = []
var _warned_big: Dictionary = {}
var _branch_ready: bool = false


## Ejecuta como máximo `concurrency` trabajos a la vez (cada uno es una corrutina).
class Pool extends RefCounted:
	signal finished
	var jobs: Array = []
	var worker: Callable
	var next_index: int = 0
	var running: int = 0

	func run(concurrency: int) -> void:
		var count: int = mini(concurrency, jobs.size())
		for i in count:
			_worker_loop()
		if running > 0:
			await finished

	func _worker_loop() -> void:
		running += 1
		while next_index < jobs.size():
			var job: Variant = jobs[next_index]
			next_index += 1
			await worker.call(job)
		running -= 1
		if running == 0:
			finished.emit()


# ============================================================ Ciclo de vida

func _ready() -> void:
	_poll_timer = Timer.new()
	_poll_timer.one_shot = false
	_poll_timer.wait_time = poll_seconds
	_poll_timer.timeout.connect(_on_poll)
	add_child(_poll_timer)

	_debounce_timer = Timer.new()
	_debounce_timer.one_shot = true
	_debounce_timer.wait_time = DEBOUNCE_SECONDS
	_debounce_timer.timeout.connect(_on_debounce)
	add_child(_debounce_timer)


func start() -> void:
	var fs := EditorInterface.get_resource_filesystem()
	if not fs.filesystem_changed.is_connected(notify_local_change):
		fs.filesystem_changed.connect(notify_local_change)
	if load_config():
		await resume()
	else:
		_emit_status("Sin conectar", State.DISCONNECTED)
		connection_changed.emit(false)


func shutdown() -> void:
	if _poll_timer:
		_poll_timer.stop()
	if _debounce_timer:
		_debounce_timer.stop()
	var fs := EditorInterface.get_resource_filesystem()
	if fs and fs.filesystem_changed.is_connected(notify_local_change):
		fs.filesystem_changed.disconnect(notify_local_change)
	connected = false


## Reanuda una conexión guardada en .godotgit/config.cfg.
func resume() -> void:
	api.token = load_token()
	if api.token == "":
		_emit_status("Falta el token de GitHub", State.ERROR)
		connection_changed.emit(false)
		return
	var me: Dictionary = await api.get_user()
	if not me.ok and me.code != 0:
		_log("No se pudo verificar el token: " + str(me.error), Level.ERROR)
		_emit_status("Token no válido", State.ERROR)
		connection_changed.emit(false)
		return
	if me.ok:
		user_login = str(me.data.get("login", ""))
	load_state()
	connected = true
	connection_changed.emit(true)
	_poll_timer.start(poll_seconds)
	if not me.ok:
		_emit_status("Sin conexión, reintentando…", State.ERROR)
		return
	await sync_now()


# ================================================================ Conexión

## Conecta con un repo existente. Devuelve "" si todo fue bien o el mensaje de error.
func connect_repo(owner_in: String, repo_in: String, branch_in: String, token_in: String) -> String:
	if busy:
		return "Hay una sincronización en curso."
	api.token = token_in.strip_edges()
	if api.token == "":
		return "Pega tu token de GitHub primero."

	var me: Dictionary = await api.get_user()
	if not me.ok:
		return "Token rechazado por GitHub: " + str(me.error)
	user_login = str(me.data.get("login", ""))

	var info: Dictionary = await api.get_repo(owner_in, repo_in)
	if not info.ok:
		if info.code == 404:
			return "No encuentro '%s/%s' (o tu token no tiene acceso a ese repo)." % [owner_in, repo_in]
		return "No se pudo leer el repo: " + str(info.error)
	var perms: Variant = info.data.get("permissions", {})
	if perms is Dictionary and not perms.get("push", false):
		return "Tu usuario no tiene permiso de escritura en ese repo. Pide que te inviten como colaborador."

	var default_branch := str(info.data.get("default_branch", "main"))
	var target_branch := branch_in.strip_edges()
	if target_branch == "":
		target_branch = default_branch

	var err := await _ensure_branch(owner_in, repo_in, target_branch, default_branch)
	if err != "":
		return err

	save_token(api.token)
	_branch_ready = true
	repo_owner = owner_in
	repo = repo_in
	branch = target_branch
	base_commit = ""
	base = {}
	cache = {}
	ref_etag = ""
	_activity_dirty = true
	save_config()
	save_state()

	connected = true
	connection_changed.emit(true)
	_log("Conectado a %s/%s (rama %s) como %s" % [repo_owner, repo, branch, user_login], Level.INFO)
	_poll_timer.start(poll_seconds)
	await sync_now()
	return ""


## Crea un repo nuevo en tu cuenta y conecta el proyecto a él.
func create_and_connect(repo_name: String, is_private: bool, token_in: String) -> String:
	api.token = token_in.strip_edges()
	if api.token == "":
		return "Pega tu token de GitHub primero."
	var me: Dictionary = await api.get_user()
	if not me.ok:
		return "Token rechazado por GitHub: " + str(me.error)
	var created: Dictionary = await api.create_repo(repo_name.strip_edges(), is_private, "Proyecto Godot")
	if not created.ok:
		if created.code == 422:
			return "No se pudo crear '%s': ya existe un repo con ese nombre." % repo_name
		return "No se pudo crear el repo: " + str(created.error)
	var full: Array = str(created.data.get("full_name", "")).split("/")
	if full.size() != 2:
		return "Respuesta inesperada de GitHub al crear el repo."
	# GitHub tarda un instante en tener lista la rama inicial.
	await get_tree().create_timer(1.5).timeout
	return await connect_repo(str(full[0]), str(full[1]), "", api.token)


func disconnect_repo() -> void:
	_poll_timer.stop()
	_debounce_timer.stop()
	connected = false
	busy = false
	_branch_ready = false
	base = {}
	cache = {}
	base_commit = ""
	for path in [CONFIG_PATH, STATE_PATH]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	_log("Desconectado. Tus archivos locales y el repo quedan como están.", Level.INFO)
	_emit_status("Sin conectar", State.DISCONNECTED)
	connection_changed.emit(false)


## Resuelve la rama por defecto (si config.cfg no la trae) y se asegura de que exista.
func _prepare_branch() -> String:
	var info: Dictionary = await api.get_repo(repo_owner, repo)
	if not info.ok:
		return "No se pudo leer %s/%s: %s" % [repo_owner, repo, str(info.error)]
	var default_branch := str(info.data.get("default_branch", "main"))
	if branch == "":
		branch = default_branch
		save_config()
	return await _ensure_branch(repo_owner, repo, branch, default_branch)


func _ensure_branch(o: String, r: String, wanted: String, default_branch: String) -> String:
	var ref: Dictionary = await api.get_ref(o, r, wanted)
	if ref.ok:
		return ""
	# ¿Repo completamente vacío? (sin ramas)
	var branches: Dictionary = await api.list_branches(o, r)
	var is_empty: bool = (branches.ok and branches.data is Array and branches.data.is_empty()) or ref.code == 409
	if is_empty:
		_log("El repo está vacío: creo el primer commit.", Level.INFO)
		var first_commit: Dictionary = await api.put_file(o, r, ".gitignore", DEFAULT_GITIGNORE.to_utf8_buffer(), "Commit inicial (Godot)")
		if not first_commit.ok:
			return "No se pudo inicializar el repo vacío: " + str(first_commit.error)
		if wanted == default_branch:
			return ""
		ref = await api.get_ref(o, r, default_branch)
		if not ref.ok:
			return "No se pudo leer la rama inicial: " + str(ref.error)
	elif wanted == default_branch:
		return "No se pudo leer la rama '%s': %s" % [wanted, str(ref.error)]
	else:
		ref = await api.get_ref(o, r, default_branch)
		if not ref.ok:
			return "No se pudo leer la rama '%s': %s" % [default_branch, str(ref.error)]
	# La rama pedida no existe: se crea a partir de la rama por defecto.
	var sha := str(ref.data["object"]["sha"])
	var made: Dictionary = await api.create_ref(o, r, wanted, sha)
	if not made.ok:
		return "No se pudo crear la rama '%s': %s" % [wanted, str(made.error)]
	_log("Rama '%s' creada a partir de '%s'." % [wanted, default_branch], Level.INFO)
	return ""


func invite(username: String) -> void:
	var who := username.strip_edges().trim_prefix("@")
	if who == "" or not connected:
		return
	var res: Dictionary = await api.add_collaborator(repo_owner, repo, who)
	if res.ok:
		if res.code == 204:
			_log("%s ya es colaborador de %s/%s." % [who, repo_owner, repo], Level.INFO)
		else:
			_log("Invitación enviada a %s. Debe aceptarla (le llega por email y en github.com/notifications)." % who, Level.INFO)
	else:
		_log("No se pudo invitar a %s: %s (hace falta ser admin del repo)" % [who, str(res.error)], Level.ERROR)


# =========================================================== Disparadores

func notify_local_change() -> void:
	if not connected or not auto_sync:
		return
	if busy:
		_again = true
		return
	_debounce_timer.start(DEBOUNCE_SECONDS)


func set_poll_seconds(seconds: float) -> void:
	poll_seconds = clampf(seconds, 2.0, 60.0)
	if connected:
		_poll_timer.start(poll_seconds)
	save_config()


func set_auto_sync(enabled: bool) -> void:
	auto_sync = enabled
	save_config()


func _on_debounce() -> void:
	if not connected or not auto_sync:
		return
	if busy:
		_again = true
		return
	if _local_dirty():
		await sync_now()


func _on_poll() -> void:
	if not connected or busy or not auto_sync:
		return
	if not _branch_ready:
		await sync_now()
		return
	var remote_state: int = await _remote_changed()
	if remote_state == -1:
		_error_streak += 1
		_poll_timer.start(minf(poll_seconds * pow(2.0, _error_streak), 60.0))
		_emit_status("Sin conexión con GitHub, reintentando…", State.ERROR)
		return
	if _error_streak > 0:
		_error_streak = 0
		_poll_timer.start(poll_seconds)
	if remote_state == 1 or _local_dirty():
		await sync_now()


## 1 = la rama avanzó, 0 = sin cambios, -1 = error de red/API.
func _remote_changed() -> int:
	var res: Dictionary = await api.get_ref(repo_owner, repo, branch, ref_etag)
	if res.code == 304:
		return 0
	if not res.ok:
		return -1
	ref_etag = str(res.headers.get("etag", ""))
	return 1 if str(res.data["object"]["sha"]) != base_commit else 0


# ============================================================ Sincronizar

func sync_now() -> bool:
	if not connected:
		return false
	if busy:
		_again = true
		return false
	busy = true
	_emit_status("Sincronizando…", State.SYNCING)

	var result := "error"
	if not _branch_ready:
		var prep_error := await _prepare_branch()
		if prep_error != "":
			_log(prep_error, Level.ERROR)
			busy = false
			_error_streak += 1
			_poll_timer.start(minf(poll_seconds * pow(2.0, _error_streak), 60.0))
			_emit_status("Error al preparar la rama (mira el registro)", State.ERROR)
			return false
		_branch_ready = true
	for attempt in MAX_ATTEMPTS:
		result = await _sync_once()
		if result != "retry":
			break
	busy = false
	_flush_editor_refresh()

	if result == "ok":
		_error_streak = 0
		_poll_timer.start(poll_seconds)
		last_sync_text = Time.get_time_string_from_system()
		_emit_status("Sincronizado · %s" % last_sync_text, State.IDLE)
		if _activity_dirty:
			_activity_dirty = false
			refresh_activity()
	else:
		_error_streak += 1
		_poll_timer.start(minf(poll_seconds * pow(2.0, _error_streak), 60.0))
		_emit_status("Error al sincronizar (mira el registro)", State.ERROR)
	save_state()

	if _again and result == "ok":
		_again = false
		call_deferred("sync_now")
	else:
		_again = false
	return result == "ok"


## Una pasada completa. Devuelve "ok", "retry" (la rama se movió) o "error".
func _sync_once() -> String:
	var first_sync := base_commit == ""
	_load_ignore_rules()

	# 1) Cabeza remota
	var ref: Dictionary = await api.get_ref(repo_owner, repo, branch)
	if not ref.ok:
		_log("No se pudo leer la rama '%s': %s" % [branch, str(ref.error)], Level.ERROR)
		return "error"
	var head_sha := str(ref.data["object"]["sha"])
	ref_etag = str(ref.headers.get("etag", ""))

	# 2) Árbol remoto
	var remote: Dictionary = {}
	var tree_sha := ""
	if head_sha == base_commit:
		remote = base.duplicate()
	else:
		var commit: Dictionary = await api.get_commit(repo_owner, repo, head_sha)
		if not commit.ok:
			_log("No se pudo leer el commit remoto: " + str(commit.error), Level.ERROR)
			return "error"
		tree_sha = str(commit.data["tree"]["sha"])
		var tree: Dictionary = await api.get_tree(repo_owner, repo, tree_sha)
		if not tree.ok:
			_log("No se pudo leer el árbol remoto: " + str(tree.error), Level.ERROR)
			return "error"
		if tree.data.get("truncated", false):
			_log("El repo es enorme y GitHub devolvió el árbol incompleto; algunos archivos no se sincronizarán.", Level.WARN)
		for item in tree.data["tree"]:
			if str(item.get("type", "")) != "blob" or str(item.get("mode", "")) == "120000":
				continue
			var rel := str(item["path"])
			if _remote_path_ignored(rel):
				continue
			if int(item.get("size", 0)) > MAX_FILE_BYTES:
				_warn_big(rel)
				continue
			remote[rel] = str(item["sha"])

	# 3) Estado local
	var local: Dictionary = _scan_local()
	if first_sync and not remote.has(".gitignore") and not local.has(".gitignore"):
		_write_file(".gitignore", DEFAULT_GITIGNORE.to_utf8_buffer())
		_load_ignore_rules()
		local = _scan_local()

	# 4) Plan
	var downloads: Array = []       # {path, sha, apply, save_local, save_remote}
	var local_deletes: Array = []
	var pushes: Dictionary = {}     # ruta -> sha local ("" = borrar en remoto)
	var in_sync: Array = []
	var conflicts: Array = []
	var all_paths: Dictionary = {}
	for p in base:
		all_paths[p] = true
	for p in local:
		all_paths[p] = true
	for p in remote:
		all_paths[p] = true

	for p in all_paths:
		var b: String = base.get(p, "")
		var l: String = local.get(p, "")
		var r: String = remote.get(p, "")
		if l == r:
			in_sync.append(p)
		elif l == b:
			if r == "":
				local_deletes.append(p)
			else:
				downloads.append({"path": p, "sha": r, "apply": true, "save_local": false, "save_remote": false})
		elif r == b:
			pushes[p] = l
		else:
			conflicts.append(p)
			if first_sync:
				downloads.append({"path": p, "sha": r, "apply": true, "save_local": true, "save_remote": false})
			else:
				pushes[p] = l
				if r != "":
					downloads.append({"path": p, "sha": r, "apply": false, "save_local": false, "save_remote": true})

	# 5) Descargar lo remoto
	var fetched: Dictionary = {}
	var flags := {"failed": false}
	if not downloads.is_empty():
		_emit_status("Descargando %d archivo(s)…" % downloads.size(), State.SYNCING)
		var pool := Pool.new()
		pool.jobs = downloads
		pool.worker = func(job: Dictionary) -> void:
			if flags["failed"]:
				return
			var res: Dictionary = await api.get_blob(repo_owner, repo, str(job["sha"]))
			if not res.ok:
				flags["failed"] = true
				_log("No se pudo descargar %s: %s" % [job["path"], str(res.error)], Level.ERROR)
				return
			var content := str(res.data.get("content", ""))
			var bytes := PackedByteArray()
			if str(res.data.get("encoding", "")) == "base64":
				bytes = Marshalls.base64_to_raw(content.replace("\n", "").replace("\r", ""))
			else:
				bytes = content.to_utf8_buffer()
			if git_blob_sha(bytes) != str(job["sha"]):
				flags["failed"] = true
				_log("La descarga de %s llegó corrupta." % job["path"], Level.ERROR)
				return
			fetched[job["path"]] = bytes
		await pool.run(PARALLEL_REQUESTS)
		if flags["failed"]:
			return "error"

	# 6) Aplicar lo remoto en local
	var stamp := Time.get_datetime_string_from_system().replace(":", "-").replace("T", "_")
	for job in downloads:
		var rel: String = job["path"]
		var bytes: PackedByteArray = fetched[rel]
		if job["save_local"]:
			_save_conflict_copy(rel, FileAccess.get_file_as_bytes("res://" + rel), stamp, "local")
		if job["save_remote"]:
			_save_conflict_copy(rel, bytes, stamp, "remote")
		if job["apply"]:
			if _write_file(rel, bytes):
				base[rel] = job["sha"]
				_pending_refresh.append(rel)
			else:
				return "error"
	for rel in local_deletes:
		DirAccess.remove_absolute("res://" + rel)
		_remove_empty_parents(rel)
		base.erase(rel)
		cache.erase(rel)
		_pending_refresh.append(rel)
	for rel in in_sync:
		var l_sha: String = local.get(rel, "")
		if l_sha == "":
			base.erase(rel)
		else:
			base[rel] = l_sha
	if not conflicts.is_empty():
		var how := "se conserva tu versión y la remota queda en .godotgit/conflicts/" if not first_sync \
			else "se usa la versión del repo y la tuya queda en .godotgit/conflicts/"
		_log("%d conflicto(s): %s (%s)" % [conflicts.size(), how, ", ".join(PackedStringArray(conflicts.slice(0, 5)))], Level.WARN)
	if not downloads.is_empty() or not local_deletes.is_empty():
		var pulled := 0
		for job in downloads:
			if job["apply"]:
				pulled += 1
		_log("Recibido de GitHub: %d archivo(s) nuevos/modificados, %d borrado(s)." % [pulled, local_deletes.size()], Level.INFO)
		_activity_dirty = true

	# 7) Subir lo local
	if pushes.is_empty():
		if base_commit != head_sha:
			_activity_dirty = true
		base_commit = head_sha
		return "ok"

	_emit_status("Subiendo %d archivo(s)…" % pushes.size(), State.SYNCING)
	var blob_jobs: Array = []
	for p in pushes:
		if pushes[p] != "":
			blob_jobs.append(p)
	var blob_shas: Dictionary = {}
	if not blob_jobs.is_empty():
		var pool2 := Pool.new()
		pool2.jobs = blob_jobs
		pool2.worker = func(rel: String) -> void:
			if flags["failed"]:
				return
			var data := FileAccess.get_file_as_bytes("res://" + rel)
			var res: Dictionary = await api.create_blob(repo_owner, repo, data)
			if not res.ok:
				flags["failed"] = true
				_log("No se pudo subir %s: %s" % [rel, str(res.error)], Level.ERROR)
				return
			blob_shas[rel] = str(res.data["sha"])
		await pool2.run(PARALLEL_REQUESTS)
		if flags["failed"]:
			return "error"

	var entries: Array = []
	for p in pushes:
		if pushes[p] == "":
			entries.append({"path": p, "mode": "100644", "type": "blob", "sha": null})
		else:
			entries.append({"path": p, "mode": "100644", "type": "blob", "sha": blob_shas[p]})

	if tree_sha == "":
		var head_commit: Dictionary = await api.get_commit(repo_owner, repo, head_sha)
		if not head_commit.ok:
			_log("No se pudo leer el commit remoto: " + str(head_commit.error), Level.ERROR)
			return "error"
		tree_sha = str(head_commit.data["tree"]["sha"])

	var new_tree: Dictionary = await api.create_tree(repo_owner, repo, tree_sha, entries)
	if not new_tree.ok:
		_log("No se pudo crear el árbol: " + str(new_tree.error), Level.ERROR)
		return "error"
	var message := _make_commit_message(pushes)
	var new_commit: Dictionary = await api.create_commit(repo_owner, repo, message, str(new_tree.data["sha"]), [head_sha])
	if not new_commit.ok:
		_log("No se pudo crear el commit: " + str(new_commit.error), Level.ERROR)
		return "error"
	var moved: Dictionary = await api.update_ref(repo_owner, repo, branch, str(new_commit.data["sha"]))
	if not moved.ok:
		if moved.code == 422 or moved.code == 409:
			_log("Alguien subió cambios a la vez; vuelvo a sincronizar…", Level.INFO)
			return "retry"
		_log("No se pudo actualizar la rama: " + str(moved.error), Level.ERROR)
		return "error"

	for p in pushes:
		if pushes[p] == "":
			base.erase(p)
		else:
			base[p] = pushes[p]
	base_commit = str(new_commit.data["sha"])
	ref_etag = ""
	_activity_dirty = true
	_log("Subido a GitHub: %s (%s)" % [message.get_slice("\n", 0), base_commit.substr(0, 7)], Level.INFO)
	return "ok"


func _make_commit_message(pushes: Dictionary) -> String:
	var lines: PackedStringArray = []
	var summary := ""
	for p in pushes:
		var verb := "Actualizar"
		if pushes[p] == "":
			verb = "Borrar"
		elif not base.has(p):
			verb = "Añadir"
		lines.append("%s %s" % [verb, p])
	lines.sort()
	if lines.size() == 1:
		summary = lines[0]
	else:
		summary = "%d cambios en el proyecto" % lines.size()
	if lines.size() == 1:
		return "[Godot] " + summary
	var shown := lines.slice(0, 60)
	var body := "\n".join(shown)
	if lines.size() > shown.size():
		body += "\n… y %d más" % (lines.size() - shown.size())
	return "[Godot] %s\n\n%s" % [summary, body]


# ============================================================ Archivos locales

func _local_dirty() -> bool:
	_load_ignore_rules()
	var local := _scan_local()
	if local.size() != base.size():
		return true
	for p in local:
		if str(base.get(p, "")) != str(local[p]):
			return true
	return false


func _scan_local() -> Dictionary:
	var result: Dictionary = {}
	_scan_dir("res://", result)
	for rel in cache.keys():
		if not result.has(rel):
			cache.erase(rel)
	return result


func _scan_dir(dir_path: String, result: Dictionary) -> void:
	var dir := DirAccess.open(dir_path)
	if dir == null:
		return
	dir.include_hidden = true
	dir.include_navigational = false
	var now := Time.get_unix_time_from_system()
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var full := dir_path.path_join(entry)
		var rel := full.trim_prefix("res://")
		if dir.current_is_dir():
			if not _path_ignored(rel, true):
				_scan_dir(full, result)
		elif not _path_ignored(rel, false):
			var mtime := FileAccess.get_modified_time(full)
			var cached: Variant = cache.get(rel)
			var sha := ""
			if cached is Array and cached.size() == 3 and int(cached[0]) == mtime and now - mtime > 2.0:
				sha = str(cached[2])
			else:
				var file := FileAccess.open(full, FileAccess.READ)
				if file != null:
					var size := file.get_length()
					if size > MAX_FILE_BYTES:
						_warn_big(rel)
					else:
						var bytes := file.get_buffer(size)
						sha = git_blob_sha(bytes)
						cache[rel] = [mtime, size, sha]
					file.close()
			if sha != "":
				result[rel] = sha
		entry = dir.get_next()
	dir.list_dir_end()


func _write_file(rel: String, bytes: PackedByteArray) -> bool:
	var path := "res://" + rel
	var dir := path.get_base_dir()
	if not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_log("No se pudo escribir %s (error %d)" % [rel, FileAccess.get_open_error()], Level.ERROR)
		return false
	file.store_buffer(bytes)
	file.close()
	cache[rel] = [FileAccess.get_modified_time(path), bytes.size(), git_blob_sha(bytes)]
	return true


func _save_conflict_copy(rel: String, bytes: PackedByteArray, stamp: String, tag: String) -> void:
	var path := "%s/%s/%s/%s" % [CONFLICTS_DIR, stamp, tag, rel]
	var dir := path.get_base_dir()
	if not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_buffer(bytes)
		file.close()


func _remove_empty_parents(rel: String) -> void:
	var dir_rel := rel.get_base_dir()
	while dir_rel != "":
		var dir := DirAccess.open("res://" + dir_rel)
		if dir == null:
			return
		dir.include_hidden = true
		if dir.get_files().size() > 0 or dir.get_directories().size() > 0:
			return
		DirAccess.remove_absolute("res://" + dir_rel)
		dir_rel = dir_rel.get_base_dir()


static func git_blob_sha(data: PackedByteArray) -> String:
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA1)
	ctx.update(("blob %d" % data.size()).to_utf8_buffer() + PackedByteArray([0]))
	ctx.update(data)
	return ctx.finish().hex_encode()


func _warn_big(rel: String) -> void:
	if not _warned_big.has(rel):
		_warned_big[rel] = true
		_log("'%s' pesa más de 50 MB: no se sincroniza (usa Git LFS o exclúyelo en .gitignore)." % rel, Level.WARN)


# ============================================================ .gitignore

func _load_ignore_rules() -> void:
	_ignore_rules.clear()
	var path := "res://.gitignore"
	if not FileAccess.file_exists(path):
		return
	for raw_line in FileAccess.get_file_as_string(path).split("\n"):
		var line := raw_line.strip_edges()
		if line == "" or line.begins_with("#") or line.begins_with("!"):
			continue
		var dir_only := line.ends_with("/")
		if dir_only:
			line = line.trim_suffix("/")
		var anchored := line.contains("/")
		line = line.trim_prefix("/")
		if line == "":
			continue
		_ignore_rules.append({"pattern": line, "dir_only": dir_only, "anchored": anchored})


## Evalúa solo el último segmento de `rel` (los padres ya se filtraron al recorrer).
func _path_ignored(rel: String, is_dir: bool) -> bool:
	var leaf := rel.get_file()
	if is_dir:
		if ALWAYS_IGNORED_DIRS.has(leaf):
			return true
	else:
		if ALWAYS_IGNORED_FILES.has(leaf) or leaf.ends_with(".tmp") or leaf.ends_with(".TMP"):
			return true
	for rule in _ignore_rules:
		if rule["dir_only"] and not is_dir:
			continue
		var pattern: String = rule["pattern"]
		if rule["anchored"]:
			if rel.match(pattern):
				return true
		elif leaf.match(pattern):
			return true
	return false


func _remote_path_ignored(rel: String) -> bool:
	var parts := rel.split("/")
	var acc := ""
	for i in parts.size():
		acc = parts[i] if acc == "" else acc + "/" + parts[i]
		if _path_ignored(acc, i < parts.size() - 1):
			return true
	return false


# ========================================================== Refrescar editor

func _flush_editor_refresh() -> void:
	if _pending_refresh.is_empty():
		return
	var paths := _pending_refresh.duplicate()
	_pending_refresh.clear()

	var fs := EditorInterface.get_resource_filesystem()
	if fs != null and not fs.is_scanning():
		fs.scan()

	var open_scenes := EditorInterface.get_open_scenes()
	var unsaved := EditorInterface.get_unsaved_scenes()
	for rel in paths:
		var res_path := "res://" + str(rel)
		if open_scenes.has(res_path) and FileAccess.file_exists(res_path):
			if unsaved.has(res_path):
				_log("'%s' cambió en GitHub pero tienes cambios sin guardar en esa escena. Guárdala o recárgala a mano." % rel, Level.WARN)
			else:
				EditorInterface.reload_scene_from_path(res_path)
	var script_editor := EditorInterface.get_script_editor()
	if script_editor != null:
		script_editor.reload_open_files()


# ============================================================ Actividad

func refresh_activity() -> void:
	if not connected:
		return
	var res: Dictionary = await api.list_commits(repo_owner, repo, branch, 25)
	if not res.ok or not (res.data is Array):
		return
	var entries: Array = []
	for item in res.data:
		var commit: Dictionary = item.get("commit", {})
		var author_info: Dictionary = commit.get("author", {})
		var who := ""
		if item.get("author") is Dictionary:
			who = str(item["author"].get("login", ""))
		if who == "":
			who = str(author_info.get("name", "?"))
		entries.append({
			"sha": str(item.get("sha", "")).substr(0, 7),
			"message": str(commit.get("message", "")).get_slice("\n", 0),
			"author": who,
			"date": str(author_info.get("date", "")),
		})
	activity_updated.emit(entries)


# ============================================================ Persistencia

static func load_token() -> String:
	var settings := EditorInterface.get_editor_settings()
	if settings.has_setting(SETTING_TOKEN):
		var saved := str(settings.get_setting(SETTING_TOKEN))
		if saved != "":
			return saved
	return OS.get_environment("GITHUB_TOKEN")


static func save_token(token: String) -> void:
	EditorInterface.get_editor_settings().set_setting(SETTING_TOKEN, token)


func _ensure_data_dir() -> void:
	if not DirAccess.dir_exists_absolute(DATA_DIR):
		DirAccess.make_dir_recursive_absolute(DATA_DIR)
	# .gdignore: que el editor no escanee esta carpeta como parte del proyecto.
	var marker := DATA_DIR + "/.gdignore"
	if not FileAccess.file_exists(marker):
		var file := FileAccess.open(marker, FileAccess.WRITE)
		if file != null:
			file.close()


func save_config() -> void:
	_ensure_data_dir()
	var cfg := ConfigFile.new()
	cfg.set_value("github", "owner", repo_owner)
	cfg.set_value("github", "repo", repo)
	cfg.set_value("github", "branch", branch)
	cfg.set_value("sync", "auto", auto_sync)
	cfg.set_value("sync", "poll_seconds", poll_seconds)
	cfg.save(CONFIG_PATH)


func load_config() -> bool:
	var cfg := ConfigFile.new()
	if cfg.load(CONFIG_PATH) != OK:
		return false
	repo_owner = str(cfg.get_value("github", "owner", ""))
	repo = str(cfg.get_value("github", "repo", ""))
	branch = str(cfg.get_value("github", "branch", ""))
	auto_sync = bool(cfg.get_value("sync", "auto", true))
	poll_seconds = float(cfg.get_value("sync", "poll_seconds", 4.0))
	return repo_owner != "" and repo != ""


func save_state() -> void:
	if not connected:
		return
	_ensure_data_dir()
	var file := FileAccess.open(STATE_PATH, FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify({"base_commit": base_commit, "base": base, "cache": cache}))
	file.close()


func load_state() -> void:
	base_commit = ""
	base = {}
	cache = {}
	if not FileAccess.file_exists(STATE_PATH):
		return
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(STATE_PATH))
	if parsed is Dictionary:
		base_commit = str(parsed.get("base_commit", ""))
		if parsed.get("base") is Dictionary:
			base = parsed["base"]
		if parsed.get("cache") is Dictionary:
			cache = parsed["cache"]


## Acepta "usuario/repo", "https://github.com/usuario/repo(.git)" o "git@github.com:usuario/repo.git".
## Devuelve [owner, repo] o [] si no se entiende.
static func parse_repo(text: String) -> Array:
	var t := text.strip_edges().trim_suffix("/").trim_suffix(".git")
	for prefix in ["https://github.com/", "http://github.com/", "git@github.com:", "github.com/"]:
		if t.begins_with(prefix):
			t = t.trim_prefix(prefix)
			break
	var parts := t.split("/")
	if parts.size() < 2 or parts[0] == "" or parts[1] == "":
		return []
	return [parts[0], parts[1].trim_suffix(".git")]


# ============================================================ Utilidades

func _emit_status(text: String, state: int) -> void:
	status_changed.emit(text, state)


func _log(text: String, level: int) -> void:
	log_message.emit(text, level)
	print("[GodotGit] ", text)
