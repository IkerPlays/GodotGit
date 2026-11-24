@tool
extends VBoxContainer
## Panel "GitHub" del editor: conectar un repo, ver el estado, la actividad de los
## colaboradores y el registro de sincronización.

const SyncManager := preload("sync_manager.gd")

const TOKEN_URL := "https://github.com/settings/tokens/new?scopes=repo&description=GodotGit"

var manager: SyncManager

# --- Vista "sin conectar"
var _connect_box: VBoxContainer
var _token_edit: LineEdit
var _repo_edit: LineEdit
var _branch_edit: LineEdit
var _new_name_edit: LineEdit
var _private_check: CheckBox
var _connect_button: Button
var _create_button: Button
var _connect_error: Label

# --- Vista "conectado"
var _connected_box: HBoxContainer
var _repo_label: Label
var _status_label: Label
var _sync_button: Button
var _auto_check: CheckBox
var _poll_spin: SpinBox
var _invite_edit: LineEdit
var _invite_button: Button
var _activity: RichTextLabel
var _log: RichTextLabel


func _ready() -> void:
	name = "GitHub"
	custom_minimum_size = Vector2(0, 230)
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	_build_connect_box()
	_build_connected_box()

	manager.connection_changed.connect(_on_connection_changed)
	manager.status_changed.connect(_on_status_changed)
	manager.log_message.connect(_on_log)
	manager.activity_updated.connect(_on_activity)
	_on_connection_changed(manager.connected)


# ================================================================ Construcción

func _build_connect_box() -> void:
	_connect_box = VBoxContainer.new()
	_connect_box.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(_connect_box)

	var intro := Label.new()
	intro.text = "Conecta este proyecto a GitHub: todo lo que crees, guardes o borres se sube solo, y lo que hagan tus colaboradores aparecerá aquí en segundos."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_connect_box.add_child(intro)

	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 12)
	_connect_box.add_child(grid)

	grid.add_child(_make_label("Token de GitHub"))
	var token_row := HBoxContainer.new()
	token_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(token_row)
	_token_edit = LineEdit.new()
	_token_edit.secret = true
	_token_edit.placeholder_text = "ghp_… o github_pat_…"
	_token_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_token_edit.text = SyncManager.load_token()
	token_row.add_child(_token_edit)
	var token_link := LinkButton.new()
	token_link.text = "Crear token"
	token_link.uri = TOKEN_URL
	token_row.add_child(token_link)

	grid.add_child(_make_label("Repo existente"))
	var repo_row := HBoxContainer.new()
	repo_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(repo_row)
	_repo_edit = LineEdit.new()
	_repo_edit.placeholder_text = "usuario/repositorio  o  https://github.com/usuario/repositorio"
	_repo_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	repo_row.add_child(_repo_edit)
	_branch_edit = LineEdit.new()
	_branch_edit.placeholder_text = "rama (opcional)"
	_branch_edit.custom_minimum_size = Vector2(130, 0)
	repo_row.add_child(_branch_edit)
	_connect_button = Button.new()
	_connect_button.text = "Conectar"
	_connect_button.pressed.connect(_on_connect_pressed)
	repo_row.add_child(_connect_button)

	grid.add_child(_make_label("o crear uno nuevo"))
	var new_row := HBoxContainer.new()
	new_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(new_row)
	_new_name_edit = LineEdit.new()
	_new_name_edit.text = _suggest_repo_name()
	_new_name_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	new_row.add_child(_new_name_edit)
	_private_check = CheckBox.new()
	_private_check.text = "Privado"
	_private_check.button_pressed = true
	new_row.add_child(_private_check)
	_create_button = Button.new()
	_create_button.text = "Crear y conectar"
	_create_button.pressed.connect(_on_create_pressed)
	new_row.add_child(_create_button)

	_connect_error = Label.new()
	_connect_error.add_theme_color_override("font_color", Color(1.0, 0.45, 0.4))
	_connect_error.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_connect_box.add_child(_connect_error)

	var hint := Label.new()
	hint.text = "El token (permiso «repo») se guarda solo en la configuración de tu editor, nunca en el proyecto ni en GitHub."
	hint.modulate = Color(1, 1, 1, 0.55)
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_connect_box.add_child(hint)


func _build_connected_box() -> void:
	_connected_box = HBoxContainer.new()
	_connected_box.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_connected_box.add_theme_constant_override("separation", 14)
	add_child(_connected_box)

	# Columna izquierda: controles
	var left := VBoxContainer.new()
	left.custom_minimum_size = Vector2(300, 0)
	_connected_box.add_child(left)

	_repo_label = Label.new()
	_repo_label.add_theme_font_size_override("font_size", 16)
	left.add_child(_repo_label)

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	left.add_child(_status_label)

	var buttons := HBoxContainer.new()
	left.add_child(buttons)
	_sync_button = Button.new()
	_sync_button.text = "Sincronizar ahora"
	_sync_button.pressed.connect(func() -> void: manager.sync_now())
	buttons.add_child(_sync_button)
	var open_button := Button.new()
	open_button.text = "Abrir en GitHub"
	open_button.pressed.connect(func() -> void:
		OS.shell_open("https://github.com/%s/%s/tree/%s" % [manager.repo_owner, manager.repo, manager.branch]))
	buttons.add_child(open_button)
	var disconnect_button := Button.new()
	disconnect_button.text = "Desconectar"
	disconnect_button.pressed.connect(func() -> void: manager.disconnect_repo())
	buttons.add_child(disconnect_button)

	var options := HBoxContainer.new()
	left.add_child(options)
	_auto_check = CheckBox.new()
	_auto_check.text = "Sincronizar automáticamente"
	_auto_check.button_pressed = manager.auto_sync
	_auto_check.toggled.connect(func(on: bool) -> void: manager.set_auto_sync(on))
	options.add_child(_auto_check)
	options.add_child(_make_label("cada"))
	_poll_spin = SpinBox.new()
	_poll_spin.min_value = 2
	_poll_spin.max_value = 60
	_poll_spin.step = 1
	_poll_spin.suffix = "s"
	_poll_spin.value = manager.poll_seconds
	_poll_spin.value_changed.connect(func(v: float) -> void: manager.set_poll_seconds(v))
	options.add_child(_poll_spin)

	var invite_row := HBoxContainer.new()
	left.add_child(invite_row)
	_invite_edit = LineEdit.new()
	_invite_edit.placeholder_text = "usuario de GitHub a invitar"
	_invite_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	invite_row.add_child(_invite_edit)
	_invite_button = Button.new()
	_invite_button.text = "Invitar"
	_invite_button.pressed.connect(_on_invite_pressed)
	invite_row.add_child(_invite_button)

	# Centro: actividad
	var middle := VBoxContainer.new()
	middle.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_connected_box.add_child(middle)
	middle.add_child(_make_label("Actividad del equipo (últimos commits)"))
	_activity = _make_rich_text()
	middle.add_child(_activity)

	# Derecha: registro
	var right := VBoxContainer.new()
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_connected_box.add_child(right)
	right.add_child(_make_label("Registro"))
	_log = _make_rich_text()
	right.add_child(_log)


func _make_label(text: String) -> Label:
	var label := Label.new()
	label.text = text
	return label


func _make_rich_text() -> RichTextLabel:
	var rich := RichTextLabel.new()
	rich.bbcode_enabled = true
	rich.scroll_following = true
	rich.selection_enabled = true
	rich.size_flags_vertical = Control.SIZE_EXPAND_FILL
	rich.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	return rich


func _suggest_repo_name() -> String:
	var project_name := str(ProjectSettings.get_setting("application/config/name", "godot-project"))
	var slug := project_name.to_lower().strip_edges().replace(" ", "-")
	var clean := ""
	for ch in slug:
		if (ch >= "a" and ch <= "z") or (ch >= "0" and ch <= "9") or ch == "-" or ch == "_":
			clean += ch
	return clean if clean != "" else "godot-project"


# ================================================================ Acciones

func _on_connect_pressed() -> void:
	var parsed := SyncManager.parse_repo(_repo_edit.text)
	if parsed.is_empty():
		_connect_error.text = "Escribe el repo como «usuario/repositorio» o pega su URL."
		return
	_set_connecting(true)
	var err := await manager.connect_repo(str(parsed[0]), str(parsed[1]), _branch_edit.text, _token_edit.text)
	_set_connecting(false)
	_connect_error.text = err


func _on_create_pressed() -> void:
	var repo_name := _new_name_edit.text.strip_edges()
	if repo_name == "":
		_connect_error.text = "Ponle un nombre al repositorio nuevo."
		return
	_set_connecting(true)
	var err := await manager.create_and_connect(repo_name, _private_check.button_pressed, _token_edit.text)
	_set_connecting(false)
	_connect_error.text = err


func _on_invite_pressed() -> void:
	var who := _invite_edit.text
	_invite_edit.text = ""
	await manager.invite(who)


func _set_connecting(active: bool) -> void:
	_connect_button.disabled = active
	_create_button.disabled = active
	if active:
		_connect_error.text = "Conectando…"


# ================================================================ Señales

func _on_connection_changed(now_connected: bool) -> void:
	_connect_box.visible = not now_connected
	_connected_box.visible = now_connected
	if now_connected:
		_repo_label.text = "%s/%s  ·  %s" % [manager.repo_owner, manager.repo, manager.branch]
		_auto_check.set_pressed_no_signal(manager.auto_sync)
		_poll_spin.set_value_no_signal(manager.poll_seconds)
		_activity.clear()
		_log.clear()
	else:
		_token_edit.text = SyncManager.load_token()
		if manager.repo_owner != "":
			_repo_edit.text = "%s/%s" % [manager.repo_owner, manager.repo]
			_branch_edit.text = manager.branch


func _on_status_changed(text: String, state: int) -> void:
	_status_label.text = text
	var color := Color(0.6, 0.95, 0.6)
	match state:
		SyncManager.State.SYNCING:
			color = Color(1.0, 0.85, 0.4)
		SyncManager.State.ERROR:
			color = Color(1.0, 0.45, 0.4)
		SyncManager.State.DISCONNECTED:
			color = Color(0.75, 0.75, 0.75)
	_status_label.add_theme_color_override("font_color", color)
	_sync_button.disabled = (state == SyncManager.State.SYNCING)


func _on_log(text: String, level: int) -> void:
	var color := "#b8c4d0"
	if level == SyncManager.Level.WARN:
		color = "#ffd166"
	elif level == SyncManager.Level.ERROR:
		color = "#ff6b6b"
	var stamp := Time.get_time_string_from_system()
	_log.append_text("[color=#6b7785]%s[/color] [color=%s]%s[/color]\n" % [stamp, color, text.replace("[", "[lb]")])


func _on_activity(entries: Array) -> void:
	_activity.clear()
	for entry in entries:
		var date := str(entry.get("date", "")).replace("T", " ").replace("Z", "").left(16)
		_activity.append_text("[color=#6b7785]%s[/color]  [b]%s[/b]  %s  [color=#6b7785]%s[/color]\n" % [
			date,
			str(entry.get("author", "?")).replace("[", "[lb]"),
			str(entry.get("message", "")).replace("[", "[lb]"),
			str(entry.get("sha", "")),
		])
