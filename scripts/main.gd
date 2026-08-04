extends Control
## Desktop controller: starts every embedded compositor backend and launches
## apps on them.
##
## Each entry of `startup_apps` is a list of shell commands launched on that
## backend whenever it is started. Add/remove entries to decide what opens in
## each "screen".
##
## Once running, call `launch_app("WaylandNode", "sakura")` or
## `launch_on_all("sakura")` from anywhere (e.g. the MCP bridge, a console, or
## another script) to open more apps.

signal app_launched(backend: String, command: String)

@export var startup_apps: Dictionary = {
	"WaylandNode": PackedStringArray(["sakura"]),
	"GameScope": PackedStringArray(["sakura"]),
	"X11WMNode": PackedStringArray(["sakura"]),
}

## Surface size each embedded backend renders at.
@export var backend_resolution := Vector2i(960, 540)

## Backend that receives taskbar/startmenu launches. Empty = all running backends.
@export var ui_launch_target: String = ""

## Start every backend in _ready() when true.
@export var autostart := true

var backends: Dictionary = {}

func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	if has_node("GridContainer"):
		$GridContainer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	collect_backends()
	if autostart:
		start_all()
	connect_ui()

## Find every node in this scene that looks like a compositor backend.
func collect_backends(node: Node = self) -> void:
	for child in node.get_children():
		if child.has_method("start") and child.has_method("launch_client") and child.has_method("is_running"):
			backends[child.name] = child
		else:
			collect_backends(child)

func connect_ui() -> void:
	if has_node("Taskbar"):
		$Taskbar.app_launched.connect(_on_ui_app_launched)
	if has_node("StartMenu"):
		$StartMenu.app_launched.connect(_on_ui_app_launched)

func _on_ui_app_launched(command: String) -> void:
	if ui_launch_target != "":
		launch_app(ui_launch_target, command)
	else:
		launch_on_all(command)

func start_all() -> void:
	for backend_name: String in backends:
		start_backend(backend_name)

## Start one backend and launch its configured apps. Returns true if it is running.
func start_backend(backend_name: String) -> bool:
	var backend: Node = backends.get(backend_name)
	if backend == null:
		push_warning("[main] unknown backend: %s" % backend_name)
		return false
	if not backend.is_running():
		print("[main] starting %s" % backend_name)
		backend.start(backend_resolution.x, backend_resolution.y)
		if not backend.is_running():
			push_warning("[main] %s failed to start" % backend_name)
			return false
	launch_startup_apps(backend_name)
	return true

func launch_startup_apps(backend_name: String) -> void:
	var backend: Node = backends.get(backend_name)
	if backend == null or not backend.is_running():
		return
	for command: String in startup_apps.get(backend_name, []):
		if command.strip_edges() == "":
			continue
		if backend.launch_client(command):
			app_launched.emit(backend_name, command)
		else:
			push_warning("[main] failed to launch '%s' on %s" % [command, backend_name])

## Start an app on one backend, starting the backend first if needed.
func launch_app(backend_name: String, command: String) -> bool:
	var backend: Node = backends.get(backend_name)
	if backend == null:
		push_warning("[main] unknown backend: %s" % backend_name)
		return false
	if not start_backend(backend_name):
		return false
	if not backend.launch_client(command):
		push_warning("[main] failed to launch '%s' on %s" % [command, backend_name])
		return false
	app_launched.emit(backend_name, command)
	return true

## Start an app on every running (or startable) backend.
func launch_on_all(command: String) -> void:
	for backend_name: String in backends:
		launch_app(backend_name, command)

## Add a command to a backend's startup list (does not launch it now).
func add_startup_app(backend_name: String, command: String) -> void:
	var apps: Array = startup_apps.get(backend_name, [])
	if command not in apps:
		apps.append(command)
		startup_apps[backend_name] = apps

## Remove a command from a backend's startup list.
func remove_startup_app(backend_name: String, command: String) -> void:
	var apps: Array = startup_apps.get(backend_name, [])
	apps.erase(command)
	startup_apps[backend_name] = apps

func print_status() -> void:
	for backend_name: String in backends:
		var backend: Node = backends[backend_name]
		print("[main] %s running=%s apps=%s" % [
			backend_name,
			backend.is_running(),
			str(startup_apps.get(backend_name, [])),
		])
