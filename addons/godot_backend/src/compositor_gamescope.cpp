// Gamescope-optimized backend selection for the godot_gamescope extension.
//
// The GameScope node runs an embedded Wayland compositor. When a Gamescope /
// Wayland session is already active we render nested into it using the wlr
// Wayland backend (gamescope's own surface becomes our output). Otherwise we
// run headless and the composited frames are read back and displayed inside
// the GameScope control, so apps can run inside the node in either case.

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

struct wlr_backend *create_gamescope_backend(struct wl_display *display) {
	struct wlr_backend *backend = nullptr;

	const char *session = getenv("GAMESCOPE_SESSION");
	const char *compositor = getenv("GAMESCOPE_COMPOSITOR");
	const char *wayland_display = getenv("WAYLAND_DISPLAY");

	if ((session != nullptr && strlen(session) > 0) ||
			(compositor != nullptr && strlen(compositor) > 0) ||
			(wayland_display != nullptr && strlen(wayland_display) > 0)) {
		wlr_log(WLR_INFO, "Gamescope/Wayland session detected, creating Wayland backend");
		backend = wlr_wl_backend_create(wl_display_get_event_loop(display), display);
		if (backend == nullptr) {
			wlr_log(WLR_ERROR, "failed to create Gamescope Wayland backend");
			return nullptr;
		}
		return backend;
	}

	wlr_log(WLR_INFO, "No Gamescope session detected, using headless backend");
	backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
	if (backend == nullptr) {
		wlr_log(WLR_ERROR, "failed to create headless backend");
		return nullptr;
	}
	return backend;
}
