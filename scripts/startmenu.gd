extends PanelContainer
## App launcher that searches /bin for executables.

signal app_launched(command: String)

@export var app_dir: String = "/bin"

var apps: Array[Dictionary] = []
var filtered_apps: Array[Dictionary] = []
var search_text: String = ""
var is_open: bool = false
var selected_index: int = 0

@onready var search_line: LineEdit = $MarginContainer/VBoxContainer/SearchBar/SearchField
@onready var app_list: ItemList = $MarginContainer/VBoxContainer/AppListContainer/AppList
@onready var scroll_container: ScrollContainer = $MarginContainer/VBoxContainer/AppListContainer

func _ready() -> void:
	visible = false
	load_apps()
	connect_signals()

func load_apps() -> void:
	apps.clear()
	var dir = DirAccess.open(app_dir)
	if dir == null:
		return
	dir.list_dir_begin()
	var filename = dir.get_next()
	while filename != "":
		if not filename.begins_with(".") and dir.current_is_dir() == false:
			apps.append({"name": filename, "command": app_dir + "/" + filename})
		filename = dir.get_next()
	dir.list_dir_end()
	apps.sort_custom(func(a, b): return a["name"].to_lower() < b["name"].to_lower())
	filtered_apps = apps.duplicate()

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
		app_launched.emit(app["command"])
		toggle_menu()

func filter_apps() -> void:
	app_list.clear()
	filtered_apps = apps.filter(func(app):
		return app["name"].to_lower().find(search_text) >= 0
	)
	for app in filtered_apps:
		app_list.add_item(app["name"])
		app_list.set_item_metadata(app_list.get_item_count() - 1, app)
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

func _process(_delta: float) -> void:
	if is_open and Input.is_action_just_pressed("ui_cancel"):
		toggle_menu()
