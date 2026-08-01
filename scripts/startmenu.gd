extends PanelContainer

## LXDE-style start menu with search and running app indicators.

signal app_launched(command: String)

var apps: Array[Dictionary] = []
var filtered_apps: Array[Dictionary] = []
var search_text: String = ""
var is_open: bool = false
var selected_index: int = 0

@onready var search_line: LineEdit = $MarginContainer/VBoxContainer/SearchBar/SearchField
@onready var app_list: ItemList = $MarginContainer/VBoxContainer/AppListContainer/AppList
@onready var running_section: VBoxContainer = $MarginContainer/VBoxContainer/RunningSection

func _ready() -> void:
    visible = false
    load_apps()
    connect_signals()

func load_apps() -> void:
    var log = FileAccess.open("/tmp/startmenu_debug.txt", FileAccess.WRITE)
    if log:
        log.store_line("Load apps called")
        log.close()
    apps.clear()
    var dir = DirAccess.open("/home/sugo/.local/share")
    if dir == null:
        var log = FileAccess.open("/tmp/startmenu_debug.txt", FileAccess.WRITE)
        if log:
            log.store_line("FAILED to open /home/sugo/.local/share")
            log.close()
        return
    dir.list_dir_begin()
    var filename = dir.get_next()
    var log = FileAccess.open("/tmp/startmenu_debug.txt", FileAccess.WRITE)
    if log:
        log.store_line("Scanning /home/sugo/.local/share")
        log.close()
    while filename != "":
        if filename.ends_with(".desktop"):
            var app_info = parse_desktop_file("/home/sugo/.local/share/" + filename)
            if app_info.get("Name", "") != "" and app_info.get("NoDisplay", false) == false:
                apps.append(app_info)
                var log = FileAccess.open("/tmp/startmenu_debug.txt", FileAccess.WRITE)
                if log:
                    log.store_line("Found app: " + filename + " -> " + app_info.get("Name", ""))
                    log.close()
        filename = dir.get_next()
    dir.list_dir_end()
    apps.sort_custom(func(a, b): return a.get("Name", "").to_lower() < b.get("Name", "").to_lower())
    var log = FileAccess.open("/tmp/startmenu_debug.txt", FileAccess.WRITE)
    if log:
        log.store_line("apps loaded: " + str(apps.size()))
        log.close()
    filtered_apps = apps.duplicate()

func parse_desktop_file(path: String) -> Dictionary:
    var file = FileAccess.open(path, FileAccess.READ)
    if file == null:
        return {}
    var info: Dictionary = {"Name": "", "Exec": "", "Categories": "", "Terminal": false}
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
        return (app["Name"].to_lower().find(search_text) >= 0
            or app.get("Categories", "").to_lower().find(search_text) >= 0)
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

func _process(_delta: float) -> void:
    if is_open and Input.is_action_just_pressed("ui_cancel"):
        toggle_menu()
