extends PanelContainer
## LXDE-style taskbar with start menu button, running app indicators, and app launcher.

signal app_launched(command: String)
signal start_menu_toggled(is_open: bool)

@export var apps: Array[Dictionary] = [
	{ "label": "0AD", "command": "mangohud 0ad -windowed" },
	{ "label": "0AD (plain)", "command": "0ad -windowed" },
	{ "label": "QTerminal", "command": "qterminal" },
	{ "label": "Weston Terminal", "command": "weston-terminal" },
]

var start_menu: Control = null
var start_button: Button = null

func _ready() -> void:
	start_menu = %StartMenu
	_build_ui()

func _build_ui() -> void:
	# Create start button
	start_button = Button.new()
	start_button.text = "☰"
	start_button.tooltip_text = "Start Menu"
	start_button.custom_minimum_size = Vector2(40, 36)
	start_button.focus_mode = Control.FOCUS_NONE
	start_button.add_theme_stylebox_override("panel", _create_start_button_style())
	start_button.pressed.connect(_on_start_button_pressed)
	$MarginContainer/HBoxContainer.add_child(start_button)

	# Add separator
	var separator = VSeparator.new()
	$MarginContainer/HBoxContainer.add_child(separator)

	# Add app buttons
	for app in apps:
		var button := Button.new()
		button.text = app.get("label", "App")
		button.tooltip_text = app.get("command", "")
		button.focus_mode = Control.FOCUS_NONE
		var cmd: String = app.get("command", "")
		button.pressed.connect(func() -> void: app_launched.emit(cmd))
		$MarginContainer/HBoxContainer.add_child(button)

	# Add stretch spacer
	var spacer = Control.new()
	spacer.custom_minimum_size = Vector2(0, 0)
	spacer.set_h_size_flags(Control.SIZE_EXPAND_FILL)
	$MarginContainer/HBoxContainer.add_child(spacer)

func _create_start_button_style() -> StyleBoxFlat:
	var style = StyleBoxFlat.new()
	style.bg_color = Color(0.14, 0.16, 0.2, 0.9)
	style.corner_radius_top_left = 6
	style.corner_radius_top_right = 6
	style.corner_radius_bottom_left = 6
	style.corner_radius_bottom_right = 6
	style.content_margin_left = 8
	style.content_margin_top = 4
	style.content_margin_right = 8
	style.content_margin_bottom = 4
	return style

func _on_start_button_pressed() -> void:
	#var start_menu_scene = load("res://ui/startmenu.tscn")
	#start_menu = start_menu_scene.instantiate()
	#start_menu.app_launched.connect(_on_app_launched)
	#get_tree().current_scene.add_child(start_menu)
	# Position below start button
	#var btn_rect = start_button.get_global_rect()
	#start_menu.position = Vector2(btn_rect.position.x, btn_rect.end.y)
	#start_menu.visible = true
	start_menu.visible = not start_menu.visible
	start_menu_toggled.emit(start_menu.visible)

func _on_app_launched(command: String) -> void:
	app_launched.emit(command)

func register_running_app(_name: String) -> void:
	# Called by the compositor when a new client is launched
	# This would be connected to the compositor's client tracking
	pass

func unregister_running_app(_name: String) -> void:
	# Called when a client exits
	pass
