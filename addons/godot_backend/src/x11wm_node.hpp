#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "x11_wm.hpp"

#include <cstdint>
#include <string>

namespace godot {

// Embedded X11 window manager node. Runs an Xvfb/Xephyr server (or attaches to
// an existing display), acts as the window manager, renders the root
// framebuffer into a texture and forwards Godot input into the display via
// XTEST.
class X11WMNode : public Control, public X11WMClient {
	GDCLASS(X11WMNode, Control)

public:
	enum WmMode { WM_TINYWM = 0, WM_MWM_LITE = 1, WM_MWM = 2 };
	enum ServerMode { SERVER_XVFB = 0, SERVER_XEPHYR = 1, SERVER_ATTACH = 2 };

	X11WMNode();
	~X11WMNode();

	void start(int width, int height);
	void start_with_commands(int width, int height, const PackedStringArray &commands);
	void stop();
	bool launch_client(const String &command);
	bool is_running() const;
	Ref<ImageTexture> get_texture() const;
	String get_clipboard() const;
	void set_clipboard(const String &text);

	void set_launch_on_start(const PackedStringArray &commands);
	PackedStringArray get_launch_on_start() const;
	void set_autostart(bool enable);
	bool is_autostart() const;
	void set_forward_input(bool enable);
	bool is_forwarding_input() const;
	void set_wm_mode(int mode);
	int get_wm_mode() const;
	void set_server_mode(int mode);
	int get_server_mode() const;
	void set_display_name(const String &name);
	String get_display_name() const;

	void on_frame(const uint8_t *rgba, int width, int height, int stride) override;
	void on_log(const char *message) override;
	void on_clipboard_text(const std::string &text) override;
	std::string get_clipboard_text() override;

	void _process(double delta) override;
	void _unhandled_input(const Ref<InputEvent> &event) override;
	void _draw() override;

protected:
	static void _bind_methods();
	void _notification(int what);

private:
	void start_wm(int width, int height);
	void launch_configured_apps();
	void poll_host_clipboard();

	X11WM wm;
	Ref<ImageTexture> texture;
	Ref<Image> image;
	PackedByteArray frame_data;
	std::string host_clipboard_cache;
	PackedStringArray launch_on_start;
	bool autostart = true;
	bool running = false;
	bool forward_input = true;
	int wm_mode = WM_MWM_LITE;
	int server_mode = SERVER_XVFB;
	String display_name;
	int output_width = 0;
	int output_height = 0;

	Vector2 screen_to_wm(const Vector2 &screen_pos) const;
	void send_key(const Ref<InputEventKey> &event);
	void send_mouse_button(const Ref<InputEventMouseButton> &event);
	void send_mouse_motion(const Ref<InputEventMouseMotion> &event);
};

} // namespace godot
