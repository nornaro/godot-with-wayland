#include "wayland_node.hpp"

#include <linux/input-event-codes.h>

#include <cstring>

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

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

WaylandNode::WaylandNode() {
	set_process(true);
	set_process_unhandled_input(true);
	set_mouse_filter(MOUSE_FILTER_IGNORE);
}

WaylandNode::~WaylandNode() {
	stop();
}

void WaylandNode::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "width", "height"), &WaylandNode::start);
	ClassDB::bind_method(D_METHOD("stop"), &WaylandNode::stop);
	ClassDB::bind_method(D_METHOD("launch_client", "command"), &WaylandNode::launch_client);
	ClassDB::bind_method(D_METHOD("get_socket_name"), &WaylandNode::get_socket_name);
	ClassDB::bind_method(D_METHOD("is_running"), &WaylandNode::is_running);
	ClassDB::bind_method(D_METHOD("get_texture"), &WaylandNode::get_texture);
	ClassDB::bind_method(D_METHOD("get_clipboard"), &WaylandNode::get_clipboard);
	ClassDB::bind_method(D_METHOD("set_clipboard", "text"), &WaylandNode::set_clipboard);
	ClassDB::bind_method(D_METHOD("set_forward_input", "enable"), &WaylandNode::set_forward_input);
	ClassDB::bind_method(D_METHOD("is_forwarding_input"), &WaylandNode::is_forwarding_input);
	ClassDB::bind_method(D_METHOD("set_decorations_enabled", "enable"), &WaylandNode::set_decorations_enabled);
	ClassDB::bind_method(D_METHOD("is_decorations_enabled"), &WaylandNode::is_decorations_enabled);
	ClassDB::bind_method(D_METHOD("set_fullscreen_apps", "enable"), &WaylandNode::set_fullscreen_apps);
	ClassDB::bind_method(D_METHOD("is_fullscreen_apps"), &WaylandNode::is_fullscreen_apps);
	ClassDB::bind_method(D_METHOD("set_stretch_to_fit", "enable"), &WaylandNode::set_stretch_to_fit);
	ClassDB::bind_method(D_METHOD("is_stretch_to_fit"), &WaylandNode::is_stretch_to_fit);
	ClassDB::bind_method(D_METHOD("send_key", "event"), &WaylandNode::send_key);
	ClassDB::bind_method(D_METHOD("send_mouse_button", "event"), &WaylandNode::send_mouse_button);
	ClassDB::bind_method(D_METHOD("send_mouse_motion", "event"), &WaylandNode::send_mouse_motion);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "ImageTexture", PROPERTY_USAGE_READ_ONLY), "", "get_texture");
	ADD_SIGNAL(MethodInfo("frame_ready"));
	ADD_SIGNAL(MethodInfo("socket_ready", PropertyInfo(Variant::STRING, "socket_name")));
	ADD_SIGNAL(MethodInfo("log_message", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("selection_changed", PropertyInfo(Variant::STRING, "text")));
}

void WaylandNode::_notification(int what) {
	if (what == NOTIFICATION_EXIT_TREE) {
		stop();
	}
}

void WaylandNode::_process(double delta) {
	(void)delta;
	if (running) {
		process_count++;
		if (delta > 0.005) {
			printf("[perf] BIG delta %.1f ms\n", delta * 1000.0);
		}
		compositor.pump();
		if ((process_count % 30) == 0) {
			poll_host_clipboard();
		}
	}
}

void WaylandNode::_draw() {
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

void WaylandNode::_unhandled_input(const Ref<InputEvent> &event) {
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

void WaylandNode::start(int width, int height) {
	if (running || width <= 0 || height <= 0) {
		return;
	}
	compositor.client = this;
	compositor.decorations_enabled = decorations_enabled;
	compositor.fullscreen_apps = fullscreen_apps;
	output_width = width;
	output_height = height;
	running = compositor.init(width, height);
}

void WaylandNode::stop() {
	if (!running) {
		return;
	}
	compositor.shutdown();
	compositor.client = nullptr;
	running = false;
	texture.unref();
	queue_redraw();
}

bool WaylandNode::launch_client(const String &command) {
	if (!running) {
		return false;
	}
	return compositor.launch_process(command.utf8().get_data());
}

String WaylandNode::get_socket_name() const {
	return compositor.socket_name != nullptr ? String(compositor.socket_name) : String();
}

bool WaylandNode::is_running() const {
	return running;
}

String WaylandNode::get_clipboard() const {
	return DisplayServer::get_singleton()->clipboard_get();
}

void WaylandNode::set_clipboard(const String &text) {
	host_clipboard_cache = text.utf8().get_data();
	DisplayServer::get_singleton()->clipboard_set(text);
	if (running) {
		compositor.clipboard_sync();
	}
}

std::string WaylandNode::get_clipboard_text() {
	return host_clipboard_cache;
}

void WaylandNode::on_clipboard_text(const std::string &text) {
	host_clipboard_cache = text;
	String s = String::utf8(text.data(), (int)text.size());
	DisplayServer::get_singleton()->clipboard_set(s);
	emit_signal("selection_changed", s);
}

void WaylandNode::poll_host_clipboard() {
	String clip = DisplayServer::get_singleton()->clipboard_get();
	std::string utf8 = clip.utf8().get_data();
	if (utf8 != host_clipboard_cache) {
		host_clipboard_cache = utf8;
		if (running) {
			compositor.clipboard_sync();
		}
	}
}

Ref<ImageTexture> WaylandNode::get_texture() const {
	return texture;
}

void WaylandNode::set_forward_input(bool enable) {
	forward_input = enable;
}

bool WaylandNode::is_forwarding_input() const {
	return forward_input;
}

void WaylandNode::set_decorations_enabled(bool enable) {
	decorations_enabled = enable;
}

bool WaylandNode::is_decorations_enabled() const {
	return decorations_enabled;
}

void WaylandNode::set_fullscreen_apps(bool enable) {
	fullscreen_apps = enable;
}

bool WaylandNode::is_fullscreen_apps() const {
	return fullscreen_apps;
}

void WaylandNode::set_stretch_to_fit(bool enable) {
	stretch_to_fit = enable;
	queue_redraw();
}

bool WaylandNode::is_stretch_to_fit() const {
	return stretch_to_fit;
}

void WaylandNode::on_frame(const uint8_t *rgba, int width, int height, int stride) {
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

void WaylandNode::on_socket(const char *name) {
	emit_signal("socket_ready", String(name));
}

void WaylandNode::on_log(const char *message) {
	emit_signal("log_message", String(message));
}

Vector2 WaylandNode::screen_to_compositor(const Vector2 &screen_pos) const {
	Vector2 local = get_global_transform().affine_inverse().xform(screen_pos);
	Vector2 size = get_size();
	float nx = size.x > 0.0f ? local.x / size.x : 0.0f;
	float ny = size.y > 0.0f ? local.y / size.y : 0.0f;
	return Vector2(nx * output_width, ny * output_height);
}

void WaylandNode::send_key(const Ref<InputEventKey> &event) {
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

void WaylandNode::send_mouse_button(const Ref<InputEventMouseButton> &event) {
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

void WaylandNode::send_mouse_motion(const Ref<InputEventMouseMotion> &event) {
	Vector2 pos = screen_to_compositor(event->get_position());
	compositor.pointer_move_absolute(pos.x, pos.y);
}
