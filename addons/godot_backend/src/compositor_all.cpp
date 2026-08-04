#pragma once

#define WLR_USE_UNSTABLE

#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

struct Compositor;

static struct wlr_backend *create_backend(struct wl_display *display);

// Original comprehensive backend selection - kept for reference
// This function is now split into specialized versions
struct wlr_backend *create_all_backends(struct wl_display *display) {
	struct wlr_backend *backend = nullptr;

	const char *wlr_backends = getenv("WLR_BACKENDS");
	if (wlr_backends != nullptr && strlen(wlr_backends) > 0) {
		wlr_log(WLR_INFO, "WLR_BACKENDS=%s specified", wlr_backends);
		wlr_log(WLR_INFO, "Creating backend from WLR_BACKENDS variable");
		return wlr_backend_autocreate(wl_display_get_event_loop(display), nullptr);
	}

	// Check for Gamescope (already running)
	if (getenv("GAMESCOPE_SESSION") || getenv("GAMESCOPE_COMPOSITOR")) {
		wlr_log(WLR_INFO, "Gamescope detected, creating Wayland backend");
		backend = wlr_wl_backend_create(wl_display_get_event_loop(display), display);
		if (backend == nullptr) {
			wlr_log(WLR_ERROR, "failed to create Gamescope Wayland backend");
			return nullptr;
		}
		return backend;
	}

	// Check for mwm (previously miwm, minimalist window manager)
	if (getenv("MWM_RUN") || getenv("MWM_WINDOWID")) {
		wlr_log(WLR_INFO, "mwm detected, creating Wayland backend");
		backend = wlr_wl_backend_create(wl_display_get_event_loop(display), display);
		if (backend == nullptr) {
			wlr_log(WLR_ERROR, "failed to create mwm Wayland backend");
			return nullptr;
		}
		return backend;
	}

	// Check for TinyWM
	if (getenv("TINYWM_SOCKET") || (getenv("XDG_SESSION_TYPE") && strcmp(getenv("XDG_SESSION_TYPE"), "wayland") == 0)) {
		wlr_log(WLR_INFO, "TinyWM/Wayland session detected, creating Wayland backend");
		backend = wlr_wl_backend_create(wl_display_get_event_loop(display), display);
		if (backend == nullptr) {
			wlr_log(WLR_ERROR, "failed to create TinyWM Wayland backend");
			return nullptr;
		}
		return backend;
	}

	// Check for weston (another Wayland compositor)
	if (getenv("WAYLAND_DISPLAY") && strlen(getenv("WAYLAND_DISPLAY")) > 0) {
		wlr_log(WLR_INFO, "weston/Wayland compositor detected, creating Wayland backend");
		backend = wlr_wl_backend_create(wl_display_get_event_loop(display), display);
		if (backend == nullptr) {
			wlr_log(WLR_ERROR, "failed to create weston Wayland backend");
			return nullptr;
		}
		return backend;
	}

	// Check for SDL (X11 with Wayland support)
	if (getenv("DISPLAY") && strlen(getenv("DISPLAY")) > 0) {
		wlr_log(WLR_INFO, "DISPLAY detected, using X11+libinput backend");
		// wlr_backend_autocreate will handle X11 when DISPLAY is set
		return wlr_backend_autocreate(wl_display_get_event_loop(display), nullptr);
	}

	// Check for KMS/DRM availability (gamescope embedded mode)
	int drm_fd = -1;
	for (int i = 128; i < 140 && drm_fd < 0; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
		if (drm_fd >= 0) {
			wlr_log(WLR_INFO, "DRM device %s available, using DRM backend", path);
			return wlr_backend_autocreate(wl_display_get_event_loop(display), nullptr);
		}
	}
	if (drm_fd >= 0) {
		close(drm_fd);
	}

	// Default to headless for testing and development
	wlr_log(WLR_INFO, "No supported backend detected, using headless backend");
	backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
	if (backend == nullptr) {
		wlr_log(WLR_ERROR, "failed to create headless backend");
		return nullptr;
	}
	return backend;
}
