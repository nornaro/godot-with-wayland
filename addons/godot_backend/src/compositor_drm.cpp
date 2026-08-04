// Backend selection for the godot_drm extension (direct KMS/DRM).
//
// If a DRM render node is available we let wlr_backend_autocreate() set up the
// KMS backend; otherwise we fall back to headless and the composited buffer is
// read back by the node.

#define WLR_USE_UNSTABLE

#include <wayland-server-core.h>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

// wlroots headers are plain C headers without `extern "C"` guards, so they
// must be wrapped for C++ linking.
extern "C" {
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/util/log.h>
}

struct wlr_backend *create_drm_backend(struct wl_display *display) {
	int drm_fd = -1;
	for (int i = 128; i < 140 && drm_fd < 0; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
		if (drm_fd >= 0) {
			wlr_log(WLR_INFO, "DRM device %s available, using DRM backend", path);
			close(drm_fd);
			return wlr_backend_autocreate(wl_display_get_event_loop(display), nullptr);
		}
	}

	wlr_log(WLR_INFO, "No DRM device found, using headless backend");
	return wlr_headless_backend_create(wl_display_get_event_loop(display));
}
