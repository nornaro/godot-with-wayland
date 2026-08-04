#pragma once

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>

#include <cstdint>
#include <string>
#include <vector>

class X11WMClient {
public:
	virtual ~X11WMClient() = default;
	virtual void on_frame(const uint8_t *rgba, int width, int height, int stride) = 0;
	virtual void on_log(const char *message) = 0;
	virtual void on_clipboard_text(const std::string &text) = 0;
	virtual std::string get_clipboard_text() = 0;
};

// Embedded X11 window manager (TinyWM / mwm style). Spawns or attaches to an
// X display, manages the windows on it, captures the root framebuffer as an
// RGBA texture and forwards Godot input via XTEST.
class X11WM {
public:
	enum class ServerMode { Xvfb, Xephyr, Attach };
	enum class WmMode { TinyWm, MwmLite, Mwm };

	X11WM();
	~X11WM();

	X11WMClient *client = nullptr;

	bool init(int width, int height, ServerMode server_mode, const std::string &display_name);
	void shutdown();
	bool is_running() const { return display_ != nullptr; }
	const char *display_string() const { return display_str_.c_str(); }

	void pump();
	void set_wm_mode(WmMode mode);
	WmMode wm_mode() const { return wm_mode_; }

	void fake_key(uint32_t keysym, bool pressed);
	void fake_button(unsigned int button, bool pressed);
	void fake_motion(int x, int y);
	void pointer_axis_vertical(double delta);
	void pointer_axis_horizontal(double delta);

	bool launch_app(const std::string &command);
	void push_clipboard(const std::string &text);
	std::string pull_clipboard();

	Display *native_display() const { return display_; }
	XErrorHandler previous_error_handler() const { return prev_error_handler_; }

	static int keysym_from_godot(int key);

private:
	enum class InteractMode { Idle, Move, Resize };

	bool setup_server(int width, int height, ServerMode server_mode, const std::string &display_name);
	void teardown_server();
	Display *open_display_retry(const std::string &name, int attempts);
	bool select_events();
	void setup_ewmh();
	void setup_capture();
	void setup_input_grabs();

	void handle_event(XEvent &ev);
	void on_map_request(XMapRequestEvent *ev);
	void on_configure_request(XConfigureRequestEvent *ev);
	void on_map_notify(XMapEvent *ev);
	void on_unmap_notify(XUnmapEvent *ev);
	void on_destroy_notify(XDestroyWindowEvent *ev);
	void on_button(XButtonEvent *ev);
	void on_button_release(XButtonEvent *ev);
	void on_motion(XMotionEvent *ev);
	void on_key(XKeyEvent *ev);
	void on_selection_request(XSelectionRequestEvent *ev);

	void manage_window(Window w);
	void remove_client(Window w);
	bool in_clients(Window w) const;
	Window toplevel_for(Window w) const;
	void set_focus(Window w);
	void refocus();
	void focus_next();
	void close_window(Window w);
	void set_border(Window w);
	void redraw_borders();
	void update_client_list();

	void maybe_capture();
	void capture();
	void maybe_poll_clipboard();
	void maybe_focus_follows();
	void convert_image_to_rgba(const XImage *img, std::vector<uint8_t> &out);

	static int spawn_process(const std::string &path, const std::vector<std::string> &args);
	static bool wait_for_socket(const std::string &path, int timeout_ms);
	static uint64_t now_ms();

	Display *display_ = nullptr;
	Window root_ = 0;
	int screen_ = 0;
	std::string display_str_;
	int width_ = 0;
	int height_ = 0;
	int server_pid_ = -1;
	bool spawned_ = false;
	bool bad_access_ = false;

	WmMode wm_mode_ = WmMode::MwmLite;
	int border_width_ = 2;
	unsigned long border_pixel_ = 0x00606060;
	unsigned long focus_border_pixel_ = 0x00ffffff;

	std::vector<Window> clients_;
	Window focused_ = 0;
	Window owner_window_ = 0;

	InteractMode interact_mode_ = InteractMode::Idle;
	Window interact_window_ = 0;
	int press_x_ = 0;
	int press_y_ = 0;
	int press_dx_ = 0;
	int press_dy_ = 0;
	int start_w_ = 0;
	int start_h_ = 0;

	XImage *shm_image_ = nullptr;
	XShmSegmentInfo shm_info_{};
	bool shm_attached_ = false;
	bool damage_available_ = false;
	int damage_event_base_ = 0;
	int damage_error_base_ = 0;
	Damage damage_ = 0;
	bool damage_pending_ = true;
	std::vector<uint8_t> rgba_buf_;
	uint64_t last_capture_ms_ = 0;

	Atom wm_protocols_ = 0;
	Atom wm_delete_window_ = 0;
	Atom net_wm_name_ = 0;
	Atom utf8_string_ = 0;
	Atom net_supported_ = 0;
	Atom net_supporting_wm_check_ = 0;
	Atom net_active_window_ = 0;
	Atom net_client_list_ = 0;
	Atom targets_ = 0;
	Atom clipboard_ = 0;
	Atom primary_ = 0;
	Atom string_atom_ = 0;
	Atom text_atom_ = 0;
	Atom compound_text_atom_ = 0;

	std::string clipboard_buf_;
	Window clipboard_owner_ = None;
	uint64_t last_clip_poll_ms_ = 0;
	uint64_t last_ffp_ms_ = 0;

	XErrorHandler prev_error_handler_ = nullptr;
};
