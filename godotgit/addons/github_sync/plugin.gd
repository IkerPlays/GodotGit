@tool
extends EditorPlugin
## GodotGit — sincronización de proyecto con GitHub en tiempo real.

const GitHubApi := preload("github_api.gd")
const SyncManager := preload("sync_manager.gd")
const SyncDock := preload("sync_dock.gd")

var _api: GitHubApi
var _manager: SyncManager
var _dock: SyncDock
var _toolbar_button: Button


func _enter_tree() -> void:
	_api = GitHubApi.new()
	add_child(_api)

	_manager = SyncManager.new()
	_manager.api = _api
	add_child(_manager)

	_dock = SyncDock.new()
	_dock.manager = _manager
	add_control_to_bottom_panel(_dock, "GitHub")

	_toolbar_button = Button.new()
	_toolbar_button.flat = true
	_toolbar_button.text = "GitHub"
	_toolbar_button.tooltip_text = "Estado de la sincronización con GitHub (clic para abrir el panel)"
	_toolbar_button.pressed.connect(func() -> void: make_bottom_panel_item_visible(_dock))
	add_control_to_container(EditorPlugin.CONTAINER_TOOLBAR, _toolbar_button)

	_manager.status_changed.connect(_on_status_changed)
	scene_saved.connect(_on_scene_saved)
	resource_saved.connect(_on_resource_saved)

	# Esperamos un frame para que el editor termine de arrancar.
	_start_deferred.call_deferred()


func _start_deferred() -> void:
	if is_instance_valid(_manager):
		_manager.start()


func _exit_tree() -> void:
	if _manager:
		_manager.shutdown()
	if _dock:
		remove_control_from_bottom_panel(_dock)
		_dock.queue_free()
		_dock = null
	if _toolbar_button:
		remove_control_from_container(EditorPlugin.CONTAINER_TOOLBAR, _toolbar_button)
		_toolbar_button.queue_free()
		_toolbar_button = null
	if _manager:
		_manager.queue_free()
		_manager = null
	if _api:
		_api.queue_free()
		_api = null


func _on_scene_saved(_path: String) -> void:
	_manager.notify_local_change()


func _on_resource_saved(_resource: Resource) -> void:
	_manager.notify_local_change()


func _on_status_changed(text: String, state: int) -> void:
	if _toolbar_button == null:
		return
	var icon := "○"
	match state:
		SyncManager.State.IDLE:
			icon = "●"
		SyncManager.State.SYNCING:
			icon = "◐"
		SyncManager.State.ERROR:
			icon = "▲"
	_toolbar_button.text = "GitHub %s" % icon
	_toolbar_button.tooltip_text = text
