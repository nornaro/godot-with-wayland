#pragma once

#define WLR_USE_UNSTABLE

#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unordered_map>
#include <vector>

struct Compositor;

struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_scene;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;
struct wlr_scene_output;
struct wlr_scene_output_layout;
struct wlr_output_layout;
struct wlr_swapchain;
struct wlr_output;
struct wlr_xdg_shell;
struct wlr_xdg_toplevel;
struct wlr_xdg_popup;
struct wlr_xdg_toplevel_decoration_v1;
struct wlr_cursor;
struct wlr_xcursor_manager;
struct wlr_seat;
struct wlr_keyboard;
struct wlr_pointer;
struct wlr_surface;
struct xkb_keymap;

struct Toplevel {
	static constexpr int TITLE_BAR_HEIGHT = 24;
	static constexpr int CLOSE_BUTTON_SIZE = 22;

	struct wl_list link;
	Compositor *compositor;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *frame;
	struct wlr_scene_rect *close_btn;
	struct wlr_scene_tree *scene_tree;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	bool centered;
	bool fullscreen;
	bool fullscreen_resized;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener decoration_request_mode;
	struct wl_listener decoration_destroy;
};

struct Popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct CompositorClient {
	virtual void on_frame(const uint8_t *rgba, int width, int height, int stride) = 0;
	virtual void on_socket(const char *name) = 0;
	virtual void on_log(const char *message) = 0;
	virtual ~CompositorClient() = default;
};

struct Compositor {
	enum CursorMode {
		CURSOR_PASSTHROUGH,
		CURSOR_MOVE,
		CURSOR_RESIZE,
	};

	unsigned int timing_frames = 0;
	double timing_gpu_ms = 0;
	double timing_readback_ms = 0;
	double timing_client_ms = 0;
	double timing_total_ms = 0;
	unsigned int commit_count = 0;
	uint32_t commit_time = 0;
	uint32_t commit_window_start = 0;
	unsigned long pump_calls = 0;
	unsigned long last_log_pumps = 0;
	uint32_t last_log_time = 0;
	uint32_t last_render_msec = 0;
	unsigned long gate_renders = 0;
	unsigned long gate_skips = 0;
	unsigned long gate_decisions = 0;
	unsigned long last_log_gate_renders = 0;
	unsigned long last_log_gate_skips = 0;
	unsigned int spurious_renders = 0;
	unsigned long frame_events = 0;
	uint32_t last_frame_event_msec = 0;
	unsigned int frame_done_counter = 0;

	struct FrameDebug {
		unsigned long ok = 0;
		unsigned long build_fail = 0;
		unsigned long commit_fail = 0;
		unsigned long last_log = 0;
	} frame_debug;
	void debug_frame_log();

	CompositorClient *client = nullptr;
	bool started = false;

	struct wl_display *display = nullptr;
	struct wlr_backend *backend = nullptr;
	struct wlr_renderer *renderer = nullptr;
	struct wlr_allocator *allocator = nullptr;
	struct wlr_scene *scene = nullptr;
	struct wlr_scene_output_layout *scene_layout = nullptr;
	struct wlr_output_layout *output_layout = nullptr;
	struct wlr_swapchain *swapchain = nullptr;
	const char *socket_name = nullptr;

	struct wlr_output *output = nullptr;
	struct wlr_scene_output *scene_output = nullptr;
	struct wl_listener new_output;
	struct wl_listener output_frame;
	struct wl_listener output_request_state;
	struct wl_listener output_destroy;

	struct wlr_xdg_shell *xdg_shell = nullptr;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_decoration;
	bool has_decoration_listener = false;
	bool decorations_enabled = false;
	bool fullscreen_apps = false;
	struct wl_list toplevels;

	struct wlr_cursor *cursor = nullptr;
	struct wlr_xcursor_manager *cursor_mgr = nullptr;
	struct wl_listener request_set_cursor;
	struct wl_listener request_set_selection;

	struct wlr_seat *seat = nullptr;

	struct wlr_keyboard *keyboard = nullptr;
	struct wl_listener keyboard_modifiers;
	struct wl_listener keyboard_key;
	struct wlr_pointer *pointer = nullptr;

	enum CursorMode cursor_mode = CURSOR_PASSTHROUGH;
	Toplevel *grabbed_toplevel = nullptr;
	double grab_x = 0, grab_y = 0;
	struct wlr_box grab_geobox = {0};
	uint32_t resize_edges = 0;

	int output_width = 0;
	int output_height = 0;
	std::vector<uint8_t> rgba_scratch;
	std::unordered_map<uint32_t, uint32_t> keysym_to_keycode_cache;
	struct xkb_keymap *keymap_for_lookup = nullptr;

	bool init(int width, int height);
	void pump();
	void shutdown();
	bool running() const { return started && display != nullptr; }

	void pointer_move_absolute(double x, double y);
	void pointer_button(uint32_t button, bool pressed);
	void pointer_axis_vertical(double delta, int32_t discrete);
	void pointer_axis_horizontal(double delta, int32_t discrete);
	void key_event(int godot_key, bool pressed);

	bool launch_process(const char *command);

	void render_frame();
	void copy_to_rgba(const uint8_t *data, uint32_t format, size_t stride, int width, int height);
	uint32_t godot_key_to_keysym(int godot_key);
	uint32_t keysym_to_keycode(uint32_t keysym);
	Toplevel *desktop_toplevel_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);
	void process_cursor_motion(uint32_t time);
	void process_cursor_move();
	void process_cursor_resize();
	void begin_interactive(Toplevel *toplevel, CursorMode mode, uint32_t edges);
	void reset_cursor_mode();

	static uint32_t now_msec();
};
