extends PanelContainer
## A launcher bar: each button starts a Wayland client via the compositor.

signal app_launched(command: String)

## App definitions: { "label": "shown on the button", "command": "shell command" }
@export var apps: Array[Dictionary] = [
	{ "label": "0AD", "command": "mangohud 0ad -windowed" },
	{ "label": "0AD (plain)", "command": "0ad -windowed" },
	{ "label": "QTerminal", "command": "qterminal" },
	{ "label": "Weston Terminal", "command": "weston-terminal" },
]

func _ready() -> void:
	_clear_buttons()
	for app in apps:
		var button := Button.new()
		button.text = app.get("label", "App")
		button.tooltip_text = app.get("command", "")
		button.focus_mode = Control.FOCUS_NONE
		button.pressed.connect(func() -> void:
			app_launched.emit(app.get("command", "")))
		$MarginContainer/HBoxContainer.add_child(button)

func _clear_buttons() -> void:
	var box := $MarginContainer/HBoxContainer as HBoxContainer
	for child in box.get_children():
		box.remove_child(child)
		child.queue_free()
