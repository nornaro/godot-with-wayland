#include "compositor.hpp"

#include <cerrno>
#include <cstring>
#include <drm_fourcc.h>
#include <algorithm>
#include <execinfo.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

// wlroots headers are plain C headers without `extern "C"` guards, so they
// must be wrapped for C++ linking.
extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/backend/wayland.h>
#include <wlr/interfaces/wlr_keyboard.h>

#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pixman.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

// wlr/xwayland.h cannot be parsed from C++ (its `struct wlr_xwayland_surface`
// declares a member literally named `class`). Use a C++-safe copy that renames
// that one member; layout is identical so ABI with libwlroots-0.19 holds.
#include "wlr_xwayland_compat.h"

// wlr_scene.h uses the C99 `[static N]` array parameter syntax, which is not
// valid C++. The header contains no other `static` declarations, so we can
// safely strip the keyword just while including it.
#define static /* no-op */
#include <wlr/types/wlr_scene.h>
#undef static
} // extern "C"

extern "C" struct wlr_allocator *wlr_gbm_allocator_create(int drm_fd);

// ---------------------------------------------------------------------------
// wlroots log handler forwarding to the Godot side
// ---------------------------------------------------------------------------

static Compositor *g_log_compositor = nullptr;

static void crash_handler(int sig) {
	const char *msg = "*** native crash (segv/bus/abort) ***\n";
	ssize_t unused = write(STDERR_FILENO, msg, strlen(msg));
	(void)unused;
	int fd = open("/tmp/opencode/crash.log",
		O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		void *frames[32];
		int n = backtrace(frames, 32);
		dprintf(fd, "signal %d (%s)\n", sig, strsignal(sig));
		backtrace_symbols_fd(frames, n, fd);
		close(fd);
	}
	_exit(128 + sig);
}

static void install_crash_handler() {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = crash_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESETHAND;
	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
}

static void log_callback(enum wlr_log_importance importance, const char *fmt,
		va_list args) {
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, args);
	if (g_log_compositor && g_log_compositor->client) {
		g_log_compositor->client->on_log(buf);
	} else {
		fprintf(stderr, "[wlroots] %s\n", buf);
	}
	(void)importance;
}

// ---------------------------------------------------------------------------
// Backend selection logic
// ---------------------------------------------------------------------------

static struct wlr_backend *create_backend(struct wl_display *display) {
	// This compositor always renders into a virtual headless output whose
	// buffer is read back into the Godot texture (see init() where
	// wlr_headless_add_output is used unconditionally). Real wlroots backends
	// (X11, KMS/DRM, nested Wayland) create their own outputs of arbitrary
	// size and would abort inside wlr_headless_add_output(), so headless is
	// the only supported mode here. X11 apps run inside via rootless XWayland.
	struct wlr_backend *backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
	if (backend == nullptr) {
		wlr_log(WLR_ERROR, "failed to create headless backend");
		return nullptr;
	}
	return backend;
}

// ---------------------------------------------------------------------------
// Static listeners
// ---------------------------------------------------------------------------

static void handle_new_output(struct wl_listener *listener, void *data);
static void handle_output_frame(struct wl_listener *listener, void *data);
static void handle_output_request_state(struct wl_listener *listener, void *data);
static void handle_output_destroy(struct wl_listener *listener, void *data);
static void handle_new_xdg_toplevel(struct wl_listener *listener, void *data);
static void handle_new_xdg_popup(struct wl_listener *listener, void *data);
static void handle_request_set_cursor(struct wl_listener *listener, void *data);
static void handle_request_set_selection(struct wl_listener *listener, void *data);
static void handle_host_clipboard_destroy(struct wl_listener *listener, void *data);
static void handle_keyboard_modifiers(struct wl_listener *listener, void *data);
static void handle_keyboard_key(struct wl_listener *listener, void *data);

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "godot-keyboard",
	.led_update = nullptr,
};

static const struct wlr_pointer_impl pointer_impl = {
	.name = "godot-pointer",
};

// ---------------------------------------------------------------------------
// Clipboard bridge
// ---------------------------------------------------------------------------
//
// Two directions:
//   * client -> host: when a client sets a selection, we accept it and read
//     the text out of its data source through a pipe, then hand it to the
//     Godot client.
//   * host -> client: we keep a wlr_data_source of our own as the seat
//     selection. When a client asks to receive, our `send` impl writes the
//     current host clipboard text to the client's pipe. The seat selection is
//     (re)created lazily via clipboard_sync() whenever the host text changes.

struct HostClipboardSource {
	struct wlr_data_source source;
	Compositor *compositor;
};

struct ClipboardRead {
	Compositor *compositor;
	int fd = -1;
	std::string buffer;
};

static void host_clipboard_send(struct wlr_data_source *source, const char *mime_type,
		int32_t fd) {
	struct HostClipboardSource *host = wl_container_of(source, host, source);
	std::string text;
	if (host->compositor->client) {
		text = host->compositor->client->get_clipboard_text();
	}
	size_t off = 0;
	while (off < text.size()) {
		ssize_t n = write(fd, text.data() + off, text.size() - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		off += (size_t)n;
	}
	close(fd);
	(void)mime_type;
}

static void host_clipboard_accept(struct wlr_data_source *source, uint32_t serial,
		const char *mime_type) {
	(void)source;
	(void)serial;
	(void)mime_type;
}

static void host_clipboard_destroy(struct wlr_data_source *source) {
	struct HostClipboardSource *host = wl_container_of(source, host, source);
	free(host);
}

static const struct wlr_data_source_impl host_clipboard_impl = {
	.send = host_clipboard_send,
	.accept = host_clipboard_accept,
	.destroy = host_clipboard_destroy,
};

static void handle_host_clipboard_destroy(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, host_clipboard_destroy);
	c->host_clipboard = nullptr;
	wl_list_remove(&c->host_clipboard_destroy.link);
	(void)data;
}

static const char *pick_text_mime(struct wlr_data_source *source) {
	const char *plain = nullptr;
	const char *any_text = nullptr;
	char *data = (char *)source->mime_types.data;
	char *end = data + source->mime_types.size;
	for (char *p = data; p < end; p += sizeof(char *)) {
		const char *mime = *(const char **)p;
		if (mime == nullptr) {
			continue;
		}
		if (strcmp(mime, "text/plain;charset=utf-8") == 0) {
			return mime;
		}
		if (strcmp(mime, "text/plain") == 0) {
			if (plain == nullptr) {
				plain = mime;
			}
			continue;
		}
		if (strncmp(mime, "text/", 5) == 0 && any_text == nullptr) {
			any_text = mime;
		}
	}
	if (plain != nullptr) {
		return plain;
	}
	return any_text;
}

void Compositor::on_client_selection(struct wlr_data_source *source, uint32_t serial) {
	(void)serial;
	if (source == nullptr) {
		return;
	}
	const char *mime = pick_text_mime(source);
	if (mime == nullptr) {
		return;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		return;
	}
	int flags = fcntl(fds[0], F_GETFL, 0);
	fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	ClipboardRead *read = new ClipboardRead();
	read->compositor = this;
	read->fd = fds[0];
	// wlroots closes fds[1] for client sources after relaying the receive.
	wlr_data_source_send(source, mime, fds[1]);
	active_reads.push_back(read);
}

void Compositor::clipboard_sync() {
	if (!running() || seat == nullptr) {
		return;
	}
	if (host_clipboard != nullptr) {
		return;
	}
	HostClipboardSource *host = (HostClipboardSource *)calloc(1, sizeof(*host));
	if (host == nullptr) {
		return;
	}
	wlr_data_source_init(&host->source, &host_clipboard_impl);
	host->compositor = this;
	char **p = (char **)wl_array_add(&host->source.mime_types, sizeof(char *));
	if (p != nullptr) {
		*p = strdup("text/plain;charset=utf-8");
	}
	p = (char **)wl_array_add(&host->source.mime_types, sizeof(char *));
	if (p != nullptr) {
		*p = strdup("text/plain");
	}
	host_clipboard_destroy.notify = handle_host_clipboard_destroy;
	wl_signal_add(&host->source.events.destroy, &host_clipboard_destroy);
	wlr_seat_set_selection(seat, &host->source, seat->selection_serial);
	host_clipboard = host;
}

void Compositor::pump_clipboard_reads() {
	if (active_reads.empty()) {
		return;
	}
	std::vector<struct pollfd> fds;
	fds.reserve(active_reads.size());
	for (ClipboardRead *r : active_reads) {
		fds.push_back({r->fd, POLLIN, 0});
	}
	int ret = poll(fds.data(), fds.size(), 0);
	if (ret <= 0) {
		return;
	}
	for (size_t i = 0; i < active_reads.size();) {
		ClipboardRead *r = active_reads[i];
		if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
			i++;
			continue;
		}
		bool done = false;
		for (;;) {
			char buf[4096];
			ssize_t n = read(r->fd, buf, sizeof(buf));
			if (n > 0) {
				r->buffer.append(buf, (size_t)n);
				continue;
			}
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				break;
			}
			done = true;
			break;
		}
		if (done) {
			if (!r->buffer.empty() && client) {
				client->on_clipboard_text(r->buffer);
			}
			close(r->fd);
			delete r;
			active_reads.erase(active_reads.begin() + (long)i);
			fds.erase(fds.begin() + (long)i);
			// Make the compositor's own source the seat selection again so
			// pasting into another client serves the same text.
			clipboard_sync();
		} else {
			i++;
		}
	}
}

void Compositor::cleanup_clipboard() {
	for (ClipboardRead *r : active_reads) {
		if (r->fd >= 0) {
			close(r->fd);
		}
		delete r;
	}
	active_reads.clear();
	if (host_clipboard != nullptr) {
		wl_list_remove(&host_clipboard_destroy.link);
		wlr_data_source_destroy(&host_clipboard->source);
		host_clipboard = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Toplevel / popup handling (ported from tinywl)
// ---------------------------------------------------------------------------

static void focus_toplevel(Compositor *c, Toplevel *toplevel) {
	if (toplevel == nullptr) {
		return;
	}
	struct wlr_seat *seat = c->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->is_x11 ?
		toplevel->xwayland_surface->surface : toplevel->xdg_toplevel->base->surface;
	{
		FILE *f = fopen("/tmp/opencode/input.log", "a");
		if (f) {
			fprintf(f, "focus -> surface=%p app_id=%s\n", (void *)surface,
				toplevel->is_x11 ?
					(toplevel->xwayland_surface->instance ?
						toplevel->xwayland_surface->instance : "(x11)") :
					(toplevel->xdg_toplevel->app_id ?
						toplevel->xdg_toplevel->app_id : "(null)"));
			fclose(f);
		}
	}
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface) {
		struct wlr_xwayland_surface *prev_xwl =
			wlr_xwayland_surface_try_from_wlr_surface(prev_surface);
		if (prev_xwl != nullptr) {
			wlr_xwayland_surface_activate(prev_xwl, false);
		} else {
			struct wlr_xdg_toplevel *prev_toplevel =
				wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
			if (prev_toplevel != nullptr) {
				wlr_xdg_toplevel_set_activated(prev_toplevel, false);
			}
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	wlr_scene_node_raise_to_top(&toplevel->tree->node);
	if (toplevel->link.next != nullptr) {
		wl_list_remove(&toplevel->link);
	}
	wl_list_insert(&c->toplevels, &toplevel->link);
	if (toplevel->is_x11) {
		wlr_xwayland_surface_activate(toplevel->xwayland_surface, true);
	} else {
		wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	}
	if (keyboard != nullptr) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, map);
	wl_list_insert(&toplevel->compositor->toplevels, &toplevel->link);
	focus_toplevel(toplevel->compositor, toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	Compositor *c = toplevel->compositor;
	if (toplevel == c->grabbed_toplevel) {
		c->reset_cursor_mode();
	}
	wl_list_remove(&toplevel->link);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, commit);
	Compositor *c = toplevel->compositor;
	c->commit_count++;
	c->commit_time = Compositor::now_msec();
	if (c->commit_time - c->commit_window_start >= 1000) {
		fprintf(stderr, "[perf] client commits: %u/s\n", c->commit_count);
		c->commit_count = 0;
		c->commit_window_start = c->commit_time;
	}
	if (toplevel->xdg_toplevel->base->initial_commit) {
		if (toplevel->fullscreen) {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
				c->output_width, c->output_height);
			toplevel->fullscreen_resized = true;
		} else {
			// Let the client pick its own size on the first configure instead
			// of forcing 0x0, which Qt6 clients apply literally and never grow.
			wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
		}
	}
	struct wlr_xdg_surface *base = toplevel->xdg_toplevel->base;

	const int w = base->geometry.width > 0 ?
		base->geometry.width : base->surface->current.width;
	const int h = base->geometry.height > 0 ?
		base->geometry.height : base->surface->current.height;

	if (toplevel->fullscreen && !toplevel->fullscreen_resized && w > 0 && h > 0) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			c->output_width, c->output_height);
		toplevel->fullscreen_resized = true;
	}

	if (w > 0 && h > 0) {
		if (toplevel->fullscreen) {
			wlr_scene_node_set_position(&toplevel->tree->node, 0, 0);
			wlr_scene_node_set_position(&toplevel->scene_tree->node, 0, 0);
		} else {
			if (toplevel->frame != nullptr) {
				wlr_scene_rect_set_size(toplevel->frame, w, Toplevel::TITLE_BAR_HEIGHT);
				wlr_scene_node_set_position(&toplevel->close_btn->node,
					w - Toplevel::CLOSE_BUTTON_SIZE, 0);
			}
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
				0, c->decorations_enabled ? Toplevel::TITLE_BAR_HEIGHT : 0);
		}
	}

	if (!toplevel->centered && !toplevel->fullscreen && w > 0 && h > 0) {
		toplevel->centered = true;
		const int title_h = c->decorations_enabled ? Toplevel::TITLE_BAR_HEIGHT : 0;
		const double pos_x = (c->output_width - w) / 2.0;
		const double pos_y = (c->output_height - (h + title_h)) / 2.0;
		wlr_scene_node_set_position(&toplevel->tree->node,
			pos_x > 0.0 ? pos_x : 0.0, pos_y > 0.0 ? pos_y : 0.0);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	if (toplevel->decoration != nullptr) {
		wl_list_remove(&toplevel->decoration_request_mode.link);
		wl_list_remove(&toplevel->decoration_destroy.link);
	}
	free(toplevel);
}

static void decoration_request_mode(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, decoration_request_mode);
	wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration,
		toplevel->compositor->decorations_enabled
			? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
			: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, decoration_destroy);
	wl_list_remove(&toplevel->decoration_request_mode.link);
	wl_list_remove(&toplevel->decoration_destroy.link);
	toplevel->decoration = nullptr;
}

static void handle_new_decoration(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, new_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *decoration =
		(struct wlr_xdg_toplevel_decoration_v1 *)data;

	Toplevel *toplevel = nullptr;
	Toplevel *iter;
	wl_list_for_each(iter, &c->toplevels, link) {
		if (iter->xdg_toplevel == decoration->toplevel) {
			toplevel = iter;
			break;
		}
	}
	if (toplevel == nullptr) {
		return;
	}

	toplevel->decoration = decoration;
	toplevel->decoration_request_mode.notify = decoration_request_mode;
	wl_signal_add(&decoration->events.request_mode, &toplevel->decoration_request_mode);
	toplevel->decoration_destroy.notify = decoration_destroy;
	wl_signal_add(&decoration->events.destroy, &toplevel->decoration_destroy);
	wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	toplevel->compositor->begin_interactive(toplevel, Compositor::CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	const struct wlr_xdg_toplevel_resize_event *event =
		(const struct wlr_xdg_toplevel_resize_event *)data;
	toplevel->compositor->begin_interactive(toplevel,
		Compositor::CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
	Compositor *c = toplevel->compositor;
	bool fullscreen = toplevel->xdg_toplevel->requested.fullscreen;
	if (fullscreen == toplevel->fullscreen) {
		return;
	}
	toplevel->fullscreen = fullscreen;
	toplevel->fullscreen_resized = false;
	toplevel->centered = false;
	if (fullscreen) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			c->output_width, c->output_height);
		toplevel->fullscreen_resized = true;
		wlr_scene_node_set_position(&toplevel->tree->node, 0, 0);
	} else if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	Popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	Popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = (struct wlr_xdg_toplevel *)data;

	Toplevel *toplevel = (Toplevel *)calloc(1, sizeof(*toplevel));
	toplevel->compositor = c;
	toplevel->xdg_toplevel = xdg_toplevel;
	if (c->fullscreen_apps) {
		toplevel->fullscreen = true;
	}
	toplevel->tree = wlr_scene_tree_create(&c->scene->tree);
	toplevel->tree->node.data = toplevel;
	if (c->decorations_enabled) {
		toplevel->frame = wlr_scene_rect_create(toplevel->tree,
			Toplevel::TITLE_BAR_HEIGHT, Toplevel::TITLE_BAR_HEIGHT,
			(float[4]){0.13f, 0.16f, 0.22f, 1.0f});
		toplevel->frame->node.data = toplevel;
		toplevel->close_btn = wlr_scene_rect_create(toplevel->tree,
			Toplevel::CLOSE_BUTTON_SIZE, Toplevel::CLOSE_BUTTON_SIZE,
			(float[4]){0.72f, 0.18f, 0.18f, 1.0f});
		toplevel->close_btn->node.data = toplevel;
	}
	toplevel->scene_tree =
		wlr_scene_xdg_surface_create(toplevel->tree, xdg_toplevel->base);
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		0, c->decorations_enabled ? Toplevel::TITLE_BAR_HEIGHT : 0);
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

// ---------------------------------------------------------------------------
// XWayland surface handling (X11 apps like tint3 through rootless Xwayland)
// ---------------------------------------------------------------------------

static void xwayland_surface_commit(struct wl_listener *listener, void *data);
static void xwayland_surface_map(struct wl_listener *listener, void *data);
static void xwayland_surface_unmap(struct wl_listener *listener, void *data);

static void xwayland_surface_map(struct wl_listener *listener, void *data) {
	(void)data;
	Toplevel *toplevel = wl_container_of(listener, toplevel, map);
	Compositor *c = toplevel->compositor;
	struct wlr_xwayland_surface *xwl = toplevel->xwayland_surface;
	if (toplevel->scene_surface == nullptr) {
		toplevel->scene_surface =
			wlr_scene_surface_create(toplevel->tree, xwl->surface);
		toplevel->scene_surface->buffer->node.data = toplevel;
		toplevel->commit.notify = xwayland_surface_commit;
		wl_signal_add(&xwl->surface->events.commit, &toplevel->commit);
	}
	if (!xwl->override_redirect) {
		wl_list_insert(&c->toplevels, &toplevel->link);
		focus_toplevel(c, toplevel);
	}
}

static void xwayland_surface_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	Toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	Compositor *c = toplevel->compositor;
	if (toplevel == c->grabbed_toplevel) {
		c->reset_cursor_mode();
	}
	if (toplevel->link.next != nullptr) {
		wl_list_remove(&toplevel->link);
	}
}

static void xwayland_surface_associate(struct wl_listener *listener, void *data) {
	(void)data;
	Toplevel *toplevel = wl_container_of(listener, toplevel, associate);
	struct wlr_xwayland_surface *xwl = toplevel->xwayland_surface;
	if (xwl->surface == nullptr) {
		return;
	}
	if (toplevel->map.link.next == nullptr) {
		toplevel->map.notify = xwayland_surface_map;
		wl_signal_add(&xwl->surface->events.map, &toplevel->map);
	}
	if (toplevel->unmap.link.next == nullptr) {
		toplevel->unmap.notify = xwayland_surface_unmap;
		wl_signal_add(&xwl->surface->events.unmap, &toplevel->unmap);
	}
}

static void xwayland_surface_dissociate(struct wl_listener *listener, void *data) {
	(void)data;
	Toplevel *toplevel = wl_container_of(listener, toplevel, dissociate);
	// The associated wlr_surface is going away: detach every listener we put
	// on it and drop the scene surface before it turns into a dangling pointer.
	if (toplevel->map.link.next != nullptr) {
		wl_list_remove(&toplevel->map.link);
	}
	if (toplevel->unmap.link.next != nullptr) {
		wl_list_remove(&toplevel->unmap.link);
	}
	if (toplevel->commit.link.next != nullptr) {
		wl_list_remove(&toplevel->commit.link);
	}
	toplevel->scene_surface = nullptr;
	toplevel->centered = false;
	if (toplevel->link.next != nullptr) {
		wl_list_remove(&toplevel->link);
	}
}

static void xwayland_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	Toplevel *toplevel = wl_container_of(listener, toplevel, commit);
	Compositor *c = toplevel->compositor;
	struct wlr_xwayland_surface *xwl = toplevel->xwayland_surface;
	if (xwl == nullptr || xwl->surface == nullptr) {
		return;
	}
	const int w = xwl->surface->current.width;
	const int h = xwl->surface->current.height;
	if (w <= 0 || h <= 0) {
		return;
	}
	if (toplevel->fullscreen) {
		wlr_scene_node_set_position(&toplevel->tree->node, 0, 0);
		return;
	}
	if (xwl->override_redirect) {
		// Docks/panels position themselves; honor their requested placement.
		wlr_scene_node_set_position(&toplevel->tree->node, xwl->x, xwl->y);
		return;
	}
	if (!toplevel->centered) {
		toplevel->centered = true;
		const double pos_x = (c->output_width - w) / 2.0;
		const double pos_y = (c->output_height - h) / 2.0;
		wlr_scene_node_set_position(&toplevel->tree->node,
			pos_x > 0.0 ? pos_x : 0.0, pos_y > 0.0 ? pos_y : 0.0);
		wlr_xwayland_surface_configure(xwl,
			(int16_t)(pos_x > 0.0 ? pos_x : 0.0),
			(int16_t)(pos_y > 0.0 ? pos_y : 0.0), (uint16_t)w, (uint16_t)h);
	}
}

static void xwayland_surface_request_configure(struct wl_listener *listener, void *data) {
	Toplevel *toplevel = wl_container_of(listener, toplevel, request_configure);
	struct wlr_xwayland_surface_configure_event *event =
		(struct wlr_xwayland_surface_configure_event *)data;
	wlr_xwayland_surface_configure(toplevel->xwayland_surface,
		event->x, event->y, event->width, event->height);
}

static void xwayland_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	Toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
	// dissociate may have detached these already; each remove must be guarded
	// because wl_list_remove on a previously-removed (NULLed) link is UB.
	if (toplevel->map.link.next != nullptr) {
		wl_list_remove(&toplevel->map.link);
	}
	if (toplevel->unmap.link.next != nullptr) {
		wl_list_remove(&toplevel->unmap.link);
	}
	if (toplevel->commit.link.next != nullptr) {
		wl_list_remove(&toplevel->commit.link);
	}
	if (toplevel->destroy.link.next != nullptr) {
		wl_list_remove(&toplevel->destroy.link);
	}
	if (toplevel->request_configure.link.next != nullptr) {
		wl_list_remove(&toplevel->request_configure.link);
	}
	if (toplevel->associate.link.next != nullptr) {
		wl_list_remove(&toplevel->associate.link);
	}
	if (toplevel->dissociate.link.next != nullptr) {
		wl_list_remove(&toplevel->dissociate.link);
	}
	free(toplevel);
}

static void handle_new_xwayland_surface(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, new_xwayland_surface);
	struct wlr_xwayland_surface *xwl = (struct wlr_xwayland_surface *)data;

	Toplevel *toplevel = (Toplevel *)calloc(1, sizeof(*toplevel));
	toplevel->compositor = c;
	toplevel->is_x11 = true;
	toplevel->xwayland_surface = xwl;
	if (c->fullscreen_apps) {
		toplevel->fullscreen = true;
	}
	toplevel->tree = wlr_scene_tree_create(&c->scene->tree);
	toplevel->tree->node.data = toplevel;

	toplevel->destroy.notify = xwayland_surface_destroy;
	wl_signal_add(&xwl->events.destroy, &toplevel->destroy);
	toplevel->request_configure.notify = xwayland_surface_request_configure;
	wl_signal_add(&xwl->events.request_configure, &toplevel->request_configure);
	toplevel->associate.notify = xwayland_surface_associate;
	wl_signal_add(&xwl->events.associate, &toplevel->associate);
	toplevel->dissociate.notify = xwayland_surface_dissociate;
	wl_signal_add(&xwl->events.dissociate, &toplevel->dissociate);
}

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = (struct wlr_xdg_popup *)data;

	Popup *popup = (Popup *)calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (parent == nullptr) {
		wlr_log(WLR_ERROR, "xdg popup with unknown parent");
		free(popup);
		return;
	}
	struct wlr_scene_tree *parent_tree = (struct wlr_scene_tree *)parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

// ---------------------------------------------------------------------------
// Output handling
// ---------------------------------------------------------------------------

static void handle_new_output(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, new_output);
	struct wlr_output *output = (struct wlr_output *)data;

	wlr_output_init_render(output, c->allocator, c->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_commit_state(output, &state);
	wlr_output_state_finish(&state);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(c->output_layout, output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(c->scene, output);
	wlr_scene_output_layout_add_output(c->scene_layout, l_output, scene_output);

	c->output = output;
	c->scene_output = scene_output;

	c->output_frame.notify = handle_output_frame;
	wl_signal_add(&output->events.frame, &c->output_frame);
	c->output_request_state.notify = handle_output_request_state;
	wl_signal_add(&output->events.request_state, &c->output_request_state);
	c->output_destroy.notify = handle_output_destroy;
	wl_signal_add(&output->events.destroy, &c->output_destroy);
}

static void handle_output_frame(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, output_frame);
	c->render_frame();
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(c->scene_output, &now);
}

static void handle_output_request_state(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, output_request_state);
	const struct wlr_output_event_request_state *event =
		(const struct wlr_output_event_request_state *)data;
	wlr_output_commit_state(c->output, event->state);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, output_destroy);
	wl_list_remove(&c->output_frame.link);
	wl_list_remove(&c->output_request_state.link);
	wl_list_remove(&c->output_destroy.link);
	c->output = nullptr;
	c->scene_output = nullptr;
}

// ---------------------------------------------------------------------------
// Seat / cursor handling
// ---------------------------------------------------------------------------

static void handle_request_set_cursor(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, request_set_cursor);
	const struct wlr_seat_pointer_request_set_cursor_event *event =
		(const struct wlr_seat_pointer_request_set_cursor_event *)data;
	struct wlr_seat_client *focused_client = c->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(c->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, request_set_selection);
	const struct wlr_seat_request_set_selection_event *event =
		(const struct wlr_seat_request_set_selection_event *)data;
	wlr_seat_set_selection(c->seat, event->source, event->serial);
	c->on_client_selection(event->source, event->serial);
}

static void handle_keyboard_modifiers(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, keyboard_modifiers);
	wlr_seat_set_keyboard(c->seat, c->keyboard);
	wlr_seat_keyboard_notify_modifiers(c->seat, &c->keyboard->modifiers);
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
	Compositor *c = wl_container_of(listener, c, keyboard_key);
	struct wlr_keyboard_key_event *event = (struct wlr_keyboard_key_event *)data;
	wlr_seat_set_keyboard(c->seat, c->keyboard);
	wlr_seat_keyboard_notify_key(c->seat, event->time_msec, event->keycode, event->state);
}

// ---------------------------------------------------------------------------
// Compositor implementation
// ---------------------------------------------------------------------------

uint32_t Compositor::now_msec() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

bool Compositor::init(int width, int height) {
	output_width = width;
	output_height = height;

	install_crash_handler();
	g_log_compositor = this;
	wlr_log_init(WLR_DEBUG, log_callback);

	display = wl_display_create();
	if (display == nullptr) {
		return false;
	}

	// Backend selection based on environment variables and system capabilities
	// This allows integration with Gamescope, mwm, TinyWM, and KMS/DRM.
	// Specialized extensions (e.g. gamescope) can install their own factory.
	backend = backend_factory != nullptr ? backend_factory(display) : create_backend(display);
	if (backend == nullptr) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return false;
	}

	renderer = nullptr;
	allocator = nullptr;
	int drm_fd = -1;
	for (int i = 128; i < 140 && drm_fd < 0; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
	}
	if (drm_fd >= 0) {
		renderer = wlr_gles2_renderer_create_with_drm_fd(drm_fd);
		if (renderer != nullptr) {
			allocator = wlr_gbm_allocator_create(drm_fd);
		}
		if (allocator == nullptr) {
			wlr_renderer_destroy(renderer);
			renderer = nullptr;
		}
	}
	if (renderer == nullptr) {
		wlr_log(WLR_INFO, "No GPU renderer available, falling back to pixman");
		if (drm_fd >= 0) {
			close(drm_fd);
		}
		renderer = wlr_pixman_renderer_create();
		if (renderer == nullptr) {
			wlr_log(WLR_ERROR, "failed to create wlr_renderer");
			return false;
		}
	}
	wlr_renderer_init_wl_display(renderer, display);

	if (allocator == nullptr) {
		allocator = wlr_allocator_autocreate(backend, renderer);
		if (allocator == nullptr) {
			wlr_log(WLR_ERROR, "failed to create wlr_allocator");
			return false;
		}
	}

	struct wlr_compositor *wlr_compositor_inst = wlr_compositor_create(display, 6, renderer);
	wlr_subcompositor_create(display);
	wlr_data_device_manager_create(display);
	wlr_drm_create(display, renderer);
	// Gamescope's Wayland backend requires the zwp_linux_dmabuf_v1 global, so
	// keep it around even when the renderer has no DRM fd. Build the default
	// feedback from the renderer; if the renderer can't provide one (pixman
	// fallback), skip the global instead of crashing.
	struct wlr_linux_dmabuf_v1 *linux_dmabuf =
		wlr_linux_dmabuf_v1_create_with_renderer(display, 4, renderer);
	if (linux_dmabuf == nullptr) {
		struct wlr_linux_dmabuf_feedback_v1 feedback;
		struct wlr_linux_dmabuf_feedback_v1_init_options options = {};
		options.main_renderer = renderer;
		options.scanout_primary_output = nullptr;
		options.output_layer_feedback_event = nullptr;
		if (wlr_linux_dmabuf_feedback_v1_init_with_options(&feedback, &options)) {
			linux_dmabuf = wlr_linux_dmabuf_v1_create(display, 4, &feedback);
			wlr_linux_dmabuf_feedback_v1_finish(&feedback);
		}
	}
	if (linux_dmabuf == nullptr) {
		wlr_log(WLR_INFO,
			"linux-dmabuf unsupported (no renderer DRM fd); nested gamescope will not work");
	}
	wlr_viewporter_create(display);
	wlr_relative_pointer_manager_v1_create(display);
	wlr_pointer_constraints_v1_create(display);
	if (wlr_presentation_create(display, backend, 1) == nullptr) {
		wlr_log(WLR_ERROR, "failed to create wlr_presentation");
	}
	if (decorations_enabled) {
		wlr_server_decoration_manager_create(display);
		struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager =
			wlr_xdg_decoration_manager_v1_create(display);
		if (xdg_decoration_manager != nullptr) {
			new_decoration.notify = handle_new_decoration;
			wl_signal_add(&xdg_decoration_manager->events.new_toplevel_decoration,
				&new_decoration);
			has_decoration_listener = true;
		}
	}

	output_layout = wlr_output_layout_create(display);
	setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);
	scene = wlr_scene_create();
	scene_layout = wlr_scene_attach_output_layout(scene, output_layout);

	wl_list_init(&toplevels);
	xdg_shell = wlr_xdg_shell_create(display, 3);
	if (xdg_shell == nullptr) {
		wlr_log(WLR_ERROR, "failed to create wlr_xdg_shell");
		return false;
	}
	new_xdg_toplevel.notify = handle_new_xdg_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	new_xdg_popup.notify = handle_new_xdg_popup;
	wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

	new_output.notify = handle_new_output;
	wl_signal_add(&backend->events.new_output, &new_output);

	output = wlr_headless_add_output(backend, width, height);
	if (output == nullptr) {
		wlr_log(WLR_ERROR, "failed to add headless output");
		return false;
	}

	uint64_t implicit_modifier = DRM_FORMAT_MOD_INVALID;
	struct wlr_drm_format drm_format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.capacity = 1,
		.modifiers = &implicit_modifier,
	};
	swapchain = wlr_swapchain_create(allocator, width, height, &drm_format);
	if (swapchain == nullptr) {
		wlr_log(WLR_ERROR, "failed to create swapchain");
		return false;
	}

	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);
	cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);
	wlr_xcursor_manager_load(cursor_mgr, 1.0);

	seat = wlr_seat_create(display, "seat0");
	if (seat == nullptr) {
		wlr_log(WLR_ERROR, "failed to create wlr_seat");
		return false;
	}
	request_set_cursor.notify = handle_request_set_cursor;
	wl_signal_add(&seat->events.request_set_cursor, &request_set_cursor);
	request_set_selection.notify = handle_request_set_selection;
	wl_signal_add(&seat->events.request_set_selection, &request_set_selection);

	// Fabricated keyboard: fed from Godot key events.
	keyboard = new (std::nothrow) struct wlr_keyboard;
	wlr_keyboard_init(keyboard, &keyboard_impl, "godot-keyboard");
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, nullptr,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap == nullptr) {
		// Fall back to a US keymap
		struct xkb_rule_names rules = {
			.rules = nullptr, .model = nullptr, .layout = "us",
			.variant = nullptr, .options = nullptr,
		};
		keymap = xkb_keymap_new_from_names(context, &rules,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	keymap_for_lookup = xkb_keymap_ref(keymap);
	wlr_keyboard_set_keymap(keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(keyboard, 25, 600);

	keyboard_modifiers.notify = handle_keyboard_modifiers;
	wl_signal_add(&keyboard->events.modifiers, &keyboard_modifiers);
	keyboard_key.notify = handle_keyboard_key;
	wl_signal_add(&keyboard->events.key, &keyboard_key);
	wlr_seat_set_keyboard(seat, keyboard);

	// Fabricated pointer: driven via wlr_cursor directly.
	pointer = new (std::nothrow) struct wlr_pointer;
	wlr_pointer_init(pointer, &pointer_impl, "godot-pointer");
	wlr_cursor_attach_input_device(cursor, &pointer->base);

	wlr_seat_set_capabilities(seat,
		WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

	socket_name = wl_display_add_socket_auto(display);
	if (socket_name == nullptr) {
		wlr_log(WLR_ERROR, "failed to add wayland socket");
		return false;
	}

	if (!wlr_backend_start(backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return false;
	}

	// Rootless XWayland so X11 apps (e.g. tint3) run inside the compositor.
	// Lazy: Xwayland is only spawned when the first X11 client connects.
	xwayland = wlr_xwayland_create(display, wlr_compositor_inst, true);
	if (xwayland == nullptr) {
		wlr_log(WLR_ERROR, "failed to create XWayland server");
		return false;
	}
	wlr_xwayland_set_seat(xwayland, seat);
	new_xwayland_surface.notify = handle_new_xwayland_surface;
	wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

	if (client) {
		client->on_socket(socket_name);
	}

	started = true;
	return true;
}

void Compositor::pump() {
	if (!running()) {
		return;
	}
	pump_calls++;
	uint32_t pump_t0 = now_msec();
	if ((pump_calls % 500) == 0) {
		fprintf(stderr, "[perf] pump t=%u\n", pump_t0);
	}
	wl_display_flush_clients(display);
	wl_event_loop_dispatch(wl_display_get_event_loop(display), 0);
	wl_display_flush_clients(display);
	pump_clipboard_reads();
	uint32_t now = now_msec();
	bool due = now - last_render_msec >= 16;
	gate_decisions++;
	if (gate_decisions >= 3000 && gate_decisions < 3020) {
		fprintf(stderr, "[perf] gd #%lu now=%u last=%u diff=%u due=%d fired=%lu\n",
			gate_decisions, now, last_render_msec, now - last_render_msec, due, gate_renders);
	}
	if (due) {
		last_render_msec = now;
		gate_renders++;
		render_frame();
		struct timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
		wlr_scene_output_send_frame_done(scene_output, &t);
	} else {
		gate_skips++;
	}
	uint32_t dur = now_msec() - pump_t0;
	if (dur >= 30) {
		fprintf(stderr, "[perf] SLOW pump %ums due=%d\n", dur, due);
	}
}

void Compositor::shutdown() {
	if (display == nullptr) {
		return;
	}
	wl_display_destroy_clients(display);
	cleanup_clipboard();

	if (xwayland != nullptr) {
		wl_list_remove(&new_xwayland_surface.link);
		wlr_xwayland_destroy(xwayland);
		xwayland = nullptr;
	}
	wl_list_remove(&new_xdg_toplevel.link);
	wl_list_remove(&new_xdg_popup.link);
	wl_list_remove(&new_output.link);
	if (has_decoration_listener) {
		wl_list_remove(&new_decoration.link);
		has_decoration_listener = false;
	}
	wl_list_remove(&request_set_cursor.link);
	wl_list_remove(&request_set_selection.link);
	wl_list_remove(&keyboard_modifiers.link);
	wl_list_remove(&keyboard_key.link);
	if (keyboard) {
		wlr_keyboard_finish(keyboard);
		delete keyboard;
		keyboard = nullptr;
	}
	if (pointer) {
		wlr_pointer_finish(pointer);
		delete pointer;
		pointer = nullptr;
	}
	if (output) {
		wl_list_remove(&output_frame.link);
		wl_list_remove(&output_request_state.link);
		wl_list_remove(&output_destroy.link);
	}

	if (keymap_for_lookup) {
		xkb_keymap_unref(keymap_for_lookup);
		keymap_for_lookup = nullptr;
	}
	if (scene && scene->tree.node.data) {
		wlr_scene_node_destroy(&scene->tree.node);
	}
	scene = nullptr;
	if (cursor_mgr) {
		wlr_xcursor_manager_destroy(cursor_mgr);
		cursor_mgr = nullptr;
	}
	if (cursor) {
		wlr_cursor_destroy(cursor);
		cursor = nullptr;
	}
	if (allocator) {
		wlr_allocator_destroy(allocator);
		allocator = nullptr;
	}
	if (renderer) {
		wlr_renderer_destroy(renderer);
		renderer = nullptr;
	}
	if (backend) {
		wlr_backend_destroy(backend);
		backend = nullptr;
	}
	if (swapchain) {
		wlr_swapchain_destroy(swapchain);
		swapchain = nullptr;
	}
	if (display) {
		wl_display_destroy(display);
		display = nullptr;
	}
	started = false;
}

void Compositor::debug_frame_log() {
	unsigned long total = frame_debug.ok + frame_debug.build_fail + frame_debug.commit_fail;
	if (total - frame_debug.last_log < 100) {
		return;
	}
	frame_debug.last_log = total;
	uint32_t now = now_msec();
	fprintf(stderr, "[perf] framedbg t=%u ok=%lu buildfail=%lu commitfail=%lu ev=%lu pumps=%lu/%lus gate=%lu/%lu\n",
		now, frame_debug.ok, frame_debug.build_fail, frame_debug.commit_fail, frame_events,
		pump_calls - last_log_pumps, (now - last_log_time) / 1000,
		gate_renders - last_log_gate_renders, gate_skips - last_log_gate_skips);
	last_log_pumps = pump_calls;
	last_log_gate_renders = gate_renders;
	last_log_gate_skips = gate_skips;
	last_log_time = now;
}

// ---------------------------------------------------------------------------
// Frame rendering + pixel readback
// ---------------------------------------------------------------------------

void Compositor::render_frame() {
	if (!scene_output || !swapchain) {
		return;
	}
	if (!wlr_scene_output_needs_frame(scene_output)) {
		spurious_renders++;
		if ((spurious_renders % 300) == 300 - 1) {
			fprintf(stderr, "[perf] spurious renders: %u consecutive without damage\n",
				spurious_renders);
		}
		return;
	}
	spurious_renders = 0;

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	struct wlr_scene_output_state_options options = {0};
	options.swapchain = swapchain;

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	if (!wlr_scene_output_build_state(scene_output, &state, &options)) {
		wlr_output_state_finish(&state);
		frame_debug.build_fail++;
		debug_frame_log();
		return;
	}

	if (!wlr_output_commit_state(output, &state)) {
		wlr_output_state_finish(&state);
		frame_debug.commit_fail++;
		debug_frame_log();
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	double t_gpu = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

	if (state.buffer != nullptr) {
		void *data = nullptr;
		uint32_t format = 0;
		size_t stride = 0;
		bool readback_ok = false;
		struct timespec t2, t3;
		clock_gettime(CLOCK_MONOTONIC, &t2);
		if (wlr_buffer_begin_data_ptr_access(state.buffer,
				WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
			copy_to_rgba((const uint8_t *)data, format, stride,
				state.buffer->width, state.buffer->height);
			wlr_buffer_end_data_ptr_access(state.buffer);
			readback_ok = true;
		} else {
			struct wlr_texture *tex = wlr_texture_from_buffer(renderer, state.buffer);
			if (tex != nullptr) {
				const int w = state.buffer->width;
				const int h = state.buffer->height;
				rgba_scratch.resize((size_t)w * h * 4);
				struct wlr_texture_read_pixels_options opts = {
					.data = rgba_scratch.data(),
					.format = DRM_FORMAT_XRGB8888,
					.stride = (uint32_t)(w * 4),
				};
				if (wlr_texture_read_pixels(tex, &opts)) {
					// glReadPixels via GL_BGRA_EXT yields B,G,R,A in memory
					for (int y = 0; y < h; y++) {
						uint8_t *p = rgba_scratch.data() + (size_t)y * w * 4;
						for (int x = 0; x < w; x++) {
							std::swap(p[x * 4 + 0], p[x * 4 + 2]);
						}
					}
					readback_ok = true;
				}
				wlr_texture_destroy(tex);
			}
		}
		if (readback_ok && client) {
			clock_gettime(CLOCK_MONOTONIC, &t3);
			double t_readback = (t3.tv_sec - t2.tv_sec) * 1000.0 + (t3.tv_nsec - t2.tv_nsec) / 1e6;
			client->on_frame(rgba_scratch.data(), state.buffer->width,
				state.buffer->height, state.buffer->width * 4);
			struct timespec t4;
			clock_gettime(CLOCK_MONOTONIC, &t4);
			double t_client = (t4.tv_sec - t3.tv_sec) * 1000.0 + (t4.tv_nsec - t3.tv_nsec) / 1e6;
			double t_total = (t4.tv_sec - t0.tv_sec) * 1000.0 + (t4.tv_nsec - t0.tv_nsec) / 1e6;
			timing_gpu_ms += t_gpu;
			timing_readback_ms += t_readback;
			timing_client_ms += t_client;
			timing_total_ms += t_total;
			timing_frames++;
			if (timing_frames >= 60) {
				fprintf(stderr,
					"[perf] %.0f f/s  total=%.3f  gpu=%.3f readback=%.3f godot=%.3f\n",
					60.0 / (timing_total_ms / 1000.0),
					timing_total_ms / timing_frames,
					timing_gpu_ms / timing_frames,
					timing_readback_ms / timing_frames,
					timing_client_ms / timing_frames);
				timing_frames = 0;
				timing_gpu_ms = 0;
				timing_readback_ms = 0;
				timing_client_ms = 0;
				timing_total_ms = 0;
			}
		}
	}

	wlr_output_state_finish(&state);
	frame_debug.ok++;
}

void Compositor::copy_to_rgba(const uint8_t *data, uint32_t format,
		size_t stride, int width, int height) {
	rgba_scratch.resize((size_t)width * height * 4);

	// DRM formats are stored in little-endian memory as B,G,R[,A]. The XRGB /
	// XBGR byte order corresponds to the physical R,G,B pixel order (the first
	// byte in memory is the least significant / blue channel).
	if (format == DRM_FORMAT_XRGB8888) {
		for (int y = 0; y < height; y++) {
			const uint8_t *src = data + (size_t)y * stride;
			uint8_t *dst = rgba_scratch.data() + (size_t)y * width * 4;
			for (int x = 0; x < width; x++) {
				dst[x * 4 + 0] = src[x * 4 + 2];
				dst[x * 4 + 1] = src[x * 4 + 1];
				dst[x * 4 + 2] = src[x * 4 + 0];
				dst[x * 4 + 3] = 0xFF;
			}
		}
	} else if (format == DRM_FORMAT_XBGR8888) {
		for (int y = 0; y < height; y++) {
			const uint8_t *src = data + (size_t)y * stride;
			uint8_t *dst = rgba_scratch.data() + (size_t)y * width * 4;
			for (int x = 0; x < width; x++) {
				dst[x * 4 + 0] = src[x * 4 + 0];
				dst[x * 4 + 1] = src[x * 4 + 1];
				dst[x * 4 + 2] = src[x * 4 + 2];
				dst[x * 4 + 3] = 0xFF;
			}
		}
	} else if (format == DRM_FORMAT_ARGB8888) {
		for (int y = 0; y < height; y++) {
			const uint8_t *src = data + (size_t)y * stride;
			uint8_t *dst = rgba_scratch.data() + (size_t)y * width * 4;
			for (int x = 0; x < width; x++) {
				dst[x * 4 + 0] = src[x * 4 + 2];
				dst[x * 4 + 1] = src[x * 4 + 1];
				dst[x * 4 + 2] = src[x * 4 + 0];
				dst[x * 4 + 3] = src[x * 4 + 3];
			}
		}
	} else if (format == DRM_FORMAT_ABGR8888) {
		for (int y = 0; y < height; y++) {
			const uint8_t *src = data + (size_t)y * stride;
			uint8_t *dst = rgba_scratch.data() + (size_t)y * width * 4;
			for (int x = 0; x < width; x++) {
				dst[x * 4 + 0] = src[x * 4 + 0];
				dst[x * 4 + 1] = src[x * 4 + 1];
				dst[x * 4 + 2] = src[x * 4 + 2];
				dst[x * 4 + 3] = src[x * 4 + 3];
			}
		}
	} else {
		wlr_log(WLR_ERROR, "unsupported frame format 0x%X", format);
	}
}

// ---------------------------------------------------------------------------
// Input forwarding (Godot -> Wayland seat)
// ---------------------------------------------------------------------------

void Compositor::pointer_move_absolute(double x, double y) {
	if (!running()) {
		return;
	}
	wlr_cursor_warp_closest(cursor, nullptr, x, y);
	process_cursor_motion(now_msec());
}

void Compositor::pointer_button(uint32_t button, bool pressed) {
	if (!running()) {
		return;
	}
	uint32_t time = now_msec();
	wlr_seat_pointer_notify_button(seat, time, button,
		pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_notify_frame(seat);

	if (pressed) {
		double nlx, nly;
		struct wlr_scene_node *node = wlr_scene_node_at(&scene->tree.node,
			cursor->x, cursor->y, &nlx, &nly);
		Toplevel *hit = nullptr;
		for (struct wlr_scene_node *n = node; n != nullptr; n = &n->parent->node) {
			if (n->data != nullptr) {
				hit = (Toplevel *)n->data;
				break;
			}
		}
		focus_toplevel(this, hit);
		if (node != nullptr && hit != nullptr && hit->frame != nullptr) {
			if (node == &hit->close_btn->node) {
				wlr_xdg_toplevel_send_close(hit->xdg_toplevel);
			} else if (node == &hit->frame->node) {
				begin_interactive(hit, CURSOR_MOVE, 0);
			}
		}
	}
	if (!pressed) {
		reset_cursor_mode();
	}
}

void Compositor::pointer_axis_vertical(double delta, int32_t discrete) {
	if (!running()) {
		return;
	}
	struct wlr_pointer_axis_event event = {
		.pointer = pointer,
		.time_msec = now_msec(),
		.source = WL_POINTER_AXIS_SOURCE_WHEEL,
		.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL,
		.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
		.delta = delta,
		.delta_discrete = discrete,
	};
	wl_signal_emit_mutable(&pointer->events.axis, &event);
	wl_signal_emit_mutable(&pointer->events.frame, pointer);
}

void Compositor::pointer_axis_horizontal(double delta, int32_t discrete) {
	if (!running()) {
		return;
	}
	struct wlr_pointer_axis_event event = {
		.pointer = pointer,
		.time_msec = now_msec(),
		.source = WL_POINTER_AXIS_SOURCE_WHEEL,
		.orientation = WL_POINTER_AXIS_HORIZONTAL_SCROLL,
		.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
		.delta = delta,
		.delta_discrete = discrete,
	};
	wl_signal_emit_mutable(&pointer->events.axis, &event);
	wl_signal_emit_mutable(&pointer->events.frame, pointer);
}

void Compositor::key_event(int godot_key, bool pressed) {
	if (!running()) {
		return;
	}
	uint32_t keysym = godot_key_to_keysym(godot_key);
	if (keysym == 0) {
		return;
	}
	uint32_t keycode = keysym_to_keycode(keysym);
	{
		FILE *f = fopen("/tmp/opencode/input.log", "a");
		if (f) {
			fprintf(f, "key_event godot=%d keysym=%u keycode=%u focus=%p\n", godot_key,
				keysym, keycode, (void *)seat->keyboard_state.focused_surface);
			fclose(f);
		}
	}
	if (keycode == 0) {
		return;
	}
	struct wlr_keyboard_key_event event = {
		.time_msec = now_msec(),
		.keycode = keycode - 8,
		.update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(keyboard, &event);
}

// ---------------------------------------------------------------------------
// Keycode mapping helpers
// ---------------------------------------------------------------------------

uint32_t Compositor::godot_key_to_keysym(int key) {
	// Printable ASCII keys: Godot's Key value equals the unshifted US char for
	// punctuation/digits, and the UPPERCASE letter for A-Z (lowercase them).
	if (key >= 'A' && key <= 'Z') {
		return (uint32_t)(key + 32);
	}
	if ((key >= '0' && key <= '9') || (key >= ' ' && key <= '/') ||
			(key >= ':' && key <= '@') || (key >= '[' && key <= '`') ||
			(key >= '{' && key <= '~')) {
		return (uint32_t)key;
	}

	switch (key) {
	case 4194305: return XKB_KEY_Escape;      // KEY_ESCAPE
	case 4194306: return XKB_KEY_Tab;         // KEY_TAB
	case 4194308: return XKB_KEY_BackSpace;   // KEY_BACKSPACE
	case 4194309: return XKB_KEY_Return;      // KEY_ENTER
	case 4194310: return XKB_KEY_KP_Enter;    // KEY_KP_ENTER
	case 4194311: return XKB_KEY_Insert;      // KEY_INSERT
	case 4194312: return XKB_KEY_Delete;      // KEY_DELETE
	case 4194313: return XKB_KEY_Pause;       // KEY_PAUSE
	case 4194314: return XKB_KEY_Print;       // KEY_PRINT
	case 4194315: return XKB_KEY_Sys_Req;     // KEY_SYSREQ
	case 4194316: return XKB_KEY_Clear;       // KEY_CLEAR
	case 4194317: return XKB_KEY_Home;        // KEY_HOME
	case 4194318: return XKB_KEY_End;         // KEY_END
	case 4194319: return XKB_KEY_Left;        // KEY_LEFT
	case 4194320: return XKB_KEY_Up;          // KEY_UP
	case 4194321: return XKB_KEY_Right;       // KEY_RIGHT
	case 4194322: return XKB_KEY_Down;        // KEY_DOWN
	case 4194323: return XKB_KEY_Page_Up;     // KEY_PAGEUP
	case 4194324: return XKB_KEY_Page_Down;   // KEY_PAGEDOWN
	case 4194325: return XKB_KEY_Shift_L;     // KEY_SHIFT
	case 4194326: return XKB_KEY_Control_L;   // KEY_CTRL
	case 4194327: return XKB_KEY_Meta_L;      // KEY_META
	case 4194328: return XKB_KEY_Alt_L;       // KEY_ALT
	case 4194329: return XKB_KEY_Caps_Lock;   // KEY_CAPSLOCK
	case 4194330: return XKB_KEY_Num_Lock;    // KEY_NUMLOCK
	case 4194331: return XKB_KEY_Scroll_Lock; // KEY_SCROLLLOCK
	case 4194370: return XKB_KEY_Menu;        // KEY_MENU
	case 4194371: return XKB_KEY_Hyper_L;     // KEY_HYPER
	case 4194373: return XKB_KEY_Help;        // KEY_HELP
	case 4194433: return XKB_KEY_KP_Multiply; // KEY_KP_MULTIPLY
	case 4194434: return XKB_KEY_KP_Divide;   // KEY_KP_DIVIDE
	case 4194435: return XKB_KEY_KP_Subtract; // KEY_KP_SUBTRACT
	case 4194436: return XKB_KEY_KP_Decimal;  // KEY_KP_PERIOD
	case 4194437: return XKB_KEY_KP_Add;      // KEY_KP_ADD
	case 4194438: return XKB_KEY_KP_0;        // KEY_KP_0
	case 4194439: return XKB_KEY_KP_1;
	case 4194440: return XKB_KEY_KP_2;
	case 4194441: return XKB_KEY_KP_3;
	case 4194442: return XKB_KEY_KP_4;
	case 4194443: return XKB_KEY_KP_5;
	case 4194444: return XKB_KEY_KP_6;
	case 4194445: return XKB_KEY_KP_7;
	case 4194446: return XKB_KEY_KP_8;
	case 4194447: return XKB_KEY_KP_9;
	default: break;
	}

	if (key >= 4194332 && key <= 4194366) { // KEY_F1..KEY_F35
		return (uint32_t)(XKB_KEY_F1 + (key - 4194332));
	}

	return 0;
}

struct KeysymLookupCtx {
	uint32_t target;
	uint32_t result;
};

static void keysym_lookup_iter(struct xkb_keymap *keymap, xkb_keycode_t key,
		void *data) {
	KeysymLookupCtx *ctx = (KeysymLookupCtx *)data;
	if (ctx->result != 0) {
		return;
	}
	const xkb_keysym_t *syms = nullptr;
	if (xkb_keymap_key_get_syms_by_level(keymap, key, 0, 0, &syms) > 0) {
		for (int i = 0; syms[i] != XKB_KEY_NoSymbol; i++) {
			if (syms[i] == ctx->target) {
				ctx->result = key;
				return;
			}
		}
	}
}

uint32_t Compositor::keysym_to_keycode(uint32_t keysym) {
	if (keymap_for_lookup == nullptr) {
		return 0;
	}
	auto it = keysym_to_keycode_cache.find(keysym);
	if (it != keysym_to_keycode_cache.end()) {
		return it->second;
	}
	KeysymLookupCtx ctx = {.target = keysym, .result = 0};
	xkb_keymap_key_for_each(keymap_for_lookup, keysym_lookup_iter, &ctx);
	keysym_to_keycode_cache[keysym] = ctx.result;
	return ctx.result;
}

// ---------------------------------------------------------------------------
// Cursor motion / interactive move-resize (ported from tinywl)
// ---------------------------------------------------------------------------

void Compositor::reset_cursor_mode() {
	cursor_mode = CURSOR_PASSTHROUGH;
	grabbed_toplevel = nullptr;
}

Toplevel *Compositor::desktop_toplevel_at(double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&scene->tree.node, lx, ly, sx, sy);
	if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
		return nullptr;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (scene_surface == nullptr) {
		return nullptr;
	}
	*surface = scene_surface->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree != nullptr && tree->node.data == nullptr) {
		tree = tree->node.parent;
	}
	if (tree == nullptr) {
		return nullptr;
	}
	return (Toplevel *)tree->node.data;
}

void Compositor::process_cursor_move() {
	Toplevel *toplevel = grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->tree->node,
		cursor->x - grab_x, cursor->y - grab_y);
}

void Compositor::process_cursor_resize() {
	Toplevel *toplevel = grabbed_toplevel;
	double border_x = cursor->x - grab_x;
	double border_y = cursor->y - grab_y;
	int new_left = grab_geobox.x;
	int new_right = grab_geobox.x + grab_geobox.width;
	int new_top = grab_geobox.y;
	int new_bottom = grab_geobox.y + grab_geobox.height;

	if (resize_edges & WLR_EDGE_TOP) {
		new_top = (int)border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = (int)border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (resize_edges & WLR_EDGE_LEFT) {
		new_left = (int)border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (resize_edges & WLR_EDGE_RIGHT) {
		new_right = (int)border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	const int title_h = decorations_enabled ? Toplevel::TITLE_BAR_HEIGHT : 0;
	wlr_scene_node_set_position(&toplevel->tree->node,
		new_left - geo_box->x, new_top - geo_box->y - title_h);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

void Compositor::process_cursor_motion(uint32_t time) {
	if (cursor_mode == CURSOR_MOVE) {
		process_cursor_move();
		return;
	} else if (cursor_mode == CURSOR_RESIZE) {
		process_cursor_resize();
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = nullptr;
	Toplevel *toplevel = desktop_toplevel_at(cursor->x, cursor->y,
		&surface, &sx, &sy);
	if (toplevel == nullptr) {
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(seat);
	}
}

void Compositor::begin_interactive(Toplevel *toplevel, CursorMode mode,
		uint32_t edges) {
	grabbed_toplevel = toplevel;
	cursor_mode = mode;

	if (mode == CURSOR_MOVE) {
		grab_x = cursor->x - toplevel->tree->node.x;
		grab_y = cursor->y - toplevel->tree->node.y;
	} else {
		struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
		const int title_h = decorations_enabled ? Toplevel::TITLE_BAR_HEIGHT : 0;
		double border_x = (toplevel->tree->node.x + geo_box->x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
		double border_y = (toplevel->tree->node.y + geo_box->y + title_h) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
		grab_x = cursor->x - border_x;
		grab_y = cursor->y - border_y;
		grab_geobox = *geo_box;
		grab_geobox.x += (int)toplevel->tree->node.x + geo_box->x;
		grab_geobox.y += (int)toplevel->tree->node.y +
			geo_box->y + Toplevel::TITLE_BAR_HEIGHT;
		resize_edges = edges;
	}
}

// ---------------------------------------------------------------------------
// Process launch
// ---------------------------------------------------------------------------

bool Compositor::launch_process(const char *command) {
	if (!running() || socket_name == nullptr) {
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {
		unsetenv("DISPLAY");
		unsetenv("XAUTHORITY");
		unsetenv("WAYLAND_DISPLAY");
		setenv("WAYLAND_DISPLAY", socket_name, 1);
		if (xwayland != nullptr && xwayland->display_name != nullptr &&
				xwayland->display_name[0] != '\0') {
			// Let X11 apps (e.g. tint3) connect through rootless Xwayland.
			setenv("DISPLAY", xwayland->display_name, 1);
		}
		setenv("SDL_VIDEODRIVER", "wayland", 1);
		setenv("QT_QPA_PLATFORM", "wayland", 1);
		setenv("GDK_BACKEND", "wayland", 1);
		setenv("MOZ_ENABLE_WAYLAND", "1", 1);
		execl("/bin/sh", "/bin/sh", "-c", command, (void *)nullptr);
		_exit(127);
	}
	return pid >= 0;
}
