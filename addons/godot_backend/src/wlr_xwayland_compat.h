/*
 * C++-safe copy of wlroots-0.19 <wlr/xwayland.h>.
 *
 * The stock header declares a struct field literally named `class`, which is
 * a C++ keyword and cannot be included from C++. This copy renames that one
 * member to `class_field`; the struct layout is otherwise byte-identical, so
 * ABI with libwlroots-0.19 is preserved. We never access the renamed field.
 *
 * Update this file whenever the pinned wlroots version changes.
 */
#ifndef WLR_XWAYLAND_COMPAT_H
#define WLR_XWAYLAND_COMPAT_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <wlr/util/addon.h>

struct wlr_box;
struct wlr_xwm;
struct wlr_data_source;
struct wlr_drag;

struct wlr_xwayland {
	struct wlr_xwayland_server *server;
	bool own_server;
	struct wlr_xwm *xwm;
	struct wlr_xwayland_shell_v1 *shell_v1;
	struct wlr_xwayland_cursor *cursor;

	const char *display_name;

	struct wl_display *wl_display;
	struct wlr_compositor *compositor;
	struct wlr_seat *seat;

	struct {
		struct wl_signal destroy;
		struct wl_signal ready;
		struct wl_signal new_surface; // struct wlr_xwayland_surface
		struct wl_signal remove_startup_info;
	} events;

	bool (*user_event_handler)(struct wlr_xwayland *wlr_xwayland,
		xcb_generic_event_t *event);

	void *data;

	struct {
		struct wl_listener server_start;
		struct wl_listener server_ready;
		struct wl_listener server_destroy;
		struct wl_listener seat_destroy;
		struct wl_listener shell_destroy;
	} WLR_PRIVATE;
};

enum wlr_xwayland_surface_decorations {
	WLR_XWAYLAND_SURFACE_DECORATIONS_ALL = 0,
	WLR_XWAYLAND_SURFACE_DECORATIONS_NO_BORDER = 1,
	WLR_XWAYLAND_SURFACE_DECORATIONS_NO_TITLE = 2,
};

enum wlr_xwayland_icccm_input_model {
	WLR_ICCCM_INPUT_MODEL_NONE = 0,
	WLR_ICCCM_INPUT_MODEL_PASSIVE = 1,
	WLR_ICCCM_INPUT_MODEL_LOCAL = 2,
	WLR_ICCCM_INPUT_MODEL_GLOBAL = 3,
};

enum wlr_xwayland_net_wm_window_type {
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DESKTOP = 0,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DOCK,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,
	WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NORMAL,
};

struct wlr_xwayland_surface {
	xcb_window_t window_id;
	struct wlr_xwm *xwm;
	uint32_t surface_id;
	uint64_t serial;

	struct wl_list link;
	struct wl_list stack_link;
	struct wl_list unpaired_link;

	struct wlr_surface *surface;
	struct wlr_addon surface_addon;

	int16_t x, y;
	uint16_t width, height;
	bool override_redirect;
	float opacity;

	char *title;
	char *class_field; // renamed from `class` (C++ keyword)
	char *instance;
	char *role;
	char *startup_id;
	pid_t pid;
	bool has_utf8_title;

	struct wl_list children;
	struct wlr_xwayland_surface *parent;
	struct wl_list parent_link;

	xcb_atom_t *window_type;
	size_t window_type_len;

	xcb_atom_t *protocols;
	size_t protocols_len;

	uint32_t decorations;
	xcb_icccm_wm_hints_t *hints;
	xcb_size_hints_t *size_hints;
	xcb_ewmh_wm_strut_partial_t *strut_partial;

	bool pinging;
	struct wl_event_source *ping_timer;

	bool modal;
	bool fullscreen;
	bool maximized_vert, maximized_horz;
	bool minimized;
	bool withdrawn;
	bool sticky;
	bool shaded;
	bool skip_taskbar;
	bool skip_pager;
	bool above;
	bool below;
	bool demands_attention;

	bool has_alpha;

	struct {
		struct wl_signal destroy;
		struct wl_signal request_configure; // struct wlr_xwayland_surface_configure_event
		struct wl_signal request_move;
		struct wl_signal request_resize;
		struct wl_signal request_minimize;
		struct wl_signal request_maximize;
		struct wl_signal request_fullscreen;
		struct wl_signal request_activate;
		struct wl_signal request_close;
		struct wl_signal request_sticky;
		struct wl_signal request_shaded;
		struct wl_signal request_skip_taskbar;
		struct wl_signal request_skip_pager;
		struct wl_signal request_above;
		struct wl_signal request_below;
		struct wl_signal request_demands_attention;

		struct wl_signal associate;
		struct wl_signal dissociate;

		struct wl_signal set_title;
		struct wl_signal set_class;
		struct wl_signal set_role;
		struct wl_signal set_parent;
		struct wl_signal set_startup_id;
		struct wl_signal set_window_type;
		struct wl_signal set_hints;
		struct wl_signal set_decorations;
		struct wl_signal set_strut_partial;
		struct wl_signal set_override_redirect;
		struct wl_signal set_geometry;
		struct wl_signal set_opacity;
		struct wl_signal focus_in;
		struct wl_signal grab_focus;
		struct wl_signal map_request;
		struct wl_signal ping_timeout;
	} events;

	void *data;

	struct {
		char *wm_name, *net_wm_name;

		struct wl_listener surface_commit;
		struct wl_listener surface_map;
		struct wl_listener surface_unmap;
	} WLR_PRIVATE;
};

struct wlr_xwayland_surface_configure_event {
	struct wlr_xwayland_surface *surface;
	int16_t x, y;
	uint16_t width, height;
	uint16_t mask; // xcb_config_window_t
};

struct wlr_xwayland_remove_startup_info_event {
	const char *id;
	xcb_window_t window;
};

struct wlr_xwayland_resize_event {
	struct wlr_xwayland_surface *surface;
	uint32_t edges;
};

struct wlr_xwayland_minimize_event {
	struct wlr_xwayland_surface *surface;
	bool minimize;
};

struct wlr_xwayland *wlr_xwayland_create(struct wl_display *wl_display,
	struct wlr_compositor *compositor, bool lazy);

struct wlr_xwayland *wlr_xwayland_create_with_server(struct wl_display *display,
	struct wlr_compositor *compositor, struct wlr_xwayland_server *server);

void wlr_xwayland_destroy(struct wlr_xwayland *wlr_xwayland);

void wlr_xwayland_set_cursor(struct wlr_xwayland *wlr_xwayland,
	uint8_t *pixels, uint32_t stride, uint32_t width, uint32_t height,
	int32_t hotspot_x, int32_t hotspot_y);

void wlr_xwayland_surface_activate(struct wlr_xwayland_surface *surface,
	bool activated);

void wlr_xwayland_surface_restack(struct wlr_xwayland_surface *surface,
	struct wlr_xwayland_surface *sibling, enum xcb_stack_mode_t mode);

void wlr_xwayland_surface_configure(struct wlr_xwayland_surface *surface,
	int16_t x, int16_t y, uint16_t width, uint16_t height);

void wlr_xwayland_surface_close(struct wlr_xwayland_surface *surface);

void wlr_xwayland_surface_set_withdrawn(struct wlr_xwayland_surface *surface,
	bool withdrawn);

void wlr_xwayland_surface_set_minimized(struct wlr_xwayland_surface *surface,
	bool minimized);

void wlr_xwayland_surface_set_maximized(struct wlr_xwayland_surface *surface,
	bool maximized_horz, bool maximized_vert);

void wlr_xwayland_surface_set_fullscreen(struct wlr_xwayland_surface *surface,
	bool fullscreen);

void wlr_xwayland_surface_set_sticky(
	struct wlr_xwayland_surface *surface, bool sticky);

void wlr_xwayland_surface_set_shaded(
	struct wlr_xwayland_surface *surface, bool shaded);

void wlr_xwayland_surface_set_skip_taskbar(
	struct wlr_xwayland_surface *surface, bool skip_taskbar);

void wlr_xwayland_surface_set_skip_pager(
	struct wlr_xwayland_surface *surface, bool skip_pager);

void wlr_xwayland_surface_set_above(
	struct wlr_xwayland_surface *surface, bool above);

void wlr_xwayland_surface_set_below(
	struct wlr_xwayland_surface *surface, bool below);

void wlr_xwayland_surface_set_demands_attention(
	struct wlr_xwayland_surface *surface, bool demands_attention);

void wlr_xwayland_set_seat(struct wlr_xwayland *xwayland,
	struct wlr_seat *seat);

struct wlr_xwayland_surface *wlr_xwayland_surface_try_from_wlr_surface(
	struct wlr_surface *surface);

void wlr_xwayland_surface_offer_focus(struct wlr_xwayland_surface *xsurface);

void wlr_xwayland_surface_ping(struct wlr_xwayland_surface *surface);

bool wlr_xwayland_surface_has_window_type(
	const struct wlr_xwayland_surface *xsurface,
	enum wlr_xwayland_net_wm_window_type window_type);

bool wlr_xwayland_surface_override_redirect_wants_focus(
	const struct wlr_xwayland_surface *xsurface);

enum wlr_xwayland_icccm_input_model wlr_xwayland_surface_icccm_input_model(
	const struct wlr_xwayland_surface *xsurface);

void wlr_xwayland_set_workareas(struct wlr_xwayland *wlr_xwayland,
	const struct wlr_box *workareas, size_t num_workareas);

xcb_connection_t *wlr_xwayland_get_xwm_connection(
	struct wlr_xwayland *wlr_xwayland);

#endif // WLR_XWAYLAND_COMPAT_H
