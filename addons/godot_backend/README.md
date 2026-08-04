# Godot Backend Extensions

This addon provides multiple backend extensions for running Godot without requiring external window managers:

## Extensions Available

### 1. godot_wayland (Primary)
- Supports Gamescope, mwm, TinyWM, weston, X11, and KMS/DRM
- Comprehensive backend selection based on environment variables
- Requires wlroots-0.19 library

### 2. godot_gamescope (Gamescope-specific)
- Optimized for Gamescope environment
- Simplified backend selection
- Faster initialization for gaming scenarios

### 3. godot_wm (Window Manager)
- For mwm (minimal window manager) and TinyWM
- Basic Wayland backend without Gamescope features

### 4. godot_drm (KMS/DRM)
- Direct KMS/DRM backend for embedded systems
- Bypasses Wayland compositor entirely
- Works on consoles and embedded devices

## Usage

```bash
# Gamescope environment
export GAMESCOPE_SESSION=1
./godot --extension addons/godot_backend/godot_wayland

# mwm/TinyWM environment  
export MWM_RUN=1  # or export TINYWM_SOCKET=/path/to/socket
./godot --extension addons/godot_backend/godot_wm

# KMS/DRM environment
# No window manager needed, direct DRM
./godot --extension addons/godot_backend/godot_drm
```

## Backend Selection Priority

1. GAMESCOPE_SESSION or GAMESCOPE_COMPOSITOR → Gamescope backend
2. MWM_RUN or MWM_WINDOWID → mwm backend  
3. TINYWM_SOCKET or XDG_SESSION_TYPE=wayland → TinyWM backend
4. WAYLAND_DISPLAY → weston backend
5. DISPLAY → X11 backend
6. /dev/dri/renderD* → KMS/DRM backend
7. Headless → fallback

## Requirements

- Linux kernel with DRM/KMS support
- wlroots-0.19 development headers
- CMake 3.10+ for building
- C++17 compiler

## Building

```bash
# Build all extensions
cd addons/godot_backend
mkdir build && cd build
cmake ..
make -j$(nproc)

# Copy built extensions to Godot project
cp bin/*.so ../godot_with_wayland/gdextension/
cp *.gdextension ../godot_with_wayland/
```
