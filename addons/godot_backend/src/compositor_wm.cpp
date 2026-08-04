// Backend selection for the godot_wm extension (mwm / TinyWM / weston hosts).
//
// When an external window manager is running we render nested into it using the
// wlr Wayland backend; otherwise we fall back to headless and the composited
// buffer is read back by the node.

#define WLR_USE_UNSTABLE

#include <wayland-server-core.h>

#include <cstdlib>
#include <cstring>

// wlroots headers are plain C headers without `extern "C"` guards, so they
// must be wrapped for C++ linking.
extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/backend/wayland.h>
#include <wlr/util/log.h>
}

struct wlr_backend *create_wm_backend(struct wl_display *display) {
	struct wlr_backend *backend = nullptr;

	const char *mwm_run = getenv("MWM_RUN");
	const char *mwm_wid = getenv("MWM_WINDOWID");
	const char *session_type = getenv("XDG_SESSION_TYPE");
	const char *wayland_display = getenv("WAYLAND_DISPLAY");

	if ((mwm_run != nullptr && strlen(mwm_run) > 0) ||
			(mwm_wid != nullptr && strlen(mwm_wid) > 0) ||
			(session_type != nullptr && strcmp(session_type, "wayland") == 0) ||
			(wayland_display != nullptr && strlen(wayland_display) > 0)) {
		wlr_log(WLR_INFO, "mwm/TinyWM/Wayland session detected, creating Wayland backend");
		backend = wlr_wl_backend_create(wl_display_get_event_loop(display), display);
		if (backend == nullptr) {
			wlr_log(WLR_ERROR, "failed to create mwm/TinyWM Wayland backend");
			return nullptr;
		}
		return backend;
	}

	wlr_log(WLR_INFO, "No mwm/TinyWM detected, using headless backend");
	backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
	if (backend == nullptr) {
		wlr_log(WLR_ERROR, "failed to create headless backend");
		return nullptr;
	}
	return backend;
}
