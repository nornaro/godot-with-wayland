extends PanelContainer
## App launcher that searches ~/.local/share for .desktop files

signal app_launched(command: String)

@export var app_dirs: Array[String] = [
	"/home/sugo/.local/share/applications",
	"/home/sugo/.local/share",
]

var apps: Dictionary
var filtered_apps: Dictionary
var search_text: String = ""
var is_open: bool = false
var selected_index: int = 0

@onready var search_line: LineEdit = $MarginContainer/VBoxContainer/SearchBar/SearchField
@onready var app_list: ItemList = $MarginContainer/VBoxContainer/AppListContainer/AppList
@onready var scroll_container: ScrollContainer = $MarginContainer/VBoxContainer/AppListContainer

func _ready() -> void:
	visible = false
	app_list.focus_mode = Control.FOCUS_NONE
	load_apps()
	connect_signals()

func load_apps() -> void:
	apps.clear()
	for dir_path in app_dirs:
		var dir = DirAccess.open(dir_path.replace("~", OS.get_environment("HOME")))
		if dir == null:
			continue
		dir.list_dir_begin()
		var filename = dir.get_next()
		while filename != "":
			if filename.ends_with(".desktop"):
				var app_info = parse_desktop_file(dir_path.replace("~", OS.get_environment("HOME")) + "/" + filename)
				if app_info.get("Name", "") != "" and app_info.get("NoDisplay", false) == false:
					apps[app_info.Name] = app_info
			filename = dir.get_next()
		dir.list_dir_end()
	#apps.sort_custom(func(a, b): return a.get("Name", "").to_lower() < b.get("Name", "").to_lower())

func parse_desktop_file(path: String) -> Dictionary:
	var info: Dictionary = {"path": path, "Name": "", "Exec": "", "Icon": "", "Categories": "", "Terminal": false}
	for line:String in FileAccess.get_file_as_string(path).split("\n"):
		if line.begins_with("Name="):
			info["Name"] = line.substr(5).strip_edges()
		if line.begins_with("Exec="):
			info["Exec"] = line.substr(5).strip_edges().split(" ")[0]
		if line.begins_with("Icon="):
			info["Icon"] = line.substr(5).strip_edges()
		if line.begins_with("Categories="):
			info["Categories"] = line.substr(11).strip_edges()
		if line.begins_with("Terminal="):
			info["Terminal"] = line.substr(9).strip_edges().to_lower() == "true"
		if line.begins_with("NoDisplay="):
			info["NoDisplay"] = line.substr(10).strip_edges().to_lower() == "true"
		if line.begins_with("Hidden="):
			info["NoDisplay"] = line.substr(7).strip_edges().to_lower() == "true"
	if info["Name"] == "":
		return {}
	return info

func connect_signals() -> void:
	search_line.text_changed.connect(_on_search_changed)
	search_line.gui_input.connect(_on_search_input)
	app_list.item_selected.connect(_on_app_selected)
	visibility_changed.connect(_on_visibility_changed)

func _on_search_changed(new_text: String) -> void:
	search_text = new_text.to_lower()
	filter_apps()

func _on_search_input(event: InputEvent) -> void:
	if event is not InputEventKey:
		return
	match event.keycode:
		KEY_ESCAPE, KEY_DOWN, KEY_UP, KEY_ENTER:
			search_line.accept_event()
	if not event.pressed:
		return
	match event.keycode:
		KEY_ESCAPE:
			hide()
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
	selected_index = wrapi(selected_index + dir, 0, count)
	app_list.select(selected_index)
	app_list.ensure_current_is_visible()

func launch_selected() -> void:
	var count = app_list.get_item_count()
	if count == 0:
		return
	var index = clampi(selected_index, 0, count - 1)
	var app: String = app_list.get_item_text(index)
	if apps.has(app):
		app_launched.emit(apps[app].Exec)
		hide()

func filter_apps() -> void:
	app_list.clear()
	for app: Dictionary in apps.values():
		if app.values().any(func(val) -> bool:
			return search_text.is_subsequence_of(str(val))
		):
			app_list.add_item(app["Name"])
	if app_list.get_item_count() > 0:
		selected_index = 0
		app_list.select(0)

func toggle_menu() -> void:
	if !visible:
		show()
	else:
		hide()

func _on_visibility_changed() -> void:
	is_open = visible
	if visible:
		search_line.text = ""
		search_text = ""
		filter_apps()
		search_line.grab_focus()

func _process(_delta: float) -> void:
	if is_open and Input.is_action_just_pressed("ui_cancel"):
		hide()
