extends Control

@onready var wayland: WaylandNode = $Wayland
@onready var taskbar: PanelContainer = $Taskbar

enum DisplayMode {
	CONTROL_FULLSCREEN, ## render into the app's main window (full-rect Control)
	OS_WINDOW,           ## render inside a real OS-level window (future)
}

@export var display_mode: DisplayMode = DisplayMode.CONTROL_FULLSCREEN
## Make the app's main window fullscreen.
@export var fullscreen := false
## Fill the host window with the compositor output, stretching it when the
## window is resized. Disable to draw at the native compositor size (centered).
@export var stretch_to_fit := true
## Treat clients as apps that handle their own windowing: their surface is
## mapped edge-to-edge to the whole compositor output instead of a centered
## window.
@export var fullscreen_apps := false
@export var client: String = "mangohud 0ad -windowed"

var _frame := 0
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
	wayland.start(1280, 720)
	wayland.launch_client(client)

#func _process(_delta: float) -> void:
	#_frame += 1
	#if _frame % 90 == 0:
		#var img: Image = wayland.get_texture().get_image()
		#img.save_png("/tmp/opencode/wl_%d_%03d.png" % [_pid, _frame / 90])

func _on_socket_ready(socket_name: String) -> void:
	print("WAYLAND SOCKET: ", socket_name)

func _on_app_launched(command: String) -> void:
	wayland.launch_client(command)

func _on_log_message(message: String) -> void:
	print("wlroots: ", message)
