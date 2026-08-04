#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input_event_mouse.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "x11wm_node.hpp"

using namespace godot;

VARIANT_ENUM_CAST(X11WMNode::WmMode);
VARIANT_ENUM_CAST(X11WMNode::ServerMode);

X11WMNode::X11WMNode() {
	set_process(true);
	set_process_unhandled_input(true);
	set_mouse_filter(MOUSE_FILTER_IGNORE);
}

X11WMNode::~X11WMNode() {
	stop();
}

void X11WMNode::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "width", "height"), &X11WMNode::start);
	ClassDB::bind_method(D_METHOD("start_with_commands", "width", "height", "commands"), &X11WMNode::start_with_commands);
	ClassDB::bind_method(D_METHOD("stop"), &X11WMNode::stop);
	ClassDB::bind_method(D_METHOD("launch_client", "command"), &X11WMNode::launch_client);
	ClassDB::bind_method(D_METHOD("is_running"), &X11WMNode::is_running);
	ClassDB::bind_method(D_METHOD("get_texture"), &X11WMNode::get_texture);
	ClassDB::bind_method(D_METHOD("get_clipboard"), &X11WMNode::get_clipboard);
	ClassDB::bind_method(D_METHOD("set_clipboard", "text"), &X11WMNode::set_clipboard);

	ClassDB::bind_method(D_METHOD("set_launch_on_start", "commands"), &X11WMNode::set_launch_on_start);
	ClassDB::bind_method(D_METHOD("get_launch_on_start"), &X11WMNode::get_launch_on_start);
	ClassDB::bind_method(D_METHOD("set_autostart", "enable"), &X11WMNode::set_autostart);
	ClassDB::bind_method(D_METHOD("is_autostart"), &X11WMNode::is_autostart);
	ClassDB::bind_method(D_METHOD("set_forward_input", "enable"), &X11WMNode::set_forward_input);
	ClassDB::bind_method(D_METHOD("is_forwarding_input"), &X11WMNode::is_forwarding_input);
	ClassDB::bind_method(D_METHOD("set_wm_mode", "mode"), &X11WMNode::set_wm_mode);
	ClassDB::bind_method(D_METHOD("get_wm_mode"), &X11WMNode::get_wm_mode);
	ClassDB::bind_method(D_METHOD("set_server_mode", "mode"), &X11WMNode::set_server_mode);
	ClassDB::bind_method(D_METHOD("get_server_mode"), &X11WMNode::get_server_mode);
	ClassDB::bind_method(D_METHOD("set_display_name", "name"), &X11WMNode::set_display_name);
	ClassDB::bind_method(D_METHOD("get_display_name"), &X11WMNode::get_display_name);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "launch_on_start"), "set_launch_on_start", "get_launch_on_start");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autostart"), "set_autostart", "is_autostart");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "forward_input"), "set_forward_input", "is_forwarding_input");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "wm_mode", PROPERTY_HINT_ENUM, "TinyWM,MWM_Lite,MWM"), "set_wm_mode", "get_wm_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "server_mode", PROPERTY_HINT_ENUM, "Xvfb,Xephyr,Attach"), "set_server_mode", "get_server_mode");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "display_name"), "set_display_name", "get_display_name");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "ImageTexture", PROPERTY_USAGE_READ_ONLY), "", "get_texture");

	ADD_SIGNAL(MethodInfo("frame_ready"));
	ADD_SIGNAL(MethodInfo("server_ready", PropertyInfo(Variant::STRING, "display")));
	ADD_SIGNAL(MethodInfo("log_message", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("selection_changed", PropertyInfo(Variant::STRING, "text")));

	BIND_ENUM_CONSTANT(WM_TINYWM);
	BIND_ENUM_CONSTANT(WM_MWM_LITE);
	BIND_ENUM_CONSTANT(WM_MWM);
	BIND_ENUM_CONSTANT(SERVER_XVFB);
	BIND_ENUM_CONSTANT(SERVER_XEPHYR);
	BIND_ENUM_CONSTANT(SERVER_ATTACH);
}

void X11WMNode::_notification(int what) {
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

void X11WMNode::_process(double delta) {
	(void)delta;
	poll_host_clipboard();
	if (running) {
		wm.pump();
	}
}

void X11WMNode::_draw() {
	if (!texture.is_valid()) {
		return;
	}
	draw_texture_rect(texture, Rect2(Vector2(), get_size()), false);
}

void X11WMNode::_unhandled_input(const Ref<InputEvent> &event) {
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

void X11WMNode::start(int width, int height) {
	if (running || width <= 0 || height <= 0) {
		return;
	}
	start_wm(width, height);
}

void X11WMNode::start_with_commands(int width, int height, const PackedStringArray &commands) {
	launch_on_start = commands;
	start(width, height);
}

void X11WMNode::start_wm(int width, int height) {
	DisplayServer::get_singleton()->window_set_title("X11WM");
	wm.client = this;
	wm.set_wm_mode((X11WM::WmMode)wm_mode);
	output_width = width;
	output_height = height;
	running = wm.init(width, height, (X11WM::ServerMode)server_mode, display_name.utf8().get_data());
	if (running) {
		emit_signal("server_ready", String(wm.display_string()));
		launch_configured_apps();
	} else {
		wm.client = nullptr;
	}
}

void X11WMNode::launch_configured_apps() {
	if (!running) {
		return;
	}
	for (int i = 0; i < launch_on_start.size(); i++) {
		const String command = launch_on_start[i];
		if (command.strip_edges().length() == 0) {
			continue;
		}
		wm.launch_app(command.utf8().get_data());
	}
}

void X11WMNode::stop() {
	if (!running) {
		return;
	}
	wm.shutdown();
	wm.client = nullptr;
	running = false;
	texture.unref();
	queue_redraw();
}

bool X11WMNode::launch_client(const String &command) {
	if (!running) {
		return false;
	}
	return wm.launch_app(command.utf8().get_data());
}

bool X11WMNode::is_running() const {
	return running;
}

Ref<ImageTexture> X11WMNode::get_texture() const {
	return texture;
}

String X11WMNode::get_clipboard() const {
	return DisplayServer::get_singleton()->clipboard_get();
}

void X11WMNode::set_clipboard(const String &text) {
	host_clipboard_cache = text.utf8().get_data();
	DisplayServer::get_singleton()->clipboard_set(text);
	if (running) {
		wm.push_clipboard(host_clipboard_cache);
	}
}

void X11WMNode::set_launch_on_start(const PackedStringArray &commands) {
	launch_on_start = commands;
}

PackedStringArray X11WMNode::get_launch_on_start() const {
	return launch_on_start;
}

void X11WMNode::set_autostart(bool enable) {
	autostart = enable;
}

bool X11WMNode::is_autostart() const {
	return autostart;
}

void X11WMNode::set_forward_input(bool enable) {
	forward_input = enable;
}

bool X11WMNode::is_forwarding_input() const {
	return forward_input;
}

void X11WMNode::set_wm_mode(int mode) {
	wm_mode = mode;
}

int X11WMNode::get_wm_mode() const {
	return wm_mode;
}

void X11WMNode::set_server_mode(int mode) {
	server_mode = mode;
}

int X11WMNode::get_server_mode() const {
	return server_mode;
}

void X11WMNode::set_display_name(const String &name) {
	display_name = name;
}

String X11WMNode::get_display_name() const {
	if (running) {
		return String(wm.display_string());
	}
	return display_name;
}

void X11WMNode::on_frame(const uint8_t *rgba, int width, int height, int stride) {
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
}

void X11WMNode::on_log(const char *message) {
	emit_signal("log_message", String(message));
}

void X11WMNode::on_clipboard_text(const std::string &text) {
	host_clipboard_cache = text;
	String s = String::utf8(text.data(), (int)text.size());
	DisplayServer::get_singleton()->clipboard_set(s);
	emit_signal("selection_changed", s);
}

std::string X11WMNode::get_clipboard_text() {
	return host_clipboard_cache;
}

void X11WMNode::poll_host_clipboard() {
	String clip = DisplayServer::get_singleton()->clipboard_get();
	std::string utf8 = clip.utf8().get_data();
	if (utf8 != host_clipboard_cache) {
		host_clipboard_cache = utf8;
		if (running) {
			wm.push_clipboard(utf8);
		}
	}
}

Vector2 X11WMNode::screen_to_wm(const Vector2 &screen_pos) const {
	Vector2 local = get_global_transform().affine_inverse().xform(screen_pos);
	Vector2 size = get_size();
	float nx = size.x > 0.0f ? local.x / size.x : 0.0f;
	float ny = size.y > 0.0f ? local.y / size.y : 0.0f;
	return Vector2(nx * output_width, ny * output_height);
}

void X11WMNode::send_key(const Ref<InputEventKey> &event) {
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
	int keysym = X11WM::keysym_from_godot((int)keycode);
	if (keysym == 0) {
		return;
	}
	wm.fake_key((uint32_t)keysym, event->is_pressed());
}

void X11WMNode::send_mouse_button(const Ref<InputEventMouseButton> &event) {
	int index = event->get_button_index();
	switch (index) {
	case MOUSE_BUTTON_WHEEL_UP:
	case MOUSE_BUTTON_WHEEL_DOWN:
		if (event->is_pressed()) {
			wm.pointer_axis_vertical(index == MOUSE_BUTTON_WHEEL_UP ? 1.0 : -1.0);
		}
		return;
	case MOUSE_BUTTON_WHEEL_LEFT:
	case MOUSE_BUTTON_WHEEL_RIGHT:
		if (event->is_pressed()) {
			wm.pointer_axis_horizontal(index == MOUSE_BUTTON_WHEEL_LEFT ? -1.0 : 1.0);
		}
		return;
	default:
		break;
	}
	unsigned int x11_button = 0;
	switch (index) {
	case MOUSE_BUTTON_LEFT: x11_button = 1; break;
	case MOUSE_BUTTON_MIDDLE: x11_button = 2; break;
	case MOUSE_BUTTON_RIGHT: x11_button = 3; break;
	case MOUSE_BUTTON_XBUTTON1: x11_button = 8; break;
	case MOUSE_BUTTON_XBUTTON2: x11_button = 9; break;
	default: return;
	}
	Vector2 pos = screen_to_wm(event->get_position());
	wm.fake_motion((int)pos.x, (int)pos.y);
	wm.fake_button(x11_button, event->is_pressed());
}

void X11WMNode::send_mouse_motion(const Ref<InputEventMouseMotion> &event) {
	Vector2 pos = screen_to_wm(event->get_position());
	wm.fake_motion((int)pos.x, (int)pos.y);
}
