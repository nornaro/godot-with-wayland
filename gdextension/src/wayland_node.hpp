#pragma once

#include "compositor.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <string>

namespace godot {

class WaylandNode : public Control, public ::CompositorClient {
	GDCLASS(WaylandNode, Control)

public:
	WaylandNode();
	~WaylandNode();

	void start(int width, int height);
	void stop();
	bool launch_client(const String &command);
	String get_socket_name() const;
	bool is_running() const;
	Ref<ImageTexture> get_texture() const;
	String get_clipboard() const;
	void set_clipboard(const String &text);
	void set_forward_input(bool enable);
	bool is_forwarding_input() const;
	void set_decorations_enabled(bool enable);
	bool is_decorations_enabled() const;
	void set_fullscreen_apps(bool enable);
	bool is_fullscreen_apps() const;
	void set_stretch_to_fit(bool enable);
	bool is_stretch_to_fit() const;

	void on_frame(const uint8_t *rgba, int width, int height, int stride) override;
	void on_socket(const char *name) override;
	void on_log(const char *message) override;
	std::string get_clipboard_text() override;
	void on_clipboard_text(const std::string &text) override;

	void _process(double delta) override;
	void _unhandled_input(const Ref<InputEvent> &event) override;
	void _draw() override;

protected:
	static void _bind_methods();
	void _notification(int what);

private:
	::Compositor compositor;
	Ref<ImageTexture> texture;
	Ref<Image> image;
	PackedByteArray frame_data;
	bool running = false;
	bool forward_input = true;
	bool decorations_enabled = false;
	bool fullscreen_apps = false;
	bool stretch_to_fit = true;
	int output_width = 0;
	int output_height = 0;
	unsigned long process_count = 0;
	int64_t last_frame_time = 0;
	unsigned long frame_count = 0;
	std::string host_clipboard_cache;

	Vector2 screen_to_compositor(const Vector2 &screen_pos) const;
	void poll_host_clipboard();
	void send_key(const Ref<InputEventKey> &event);
	void send_mouse_button(const Ref<InputEventMouseButton> &event);
	void send_mouse_motion(const Ref<InputEventMouseMotion> &event);
};

} // namespace godot
