extends Control

@onready var wayland: WaylandNode = $Wayland
@onready var wm: WaylandNode = $WM
@onready var gamescope: WaylandNode = $Gamescope
@onready var taskbar: PanelContainer = $Taskbar
@onready var start_menu: Control = $StartMenu

enum DisplayMode {
	CONTROL_FULLSCREEN, ## render into the app's main window (full-rect Control)
	OS_WINDOW,           ## render inside a real OS-level window (future)
}

@export var display_mode: DisplayMode = DisplayMode.CONTROL_FULLSCREEN
@export var fullscreen := false
@export var stretch_to_fit := true
@export var fullscreen_apps := false
@export var client: String = "mangohud 0ad -windowed"

var _pid: int

func _ready() -> void:
	_pid = OS.get_process_id()
	wayland.set_decorations_enabled(false)
	wayland.set_stretch_to_fit(stretch_to_fit)
	wayland.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	match display_mode:
		DisplayMode.CONTROL_FULLSCREEN:
			if fullscreen:
				DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_FULLSCREEN)
		_:
			push_warning("OS_WINDOW display mode not implemented yet, using Control")

	wayland.set_forward_input(true)
	wayland.set_fullscreen_apps(fullscreen_apps)
	wayland.socket_ready.connect(_on_socket_ready)
	wayland.log_message.connect(_on_log_message)
	taskbar.app_launched.connect(_on_app_launched)
	start_menu.app_launched.connect(_on_app_launched)
	wayland.start(1280, 720)
	wayland.launch_client(client)
	
	# Add WM and Gamescope backends for different use cases
	wm.set_decorations_enabled(true)
	wm.set_stretch_to_fit(false)
	wm.set_fullscreen_apps(true)
	wm.start(1280, 720)
	
	gamescope.set_decorations_enabled(false)
	gamescope.set_stretch_to_fit(true)
	gamescope.set_fullscreen_apps(true)
	gamescope.start(1280, 720)

func _on_socket_ready(socket_name: String) -> void:
	print("WAYLAND SOCKET: ", socket_name)

func _on_app_launched(command: String) -> void:
	wayland.launch_client(command)

func _on_log_message(message: String) -> void:
	print("wlroots: ", message)
