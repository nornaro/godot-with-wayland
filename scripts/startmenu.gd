extends PanelContainer
## LXDE-style start menu with search and running app indicators.

signal app_launched(command: String)

@export var app_dirs: Array[String] = [
	"/usr/share/applications",
	"/usr/local/share/applications",
	"~/.local/share/applications",
]
@export var search_delay_ms: int = 200

var apps: Array[Dictionary] = []
var filtered_apps: Array[Dictionary] = []
var running_apps: Array[String] = []
var search_text: String = ""
var is_open: bool = false
var selected_index: int = 0

@onready var search_line: LineEdit = $MarginContainer/VBoxContainer/SearchBar/SearchField
@onready var app_list: ItemList = $MarginContainer/VBoxContainer/AppListContainer/AppList
@onready var running_section: VBoxContainer = $MarginContainer/VBoxContainer/RunningSection
@onready var scroll_container: ScrollContainer = $MarginContainer/VBoxContainer/AppListContainer

func _ready() -> void:
	visible = false
	load_apps()
	connect_signals()

func load_apps() -> void:
	apps.clear()
	for dir_path in app_dirs:
		var expanded_dir = dir_path.replace("~", OS.get_environment("HOME"))
		var dir = DirAccess.open(expanded_dir)
		if dir == null:
			continue
		dir.list_dir_begin()
		var filename = dir.get_next()
		while filename != "":
			if filename.ends_with(".desktop"):
				var app_info = parse_desktop_file(expanded_dir + "/" + filename)
				if app_info.get("Name", "") != "" and app_info.get("NoDisplay", false) == false:
					apps.append(app_info)
			filename = dir.get_next()
		dir.list_dir_end()
	apps.sort_custom(func(a, b): return a.get("Name", "").to_lower() < b.get("Name", "").to_lower())
	filtered_apps = apps.duplicate()

func parse_desktop_file(path: String) -> Dictionary:
	var file = FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var info: Dictionary = {"path": path, "Name": "", "Exec": "", "Icon": "", "Categories": "", "Terminal": false}
	var in_entry := false
	while true:
		var line = file.get_line()
		if line.begins_with("[Desktop Entry]"):
			in_entry = true
			continue
		if not in_entry:
			continue
		if line.begins_with("Name="):
			info["Name"] = line.substr(5).strip_edges()
		elif line.begins_with("Exec="):
			info["Exec"] = line.substr(5).strip_edges().split(" ")[0]
		elif line.begins_with("Icon="):
			info["Icon"] = line.substr(5).strip_edges()
		elif line.begins_with("Categories="):
			info["Categories"] = line.substr(11).strip_edges()
		elif line.begins_with("Terminal="):
			info["Terminal"] = line.substr(9).strip_edges().to_lower() == "true"
		elif line.begins_with("NoDisplay="):
			info["NoDisplay"] = line.substr(10).strip_edges().to_lower() == "true"
		elif line.begins_with("Hidden="):
			info["NoDisplay"] = line.substr(7).strip_edges().to_lower() == "true"
		elif line.begins_with("["):
			break
	file.close()
	if info["Name"] == "":
		return {}
	return info

func connect_signals() -> void:
	search_line.text_changed.connect(_on_search_changed)
	search_line.gui_input.connect(_on_search_input)
	app_list.item_selected.connect(_on_app_selected)

func _on_search_changed(new_text: String) -> void:
	search_text = new_text.to_lower()
	filter_apps()

func _on_search_input(event: InputEvent) -> void:
	if event is InputEventKey:
		var key = event as InputEventKey
		if key.pressed:
			match key.keycode:
				KEY_ESCAPE:
					toggle_menu()
				KEY_DOWN:
					move_selection(1)
				KEY_UP:
					move_selection(-1)
				KEY_ENTER:
					launch_selected()

func _on_app_selected(index: int) -> void:
	selected_index = index
	launch_selected()

func move_selection(dir: int) -> void:
	var count = app_list.get_item_count()
	if count == 0:
		return
	selected_index = clamp(selected_index + dir, 0, count - 1)
	app_list.select(selected_index)
	app_list.ensure_current_is_visible()

func launch_selected() -> void:
	if selected_index >= 0 and selected_index < filtered_apps.size():
		var app = filtered_apps[selected_index]
		app_launched.emit(app["Exec"])
		toggle_menu()

func filter_apps() -> void:
	app_list.clear()
	filtered_apps = apps.filter(func(app):
		return app["Name"].to_lower().find(search_text) >= 0 \
			or app.get("Categories", "").to_lower().find(search_text) >= 0
	)
	for app in filtered_apps:
		var item_idx = app_list.add_item(app["Name"])
		app_list.set_item_metadata(item_idx, app)
	selected_index = 0
	if filtered_apps.size() > 0:
		app_list.select(0)

func toggle_menu() -> void:
	is_open = not is_open
	visible = is_open
	if is_open:
		search_text = ""
		search_line.text = ""
		filter_apps()
		search_line.grab_focus()

func _on_app_launched(command: String) -> void:
	# Emit to parent to launch the app
	app_launched.emit(command)

func update_running_apps() -> void:
	for child in running_section.get_children():
		child.queue_free()
	for app_name in running_apps:
		var label = Label.new()
		label.text = "● " + app_name
		label.add_theme_color_override("font_color", Color(0.4, 0.8, 0.4))
		running_section.add_child(label)

func _process(_delta: float) -> void:
	if is_open and Input.is_action_just_pressed("ui_cancel"):
		toggle_menu()
