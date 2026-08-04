#include "gamescope_node.hpp"

#include <linux/input-event-codes.h>

#include <cstring>

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

// Implemented in compositor_gamescope.cpp.
struct wlr_backend *create_gamescope_backend(struct wl_display *display);

namespace {
int mouse_button_to_wlr(int godot_button) {
	switch (godot_button) {
	case MOUSE_BUTTON_LEFT: return BTN_LEFT;
	case MOUSE_BUTTON_RIGHT: return BTN_RIGHT;
	case MOUSE_BUTTON_MIDDLE: return BTN_MIDDLE;
	case MOUSE_BUTTON_XBUTTON1: return BTN_SIDE;
	case MOUSE_BUTTON_XBUTTON2: return BTN_EXTRA;
	default: return 0;
	}
}
} // namespace

GameScope::GameScope() {
	set_process(true);
	set_process_unhandled_input(true);
	set_mouse_filter(MOUSE_FILTER_IGNORE);
}

GameScope::~GameScope() {
	stop();
}

void GameScope::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "width", "height"), &GameScope::start);
	ClassDB::bind_method(D_METHOD("start_with_commands", "width", "height", "commands"), &GameScope::start_with_commands);
	ClassDB::bind_method(D_METHOD("stop"), &GameScope::stop);
	ClassDB::bind_method(D_METHOD("launch_client", "command"), &GameScope::launch_client);
	ClassDB::bind_method(D_METHOD("get_socket_name"), &GameScope::get_socket_name);
	ClassDB::bind_method(D_METHOD("is_running"), &GameScope::is_running);
	ClassDB::bind_method(D_METHOD("get_texture"), &GameScope::get_texture);
	ClassDB::bind_method(D_METHOD("get_clipboard"), &GameScope::get_clipboard);
	ClassDB::bind_method(D_METHOD("set_clipboard", "text"), &GameScope::set_clipboard);
	ClassDB::bind_method(D_METHOD("launch_configured_apps"), &GameScope::launch_configured_apps);

	ClassDB::bind_method(D_METHOD("set_launch_on_start", "commands"), &GameScope::set_launch_on_start);
	ClassDB::bind_method(D_METHOD("get_launch_on_start"), &GameScope::get_launch_on_start);
	ClassDB::bind_method(D_METHOD("set_autostart", "enable"), &GameScope::set_autostart);
	ClassDB::bind_method(D_METHOD("is_autostart"), &GameScope::is_autostart);
	ClassDB::bind_method(D_METHOD("set_forward_input", "enable"), &GameScope::set_forward_input);
	ClassDB::bind_method(D_METHOD("is_forwarding_input"), &GameScope::is_forwarding_input);
	ClassDB::bind_method(D_METHOD("set_fullscreen_apps", "enable"), &GameScope::set_fullscreen_apps);
	ClassDB::bind_method(D_METHOD("is_fullscreen_apps"), &GameScope::is_fullscreen_apps);
	ClassDB::bind_method(D_METHOD("set_decorations_enabled", "enable"), &GameScope::set_decorations_enabled);
	ClassDB::bind_method(D_METHOD("is_decorations_enabled"), &GameScope::is_decorations_enabled);
	ClassDB::bind_method(D_METHOD("set_stretch_to_fit", "enable"), &GameScope::set_stretch_to_fit);
	ClassDB::bind_method(D_METHOD("is_stretch_to_fit"), &GameScope::is_stretch_to_fit);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "launch_on_start"), "set_launch_on_start", "get_launch_on_start");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autostart"), "set_autostart", "is_autostart");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "forward_input"), "set_forward_input", "is_forwarding_input");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fullscreen_apps"), "set_fullscreen_apps", "is_fullscreen_apps");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "decorations_enabled"), "set_decorations_enabled", "is_decorations_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stretch_to_fit"), "set_stretch_to_fit", "is_stretch_to_fit");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "ImageTexture", PROPERTY_USAGE_READ_ONLY), "", "get_texture");

	ADD_SIGNAL(MethodInfo("frame_ready"));
	ADD_SIGNAL(MethodInfo("socket_ready", PropertyInfo(Variant::STRING, "socket_name")));
	ADD_SIGNAL(MethodInfo("log_message", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("selection_changed", PropertyInfo(Variant::STRING, "text")));
}

void GameScope::_notification(int what) {
	switch (what) {
	case NOTIFICATION_READY:
		if (autostart) {
			Size2 size = get_size();
			if (size.x <= 0.0f || size.y <= 0.0f) {
				size = Size2(1280, 720);
			}
			start((int)size.x, (int)size.y);
		}
		break;
	case NOTIFICATION_EXIT_TREE:
		stop();
		break;
	default:
		break;
	}
}

void GameScope::_process(double delta) {
	(void)delta;
	poll_host_clipboard();
	if (running) {
		compositor.pump();
	}
}

void GameScope::_draw() {
	if (!texture.is_valid()) {
		return;
	}
	if (stretch_to_fit) {
		draw_texture_rect(texture, Rect2(Vector2(), get_size()), false);
	} else {
		const Size2 size = texture->get_size();
		draw_texture_rect(texture, Rect2((get_size() - size) / 2.0, size), false);
	}
}

void GameScope::_unhandled_input(const Ref<InputEvent> &event) {
	if (!running || !forward_input) {
		return;
	}
	Viewport *viewport = get_viewport();
	const Ref<InputEventKey> key = event;
	if (key.is_valid()) {
		// Only the backend under the pointer receives keyboard input, so a
		// keystroke is not echoed into every compositor's focused window.
		if (get_global_rect().has_point(viewport->get_mouse_position())) {
			send_key(key);
			viewport->set_input_as_handled();
		}
		return;
	}
	const Ref<InputEventMouse> mouse = event;
	if (mouse.is_valid() && !get_global_rect().has_point(mouse->get_position())) {
		return;
	}
	const Ref<InputEventMouseButton> mouse_button = event;
	if (mouse_button.is_valid()) {
		send_mouse_button(mouse_button);
		viewport->set_input_as_handled();
		return;
	}
	const Ref<InputEventMouseMotion> mouse_motion = event;
	if (mouse_motion.is_valid()) {
		send_mouse_motion(mouse_motion);
		viewport->set_input_as_handled();
		return;
	}
}

void GameScope::start(int width, int height) {
	if (running || width <= 0 || height <= 0) {
		return;
	}
	start_compositor(width, height);
}

void GameScope::start_with_commands(int width, int height, const PackedStringArray &commands) {
	launch_on_start = commands;
	start(width, height);
}

void GameScope::start_compositor(int width, int height) {
	DisplayServer::get_singleton()->window_set_title("Gamescope");
	compositor.client = this;
	compositor.backend_factory = create_gamescope_backend;
	compositor.decorations_enabled = decorations_enabled;
	compositor.fullscreen_apps = fullscreen_apps;
	output_width = width;
	output_height = height;
	running = compositor.init(width, height);
	if (running) {
		launch_configured_apps();
	}
}

void GameScope::launch_configured_apps() {
	if (!running) {
		return;
	}
	for (int i = 0; i < launch_on_start.size(); i++) {
		const String command = launch_on_start[i];
		if (command.strip_edges().length() == 0) {
			continue;
		}
		compositor.launch_process(command.utf8().get_data());
	}
}

void GameScope::stop() {
	if (!running) {
		return;
	}
	compositor.shutdown();
	compositor.client = nullptr;
	compositor.backend_factory = nullptr;
	running = false;
	texture.unref();
	queue_redraw();
}

bool GameScope::launch_client(const String &command) {
	if (!running) {
		return false;
	}
	return compositor.launch_process(command.utf8().get_data());
}

String GameScope::get_socket_name() const {
	return compositor.socket_name != nullptr ? String(compositor.socket_name) : String();
}

bool GameScope::is_running() const {
	return running;
}

Ref<ImageTexture> GameScope::get_texture() const {
	return texture;
}

String GameScope::get_clipboard() const {
	return DisplayServer::get_singleton()->clipboard_get();
}

void GameScope::set_clipboard(const String &text) {
	host_clipboard_cache = text.utf8().get_data();
	DisplayServer::get_singleton()->clipboard_set(text);
	if (running) {
		compositor.clipboard_sync();
	}
}

std::string GameScope::get_clipboard_text() {
	return host_clipboard_cache;
}

void GameScope::on_clipboard_text(const std::string &text) {
	host_clipboard_cache = text;
	String s = String::utf8(text.data(), (int)text.size());
	DisplayServer::get_singleton()->clipboard_set(s);
	emit_signal("selection_changed", s);
}

void GameScope::poll_host_clipboard() {
	String clip = DisplayServer::get_singleton()->clipboard_get();
	std::string utf8 = clip.utf8().get_data();
	if (utf8 != host_clipboard_cache) {
		host_clipboard_cache = utf8;
		if (running) {
			compositor.clipboard_sync();
		}
	}
}

void GameScope::set_launch_on_start(const PackedStringArray &commands) {
	launch_on_start = commands;
}

PackedStringArray GameScope::get_launch_on_start() const {
	return launch_on_start;
}

void GameScope::set_autostart(bool enable) {
	autostart = enable;
}

bool GameScope::is_autostart() const {
	return autostart;
}

void GameScope::set_forward_input(bool enable) {
	forward_input = enable;
}

bool GameScope::is_forwarding_input() const {
	return forward_input;
}

void GameScope::set_fullscreen_apps(bool enable) {
	fullscreen_apps = enable;
}

bool GameScope::is_fullscreen_apps() const {
	return fullscreen_apps;
}

void GameScope::set_decorations_enabled(bool enable) {
	decorations_enabled = enable;
}

bool GameScope::is_decorations_enabled() const {
	return decorations_enabled;
}

void GameScope::set_stretch_to_fit(bool enable) {
	stretch_to_fit = enable;
	queue_redraw();
}

bool GameScope::is_stretch_to_fit() const {
	return stretch_to_fit;
}

void GameScope::on_frame(const uint8_t *rgba, int width, int height, int stride) {
	const size_t bytes = (size_t)stride * height;
	frame_data.resize(bytes);
	memcpy(frame_data.ptrw(), rgba, bytes);

	if (image.is_null() || image->get_width() != width || image->get_height() != height) {
		image = Image::create_from_data(width, height, false, Image::FORMAT_RGBA8, frame_data);
		texture = ImageTexture::create_from_image(image);
	} else {
		image->set_data(width, height, false, Image::FORMAT_RGBA8, frame_data);
		texture->update(image);
	}
	queue_redraw();
	emit_signal("frame_ready");

	frame_count++;
}

void GameScope::on_socket(const char *name) {
	emit_signal("socket_ready", String(name));
}

void GameScope::on_log(const char *message) {
	emit_signal("log_message", String(message));
}

Vector2 GameScope::screen_to_compositor(const Vector2 &screen_pos) const {
	Vector2 local = get_global_transform().affine_inverse().xform(screen_pos);
	Vector2 size = get_size();
	float nx = size.x > 0.0f ? local.x / size.x : 0.0f;
	float ny = size.y > 0.0f ? local.y / size.y : 0.0f;
	return Vector2(nx * output_width, ny * output_height);
}

void GameScope::send_key(const Ref<InputEventKey> &event) {
	if (event->is_echo()) {
		return;
	}
	uint32_t keycode = event->get_physical_keycode();
	if (keycode == 0) {
		keycode = event->get_keycode();
	}
	if (keycode == 0) {
		return;
	}
	compositor.key_event((int)keycode, event->is_pressed());
}

void GameScope::send_mouse_button(const Ref<InputEventMouseButton> &event) {
	int index = event->get_button_index();
	switch (index) {
	case MOUSE_BUTTON_WHEEL_UP:
	case MOUSE_BUTTON_WHEEL_DOWN:
	case MOUSE_BUTTON_WHEEL_LEFT:
	case MOUSE_BUTTON_WHEEL_RIGHT:
		if (event->is_pressed()) {
			if (index == MOUSE_BUTTON_WHEEL_UP) {
				compositor.pointer_axis_vertical(1.0, 1);
			} else if (index == MOUSE_BUTTON_WHEEL_DOWN) {
				compositor.pointer_axis_vertical(-1.0, -1);
			} else if (index == MOUSE_BUTTON_WHEEL_LEFT) {
				compositor.pointer_axis_horizontal(-1.0, -1);
			} else {
				compositor.pointer_axis_horizontal(1.0, 1);
			}
		}
		return;
	default:
		break;
	}
	int wlr_button = mouse_button_to_wlr(index);
	if (wlr_button != 0) {
		Vector2 pos = screen_to_compositor(event->get_position());
		compositor.pointer_move_absolute(pos.x, pos.y);
		compositor.pointer_button((uint32_t)wlr_button, event->is_pressed());
	}
}

void GameScope::send_mouse_motion(const Ref<InputEventMouseMotion> &event) {
	Vector2 pos = screen_to_compositor(event->get_position());
	compositor.pointer_move_absolute(pos.x, pos.y);
}
