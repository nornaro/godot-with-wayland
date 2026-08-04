#include "x11_wm.hpp"

#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xdamage.h>
#include <X11/keysym.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
X11WM *g_singleton = nullptr;

int x11_error_handler(Display *dpy, XErrorEvent *err) {
	X11WM *wm = g_singleton;
	if (wm != nullptr && dpy == wm->native_display()) {
		return 0;
	}
	if (wm != nullptr && wm->previous_error_handler() != nullptr) {
		return wm->previous_error_handler()(dpy, err);
	}
	return 0;
}
} // namespace

X11WM::X11WM() = default;

X11WM::~X11WM() {
	shutdown();
}

bool X11WM::init(int width, int height, ServerMode server_mode, const std::string &display_name) {
	if (display_ != nullptr) {
		return false;
	}
	if (width <= 0 || height <= 0) {
		return false;
	}
	width_ = width;
	height_ = height;
	bad_access_ = false;
	prev_error_handler_ = XSetErrorHandler(x11_error_handler);
	g_singleton = this;

	if (!setup_server(width, height, server_mode, display_name)) {
		XSetErrorHandler(prev_error_handler_);
		prev_error_handler_ = nullptr;
		g_singleton = nullptr;
		return false;
	}

	display_ = open_display_retry(display_str_, spawned_ ? 60 : 1);
	if (display_ == nullptr) {
		fprintf(stderr, "x11wm: could not open display %s\n", display_str_.c_str());
		teardown_server();
		XSetErrorHandler(prev_error_handler_);
		prev_error_handler_ = nullptr;
		g_singleton = nullptr;
		return false;
	}
	screen_ = DefaultScreen(display_);
	root_ = DefaultRootWindow(display_);

	if (!select_events()) {
		fprintf(stderr, "x11wm: another window manager is running on %s\n", display_str_.c_str());
		XCloseDisplay(display_);
		display_ = nullptr;
		teardown_server();
		XSetErrorHandler(prev_error_handler_);
		prev_error_handler_ = nullptr;
		g_singleton = nullptr;
		return false;
	}

	setup_ewmh();
	setup_capture();
	setup_input_grabs();

	XSync(display_, False);
	damage_pending_ = true;
	last_capture_ms_ = 0;
	last_clip_poll_ms_ = 0;
	last_ffp_ms_ = 0;
	return true;
}

void X11WM::shutdown() {
	if (display_ == nullptr) {
		return;
	}
	if (damage_ != 0) {
		XDamageDestroy(display_, damage_);
		damage_ = 0;
	}
	if (shm_image_ != nullptr) {
		if (shm_attached_) {
			XShmDetach(display_, &shm_info_);
			shm_attached_ = false;
		}
		XDestroyImage(shm_image_);
		shm_image_ = nullptr;
	}
	XCloseDisplay(display_);
	display_ = nullptr;
	teardown_server();
	if (prev_error_handler_ != nullptr) {
		XSetErrorHandler(prev_error_handler_);
		prev_error_handler_ = nullptr;
	}
	g_singleton = nullptr;
	clients_.clear();
	focused_ = 0;
	owner_window_ = 0;
	interact_mode_ = InteractMode::Idle;
	interact_window_ = 0;
	clipboard_owner_ = None;
}

namespace {

pid_t read_lock_pid(const std::string &path) {
	int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	char buf[32] = {0};
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		return -1;
	}
	return (pid_t)strtol(buf, nullptr, 10);
}

bool process_alive(pid_t pid) {
	if (pid <= 0) {
		return false;
	}
	return kill(pid, 0) == 0 || errno == EPERM;
}

bool is_x_server_process(pid_t pid) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return false;
	}
	char buf[64] = {0};
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		return false;
	}
	return strncmp(buf, "X", 1) == 0; // Xvfb, Xorg, Xephyr, Xwayland...
}

// Find a live X server whose command line targets `display` (e.g. ":100").
// Unlike the lock file (which a previous crashed run may have unlinked while
// the server kept running and still owns the abstract socket), /proc/cmdline
// always reflects reality.
pid_t find_x_server_pid(const std::string &display) {
	DIR *dir = opendir("/proc");
	if (dir == nullptr) {
		return -1;
	}
	pid_t found = -1;
	struct dirent *ent;
	while ((ent = readdir(dir)) != nullptr) {
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
			continue;
		}
		pid_t pid = (pid_t)strtol(ent->d_name, nullptr, 10);
		if (!is_x_server_process(pid)) {
			continue;
		}
		char path[64];
		snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
		int fd = open(path, O_RDONLY);
		if (fd < 0) {
			continue;
		}
		char buf[1024] = {0};
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0) {
			continue;
		}
		// cmdline args are NUL-separated; match the display token exactly.
		size_t pos = 0;
		while (pos < (size_t)n) {
			std::string token(buf + pos);
			if (token == display) {
				found = pid;
				break;
			}
			pos += token.size() + 1;
		}
		if (found > 0) {
			break;
		}
	}
	closedir(dir);
	return found;
}

} // namespace

bool X11WM::setup_server(int width, int height, ServerMode server_mode, const std::string &display_name) {
	if (server_mode == ServerMode::Attach) {
		if (display_name.empty()) {
			fprintf(stderr, "x11wm: attach mode requires a display name\n");
			return false;
		}
		display_str_ = display_name;
		return true;
	}

	std::vector<std::string> slots = { ":1" };
	if (!display_name.empty()) {
		display_str_ = display_name;
	} else {
		std::vector<std::string> free_slots;
		for (int n = 1; n < 48 && free_slots.empty(); n++) {
			char sock[64];
			char lock[64];
			snprintf(sock, sizeof(sock), "/tmp/.X11-unix/X%d", n);
			snprintf(lock, sizeof(lock), "/tmp/.X%d-lock", n);
			if (access(sock, F_OK) == 0 || access(lock, F_OK) == 0) {
				continue;
			}
			free_slots.push_back(":" + std::to_string(n));
		}
		if (free_slots.empty()) {
			fprintf(stderr, "x11wm: no free display slot\n");
			return false;
		}
		display_str_ = free_slots[0];
	}
	(void)slots;

	std::string server;
	std::vector<std::string> args;
	if (server_mode == ServerMode::Xvfb) {
		server = "Xvfb";
		args = { display_str_, "-screen", "0", std::to_string(width) + "x" + std::to_string(height) + "x24",
				"-nolisten", "tcp" };
	} else {
		server = "Xephyr";
		args = { display_str_, "-screen", std::to_string(width) + "x" + std::to_string(height),
				"-ac", "-nolisten", "tcp" };
	}

	// Clear stale lock/socket files from a previously crashed or SIGKILLed
	// server on this display, otherwise Xvfb refuses to start
	// ("Server is already active for display N").
	std::string sock_path = "/tmp/.X11-unix/X" + std::string(display_str_, 1);
	std::string lock_path = "/tmp/.X" + std::string(display_str_, 1) + "-lock";

	// A killed game leaves its Xvfb running, and simply unlinking the
	// socket/lock files does not stop it - the fresh spawn then dies with
	// "Cannot establish any listening sockets" because the stale server still
	// owns the abstract socket. If a live X server targets this display,
	// terminate it so we can start a clean display.
	pid_t stale_pid = -1;
	pid_t lock_pid = read_lock_pid(lock_path);
	if (lock_pid > 0 && is_x_server_process(lock_pid) && process_alive(lock_pid)) {
		stale_pid = lock_pid;
	} else {
		// Lock file may be missing while the server still runs (unlinked by a
		// previous crashed run) - scan /proc/cmdline instead.
		stale_pid = find_x_server_pid(display_str_);
	}
	if (stale_pid > 0) {
		fprintf(stderr, "x11wm: stopping stale X server pid %d on %s\n", (int)stale_pid, display_str_.c_str());
		kill(stale_pid, SIGTERM);
		for (int i = 0; i < 100 && process_alive(stale_pid); i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		if (process_alive(stale_pid)) {
			kill(stale_pid, SIGKILL);
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
	unlink(sock_path.c_str());
	unlink(lock_path.c_str());

	server_pid_ = spawn_process(server, args);
	if (server_pid_ < 0) {
		fprintf(stderr, "x11wm: failed to spawn %s\n", server.c_str());
		return false;
	}
	spawned_ = true;

	std::string sock = "/tmp/.X11-unix/X" + std::string(display_str_, 1);
	if (!wait_for_socket(sock, 6000)) {
		fprintf(stderr, "x11wm: server %s did not create %s\n", server.c_str(), sock.c_str());
		teardown_server();
		return false;
	}
	return true;
}

void X11WM::teardown_server() {
	if (spawned_ && server_pid_ > 0) {
		kill(server_pid_, SIGTERM);
		int status = 0;
		waitpid(server_pid_, &status, 0);
	}
	spawned_ = false;
	server_pid_ = -1;
}

Display *X11WM::open_display_retry(const std::string &name, int attempts) {
	for (int i = 0; i < attempts; i++) {
		Display *d = XOpenDisplay(name.empty() ? nullptr : name.c_str());
		if (d != nullptr) {
			return d;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(120));
	}
	return nullptr;
}

bool X11WM::select_events() {
	XSelectInput(display_, root_, SubstructureRedirectMask | SubstructureNotifyMask | ExposureMask);
	XSync(display_, False);
	return !bad_access_;
}

void X11WM::setup_ewmh() {
	wm_protocols_ = XInternAtom(display_, "WM_PROTOCOLS", False);
	wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
	net_wm_name_ = XInternAtom(display_, "_NET_WM_NAME", False);
	utf8_string_ = XInternAtom(display_, "UTF8_STRING", False);
	net_supported_ = XInternAtom(display_, "_NET_SUPPORTED", False);
	net_supporting_wm_check_ = XInternAtom(display_, "_NET_SUPPORTING_WM_CHECK", False);
	net_active_window_ = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
	net_client_list_ = XInternAtom(display_, "_NET_CLIENT_LIST", False);
	targets_ = XInternAtom(display_, "TARGETS", False);
	clipboard_ = XInternAtom(display_, "CLIPBOARD", False);
	primary_ = XA_PRIMARY;
	string_atom_ = XA_STRING;
	text_atom_ = XInternAtom(display_, "TEXT", False);
	compound_text_atom_ = XInternAtom(display_, "COMPOUND_TEXT", False);

	owner_window_ = XCreateSimpleWindow(display_, root_, -1, -1, 1, 1, 0, 0, 0);

	Atom wm_check_atom = net_supporting_wm_check_;
	XChangeProperty(display_, root_, net_supporting_wm_check_, XA_WINDOW, 32, PropModeReplace,
			(unsigned char *)&owner_window_, 1);
	XChangeProperty(display_, owner_window_, net_supporting_wm_check_, XA_WINDOW, 32, PropModeReplace,
			(unsigned char *)&owner_window_, 1);
	const char wm_name[] = "x11wm";
	XChangeProperty(display_, owner_window_, net_wm_name_, utf8_string_, 8, PropModeReplace,
			(unsigned char *)wm_name, sizeof(wm_name) - 1);

	Atom supported[] = { net_active_window_, net_client_list_ };
	XChangeProperty(display_, root_, net_supported_, XA_ATOM, 32, PropModeReplace,
			(unsigned char *)supported, 2);
	(void)wm_check_atom;
}

void X11WM::setup_capture() {
	damage_available_ = XDamageQueryExtension(display_, &damage_event_base_, &damage_error_base_);
	if (damage_available_) {
		damage_ = XDamageCreate(display_, root_, XDamageReportNonEmpty);
	}

	if (XShmQueryExtension(display_)) {
		shm_image_ = XShmCreateImage(display_, DefaultVisual(display_, screen_), DefaultDepth(display_, screen_),
				ZPixmap, nullptr, &shm_info_, width_, height_);
		if (shm_image_ != nullptr) {
			shm_info_.shmid = shmget(IPC_PRIVATE, (size_t)shm_image_->bytes_per_line * height_, IPC_CREAT | 0600);
			if (shm_info_.shmid >= 0) {
				shm_info_.shmaddr = shm_image_->data = (char *)shmat(shm_info_.shmid, nullptr, 0);
				if (shm_info_.shmaddr != (char *)-1) {
					shm_info_.readOnly = False;
					XShmAttach(display_, &shm_info_);
					shm_attached_ = true;
					shmctl(shm_info_.shmid, IPC_RMID, nullptr);
				} else {
					XDestroyImage(shm_image_);
					shm_image_ = nullptr;
				}
			} else {
				XDestroyImage(shm_image_);
				shm_image_ = nullptr;
			}
		}
	}

	rgba_buf_.resize((size_t)width_ * height_ * 4);
}

void X11WM::setup_input_grabs() {
	XGrabButton(display_, AnyButton, AnyModifier, root_, False,
			ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeSync, GrabModeAsync, None, None);
	KeyCode f4 = XKeysymToKeycode(display_, XK_F4);
	KeyCode tab = XKeysymToKeycode(display_, XK_Tab);
	XGrabKey(display_, f4, Mod1Mask, root_, False, GrabModeAsync, GrabModeAsync);
	XGrabKey(display_, tab, Mod1Mask, root_, False, GrabModeAsync, GrabModeAsync);
}

void X11WM::pump() {
	if (display_ == nullptr) {
		return;
	}
	while (XPending(display_) > 0) {
		XEvent ev;
		XNextEvent(display_, &ev);
		handle_event(ev);
	}
	maybe_capture();
	maybe_poll_clipboard();
	maybe_focus_follows();
}

void X11WM::handle_event(XEvent &ev) {
	if (damage_available_ && ev.type == damage_event_base_) {
		damage_pending_ = true;
		return;
	}
	switch (ev.type) {
	case MapRequest:
		on_map_request(&ev.xmaprequest);
		break;
	case ConfigureRequest:
		on_configure_request(&ev.xconfigurerequest);
		break;
	case MapNotify:
		on_map_notify(&ev.xmap);
		break;
	case UnmapNotify:
		on_unmap_notify(&ev.xunmap);
		break;
	case DestroyNotify:
		on_destroy_notify(&ev.xdestroywindow);
		break;
	case ButtonPress:
		on_button(&ev.xbutton);
		break;
	case ButtonRelease:
		on_button_release(&ev.xbutton);
		break;
	case MotionNotify:
		on_motion(&ev.xmotion);
		break;
	case KeyPress:
		on_key(&ev.xkey);
		break;
	case SelectionRequest:
		on_selection_request(&ev.xselectionrequest);
		break;
	case SelectionClear:
		clipboard_owner_ = None;
		break;
	case Expose:
	case GraphicsExpose:
	case PropertyNotify:
		damage_pending_ = true;
		break;
	default:
		break;
	}
}

void X11WM::on_map_request(XMapRequestEvent *ev) {
	XWindowAttributes attrs;
	if (XGetWindowAttributes(display_, ev->window, &attrs) && attrs.override_redirect) {
		XMapWindow(display_, ev->window);
		damage_pending_ = true;
		return;
	}
	manage_window(ev->window);
	XMapWindow(display_, ev->window);
	XRaiseWindow(display_, ev->window);
	redraw_borders();
	damage_pending_ = true;
}

void X11WM::on_configure_request(XConfigureRequestEvent *ev) {
	XWindowChanges wc;
	wc.x = ev->x;
	wc.y = ev->y;
	wc.width = ev->width;
	wc.height = ev->height;
	wc.border_width = ev->border_width;
	wc.sibling = ev->above;
	wc.stack_mode = ev->detail;
	unsigned int mask = ev->value_mask;
	if (wm_mode_ != WmMode::TinyWm) {
		wc.border_width = border_width_;
		mask |= CWBorderWidth;
	}
	if (wc.width > width_) {
		wc.width = width_;
	}
	if (wc.height > height_) {
		wc.height = height_;
	}
	XConfigureWindow(display_, ev->window, mask, &wc);
	damage_pending_ = true;
}

void X11WM::on_map_notify(XMapEvent *ev) {
	if (ev->window == owner_window_) {
		return;
	}
	XWindowAttributes attrs;
	if (XGetWindowAttributes(display_, ev->window, &attrs) && attrs.override_redirect) {
		return;
	}
	manage_window(ev->window);
	redraw_borders();
	damage_pending_ = true;
}

void X11WM::on_unmap_notify(XUnmapEvent *ev) {
	if (ev->window == owner_window_) {
		return;
	}
	remove_client(ev->window);
	if (focused_ == ev->window) {
		focused_ = 0;
		refocus();
	}
	redraw_borders();
	damage_pending_ = true;
}

void X11WM::on_destroy_notify(XDestroyWindowEvent *ev) {
	remove_client(ev->window);
	if (focused_ == ev->window) {
		focused_ = 0;
		refocus();
	}
	redraw_borders();
	damage_pending_ = true;
}

void X11WM::on_button(XButtonEvent *ev) {
	Window target = toplevel_for(ev->subwindow);
	bool alt = (ev->state & Mod1Mask) != 0;

	if (alt && target != None) {
		if (ev->button == Button1) {
			interact_mode_ = InteractMode::Move;
			interact_window_ = target;
			press_x_ = ev->x_root;
			press_y_ = ev->y_root;
			XWindowAttributes attrs;
			if (XGetWindowAttributes(display_, target, &attrs)) {
				press_dx_ = ev->x_root - attrs.x;
				press_dy_ = ev->y_root - attrs.y;
			} else {
				press_dx_ = 0;
				press_dy_ = 0;
			}
			XAllowEvents(display_, AsyncPointer, CurrentTime);
			return;
		}
		if (ev->button == Button3) {
			interact_mode_ = InteractMode::Resize;
			interact_window_ = target;
			press_x_ = ev->x_root;
			press_y_ = ev->y_root;
			XWindowAttributes attrs;
			if (XGetWindowAttributes(display_, target, &attrs)) {
				start_w_ = attrs.width;
				start_h_ = attrs.height;
			} else {
				start_w_ = 200;
				start_h_ = 200;
			}
			XAllowEvents(display_, AsyncPointer, CurrentTime);
			return;
		}
	}

	if (ev->button == Button1 || ev->button == Button2 || ev->button == Button3) {
		if (target != None && target != root_) {
			set_focus(target);
			XRaiseWindow(display_, target);
			redraw_borders();
		}
	}
	XAllowEvents(display_, ReplayPointer, CurrentTime);
}

void X11WM::on_button_release(XButtonEvent *ev) {
	if (interact_mode_ != InteractMode::Idle) {
		interact_mode_ = InteractMode::Idle;
		interact_window_ = None;
	}
	(void)ev;
}

void X11WM::on_motion(XMotionEvent *ev) {
	if (interact_window_ == None) {
		return;
	}
	if (interact_mode_ == InteractMode::Move) {
		XMoveWindow(display_, interact_window_, ev->x_root - press_dx_, ev->y_root - press_dy_);
		damage_pending_ = true;
	} else if (interact_mode_ == InteractMode::Resize) {
		int w = start_w_ + (ev->x_root - press_x_);
		int h = start_h_ + (ev->y_root - press_y_);
		if (w < 32) {
			w = 32;
		}
		if (h < 32) {
			h = 32;
		}
		XResizeWindow(display_, interact_window_, (unsigned int)w, (unsigned int)h);
		damage_pending_ = true;
	}
}

void X11WM::on_key(XKeyEvent *ev) {
	KeySym ks = XLookupKeysym(ev, 0);
	if (ks == XK_F4 && (ev->state & Mod1Mask) != 0) {
		close_window(focused_);
		return;
	}
	if (ks == XK_Tab && (ev->state & Mod1Mask) != 0 && wm_mode_ == WmMode::Mwm) {
		focus_next();
		return;
	}
}

void X11WM::on_selection_request(XSelectionRequestEvent *ev) {
	XSelectionEvent reply;
	reply.type = SelectionNotify;
	reply.display = display_;
	reply.requestor = ev->requestor;
	reply.selection = ev->selection;
	reply.target = ev->target;
	reply.time = ev->time;
	reply.property = None;

	Atom prop = ev->property;
	if (prop == None) {
		prop = ev->target;
	}

	if (ev->target == targets_) {
		Atom targets[] = { targets_, utf8_string_, string_atom_, text_atom_, compound_text_atom_ };
		XChangeProperty(display_, ev->requestor, prop, XA_ATOM, 32, PropModeReplace,
				(unsigned char *)targets, 5);
		reply.property = prop;
	} else if (ev->target == utf8_string_) {
		XChangeProperty(display_, ev->requestor, prop, utf8_string_, 8, PropModeReplace,
				(unsigned char *)clipboard_buf_.data(), clipboard_buf_.size());
		reply.property = prop;
	} else if (ev->target == string_atom_ || ev->target == text_atom_ || ev->target == compound_text_atom_) {
		XChangeProperty(display_, ev->requestor, prop, string_atom_, 8, PropModeReplace,
				(unsigned char *)clipboard_buf_.data(), clipboard_buf_.size());
		reply.property = prop;
	}

	XSendEvent(display_, ev->requestor, False, NoEventMask, (XEvent *)&reply);
	XFlush(display_);
}

void X11WM::manage_window(Window w) {
	if (in_clients(w)) {
		return;
	}
	clients_.push_back(w);
	set_border(w);
	set_focus(w);
	update_client_list();
}

void X11WM::remove_client(Window w) {
	auto it = std::find(clients_.begin(), clients_.end(), w);
	if (it != clients_.end()) {
		clients_.erase(it);
		update_client_list();
	}
	if (interact_window_ == w) {
		interact_window_ = None;
		interact_mode_ = InteractMode::Idle;
	}
}

bool X11WM::in_clients(Window w) const {
	return std::find(clients_.begin(), clients_.end(), w) != clients_.end();
}

Window X11WM::toplevel_for(Window w) const {
	while (w != None && w != root_) {
		if (in_clients(w)) {
			return w;
		}
		Window root_return = None;
		Window parent = None;
		Window *children = nullptr;
		unsigned int n = 0;
		if (!XQueryTree(display_, w, &root_return, &parent, &children, &n)) {
			break;
		}
		if (children != nullptr) {
			XFree(children);
		}
		w = parent;
	}
	return None;
}

void X11WM::set_focus(Window w) {
	focused_ = w;
	XSetInputFocus(display_, w, RevertToParent, CurrentTime);
	XChangeProperty(display_, root_, net_active_window_, XA_WINDOW, 32, PropModeReplace,
			(unsigned char *)&w, 1);
	redraw_borders();
}

void X11WM::refocus() {
	if (clients_.empty()) {
		set_focus(root_);
		return;
	}
	Window w = clients_.back();
	XRaiseWindow(display_, w);
	set_focus(w);
}

void X11WM::focus_next() {
	if (clients_.empty()) {
		set_focus(root_);
		return;
	}
	size_t idx = 0;
	for (size_t i = 0; i < clients_.size(); i++) {
		if (clients_[i] == focused_) {
			idx = i;
			break;
		}
	}
	size_t next = (idx + 1) % clients_.size();
	Window w = clients_[next];
	XRaiseWindow(display_, w);
	set_focus(w);
}

void X11WM::close_window(Window w) {
	if (w == None || w == root_) {
		return;
	}
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0;
	unsigned long extra = 0;
	Atom *props = nullptr;
	if (XGetWindowProperty(display_, w, wm_protocols_, 0, 32, False, XA_ATOM, &type, &fmt, &n, &extra,
				(unsigned char **)&props) == Success) {
		for (unsigned long i = 0; props != nullptr && i < n; i++) {
			if (props[i] == wm_delete_window_) {
				XEvent ev;
				memset(&ev, 0, sizeof(ev));
				ev.xclient.type = ClientMessage;
				ev.xclient.window = w;
				ev.xclient.message_type = wm_protocols_;
				ev.xclient.format = 32;
				ev.xclient.data.l[0] = (long)wm_delete_window_;
				ev.xclient.data.l[1] = CurrentTime;
				XSendEvent(display_, w, False, NoEventMask, &ev);
				XFlush(display_);
				if (props != nullptr) {
					XFree(props);
				}
				return;
			}
		}
	}
	if (props != nullptr) {
		XFree(props);
	}
	XKillClient(display_, w);
	XFlush(display_);
}

void X11WM::set_border(Window w) {
	if (wm_mode_ == WmMode::TinyWm) {
		XSetWindowBorderWidth(display_, w, 0);
		return;
	}
	XSetWindowBorderWidth(display_, w, (unsigned int)border_width_);
	XSetWindowBorder(display_, w, focused_ == w ? focus_border_pixel_ : border_pixel_);
}

void X11WM::redraw_borders() {
	if (wm_mode_ == WmMode::TinyWm) {
		return;
	}
	for (Window w : clients_) {
		set_border(w);
	}
}

void X11WM::update_client_list() {
	std::vector<Window> list(clients_.rbegin(), clients_.rend());
	XChangeProperty(display_, root_, net_client_list_, XA_WINDOW, 32, PropModeReplace,
			(unsigned char *)list.data(), list.size());
}

void X11WM::maybe_capture() {
	bool want = damage_pending_;
	const uint64_t now = now_ms();
	if (!damage_available_) {
		want = now - last_capture_ms_ >= 33;
	} else if (now - last_capture_ms_ >= 250) {
		// Damage on the root does not fire when a child window redraws
		// (without a compositor the root is never repainted), so poll as a
		// safety net to keep the shared surface fresh.
		want = true;
	}
	if (!want) {
		return;
	}
	capture();
	damage_pending_ = false;
	last_capture_ms_ = now;
}

void X11WM::capture() {
	XImage *img = nullptr;
	if (shm_image_ != nullptr && XShmGetImage(display_, root_, shm_image_, 0, 0, AllPlanes)) {
		img = shm_image_;
	}
	XImage *allocated = nullptr;
	if (img == nullptr) {
		allocated = XGetImage(display_, root_, 0, 0, width_, height_, AllPlanes, ZPixmap);
		if (allocated == nullptr) {
			if (client != nullptr) {
				client->on_log("x11wm: XGetImage failed");
			}
			return;
		}
		img = allocated;
	}
	convert_image_to_rgba(img, rgba_buf_);
	if (allocated != nullptr) {
		XDestroyImage(allocated);
	}
	if (client != nullptr) {
		client->on_frame(rgba_buf_.data(), width_, height_, width_ * 4);
	}
}

void X11WM::convert_image_to_rgba(const XImage *img, std::vector<uint8_t> &out) {
	const int w = img->width;
	const int h = img->height;
	if ((size_t)w * h * 4 > out.size()) {
		out.resize((size_t)w * h * 4);
	}
	int r_shift = 0;
	int g_shift = 0;
	int b_shift = 0;
	unsigned long m = img->red_mask;
	while (m != 0 && (m & 1) == 0) {
		m >>= 1;
		r_shift++;
	}
	m = img->green_mask;
	while (m != 0 && (m & 1) == 0) {
		m >>= 1;
		g_shift++;
	}
	m = img->blue_mask;
	while (m != 0 && (m & 1) == 0) {
		m >>= 1;
		b_shift++;
	}

	uint8_t *dst = out.data();
	if (img->bits_per_pixel == 32 && img->format == ZPixmap && img->byte_order == LSBFirst) {
		const uint32_t *pix = (const uint32_t *)img->data;
		const int row_pix = img->bytes_per_line / 4;
		for (int y = 0; y < h; y++) {
			const uint32_t *row = pix + y * row_pix;
			for (int x = 0; x < w; x++) {
			uint32_t p = row[x];
			// 32bpp TrueColor GetImage data is XRGB with the lowest byte in
			// memory holding blue on little-endian (byte0=B, byte1=G,
			// byte2=R, byte3 unused), regardless of the visual mask values
			// some servers report. Convert to RGBA for Godot.
			dst[0] = (uint8_t)((p >> 16) & 0xff);
			dst[1] = (uint8_t)((p >> 8) & 0xff);
			dst[2] = (uint8_t)(p & 0xff);
			dst[3] = 255;
				dst += 4;
			}
		}
		return;
	}

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			unsigned long p = XGetPixel((XImage *)img, x, y);
			dst[0] = (uint8_t)((p & img->red_mask) >> r_shift);
			dst[1] = (uint8_t)((p & img->green_mask) >> g_shift);
			dst[2] = (uint8_t)((p & img->blue_mask) >> b_shift);
			dst[3] = 255;
			dst += 4;
		}
	}
}

void X11WM::maybe_poll_clipboard() {
	if (now_ms() - last_clip_poll_ms_ < 400) {
		return;
	}
	last_clip_poll_ms_ = now_ms();
	Window owner = XGetSelectionOwner(display_, clipboard_);
	if (owner == None || owner == owner_window_ || owner == clipboard_owner_) {
		return;
	}
	std::string text = pull_clipboard();
	if (!text.empty() && client != nullptr) {
		client->on_clipboard_text(text);
	}
	clipboard_owner_ = owner;
}

void X11WM::maybe_focus_follows() {
	if (wm_mode_ != WmMode::Mwm) {
		return;
	}
	if (now_ms() - last_ffp_ms_ < 120) {
		return;
	}
	last_ffp_ms_ = now_ms();
	Window root_return = None;
	Window child = None;
	int rx = 0;
	int ry = 0;
	int wx = 0;
	int wy = 0;
	unsigned int mask = 0;
	if (!XQueryPointer(display_, root_, &root_return, &child, &rx, &ry, &wx, &wy, &mask)) {
		return;
	}
	Window t = toplevel_for(child);
	if (t != None && t != focused_) {
		set_focus(t);
	}
}

void X11WM::set_wm_mode(WmMode mode) {
	wm_mode_ = mode;
	border_width_ = (mode == WmMode::TinyWm) ? 0 : 2;
}

void X11WM::fake_key(uint32_t keysym, bool pressed) {
	if (display_ == nullptr) {
		return;
	}
	KeyCode kc = XKeysymToKeycode(display_, (KeySym)keysym);
	if (kc == 0) {
		return;
	}
	XTestFakeKeyEvent(display_, kc, pressed ? True : False, CurrentTime);
	XFlush(display_);
}

void X11WM::fake_button(unsigned int button, bool pressed) {
	if (display_ == nullptr) {
		return;
	}
	XTestFakeButtonEvent(display_, button, pressed ? True : False, CurrentTime);
	XFlush(display_);
}

void X11WM::fake_motion(int x, int y) {
	if (display_ == nullptr) {
		return;
	}
	XTestFakeMotionEvent(display_, screen_, x, y, CurrentTime);
	XFlush(display_);
}

void X11WM::pointer_axis_vertical(double delta) {
	if (delta > 0.0) {
		fake_button(4, true);
		fake_button(4, false);
	} else if (delta < 0.0) {
		fake_button(5, true);
		fake_button(5, false);
	}
}

void X11WM::pointer_axis_horizontal(double delta) {
	if (delta > 0.0) {
		fake_button(6, true);
		fake_button(6, false);
	} else if (delta < 0.0) {
		fake_button(7, true);
		fake_button(7, false);
	}
}

bool X11WM::launch_app(const std::string &command) {
	if (display_ == nullptr) {
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setenv("DISPLAY", display_str_.c_str(), 1);
		unsetenv("WAYLAND_DISPLAY");
		// GDK/Qt/SDL default to the wayland backend (socket "wayland-0")
		// and would silently end up on another compositor instead of our
		// Xvfb, so pin every toolkit to X11 for this backend.
		setenv("GDK_BACKEND", "x11", 1);
		setenv("QT_QPA_PLATFORM", "xcb", 1);
		setenv("SDL_VIDEODRIVER", "x11", 1);
		execl("/bin/sh", "sh", "-c", command.c_str(), (char *)nullptr);
		_exit(127);
	}
	return pid > 0;
}

void X11WM::push_clipboard(const std::string &text) {
	if (display_ == nullptr) {
		return;
	}
	clipboard_buf_ = text;
	XSetSelectionOwner(display_, clipboard_, owner_window_, CurrentTime);
	XSetSelectionOwner(display_, primary_, owner_window_, CurrentTime);
	clipboard_owner_ = owner_window_;
	XFlush(display_);
}

std::string X11WM::pull_clipboard() {
	if (display_ == nullptr) {
		return std::string();
	}
	Window owner = XGetSelectionOwner(display_, clipboard_);
	if (owner == None || owner == owner_window_) {
		return std::string();
	}
	Atom prop = XInternAtom(display_, "_X11WM_CLIP", False);
	XDeleteProperty(display_, owner_window_, prop);
	XConvertSelection(display_, clipboard_, utf8_string_, prop, owner_window_, CurrentTime);
	XFlush(display_);

	uint64_t deadline = now_ms() + 500;
	std::string result;
	bool got = false;
	while (now_ms() < deadline) {
		if (XPending(display_) > 0) {
			XEvent ev;
			XNextEvent(display_, &ev);
			if (ev.type == SelectionNotify && ev.xselection.requestor == owner_window_) {
				if (ev.xselection.property != None) {
					Atom type = None;
					int fmt = 0;
					unsigned long n = 0;
					unsigned long extra = 0;
					unsigned char *data = nullptr;
					if (XGetWindowProperty(display_, owner_window_, ev.xselection.property, 0, 1 << 20,
								True, utf8_string_, &type, &fmt, &n, &extra, &data) == Success) {
						if (data != nullptr && n > 0) {
							result.assign((char *)data, n);
						}
						if (data != nullptr) {
							XFree(data);
						}
					}
				}
				got = true;
				break;
			}
			handle_event(ev);
		} else {
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
		}
	}

	if (!got) {
		XConvertSelection(display_, clipboard_, string_atom_, prop, owner_window_, CurrentTime);
		XFlush(display_);
		deadline = now_ms() + 500;
		while (now_ms() < deadline && !got) {
			if (XPending(display_) > 0) {
				XEvent ev;
				XNextEvent(display_, &ev);
				if (ev.type == SelectionNotify && ev.xselection.requestor == owner_window_) {
					if (ev.xselection.property != None) {
						Atom type = None;
						int fmt = 0;
						unsigned long n = 0;
						unsigned long extra = 0;
						unsigned char *data = nullptr;
						if (XGetWindowProperty(display_, owner_window_, ev.xselection.property, 0, 1 << 20,
									True, string_atom_, &type, &fmt, &n, &extra, &data) == Success) {
							if (data != nullptr && n > 0) {
								result.assign((char *)data, n);
							}
							if (data != nullptr) {
								XFree(data);
							}
						}
					}
					got = true;
					break;
				}
				handle_event(ev);
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(3));
			}
		}
	}
	return result;
}

int X11WM::spawn_process(const std::string &path, const std::vector<std::string> &args) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, 0);
			dup2(devnull, 1);
			dup2(devnull, 2);
			if (devnull > 2) {
				close(devnull);
			}
		}
		std::vector<char *> argv;
		argv.push_back((char *)path.c_str());
		for (const std::string &a : args) {
			argv.push_back((char *)a.c_str());
		}
		argv.push_back(nullptr);
		execvp(path.c_str(), argv.data());
		_exit(127);
	}
	return pid > 0 ? (int)pid : -1;
}

bool X11WM::wait_for_socket(const std::string &path, int timeout_ms) {
	uint64_t start = now_ms();
	while (now_ms() - start < (uint64_t)timeout_ms) {
		if (access(path.c_str(), F_OK) == 0) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return false;
}

uint64_t X11WM::now_ms() {
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count();
}

int X11WM::keysym_from_godot(int key) {
	if (key >= 'A' && key <= 'Z') {
		return key + 32;
	}
	if ((key >= '0' && key <= '9') || (key >= ' ' && key <= '/') ||
			(key >= ':' && key <= '@') || (key >= '[' && key <= '`') ||
			(key >= '{' && key <= '~')) {
		return key;
	}
	switch (key) {
	case 4194305: return XK_Escape;
	case 4194306: return XK_Tab;
	case 4194308: return XK_BackSpace;
	case 4194309: return XK_Return;
	case 4194310: return XK_KP_Enter;
	case 4194311: return XK_Insert;
	case 4194312: return XK_Delete;
	case 4194313: return XK_Pause;
	case 4194314: return XK_Print;
	case 4194315: return XK_Sys_Req;
	case 4194316: return XK_Clear;
	case 4194317: return XK_Home;
	case 4194318: return XK_End;
	case 4194319: return XK_Left;
	case 4194320: return XK_Up;
	case 4194321: return XK_Right;
	case 4194322: return XK_Down;
	case 4194323: return XK_Page_Up;
	case 4194324: return XK_Page_Down;
	case 4194325: return XK_Shift_L;
	case 4194326: return XK_Control_L;
	case 4194327: return XK_Meta_L;
	case 4194328: return XK_Alt_L;
	case 4194329: return XK_Caps_Lock;
	case 4194330: return XK_Num_Lock;
	case 4194331: return XK_Scroll_Lock;
	case 4194370: return XK_Menu;
	case 4194371: return XK_Hyper_L;
	case 4194373: return XK_Help;
	case 4194433: return XK_KP_Multiply;
	case 4194434: return XK_KP_Divide;
	case 4194435: return XK_KP_Subtract;
	case 4194436: return XK_KP_Decimal;
	case 4194437: return XK_KP_Add;
	case 4194438: return XK_KP_0;
	case 4194439: return XK_KP_1;
	case 4194440: return XK_KP_2;
	case 4194441: return XK_KP_3;
	case 4194442: return XK_KP_4;
	case 4194443: return XK_KP_5;
	case 4194444: return XK_KP_6;
	case 4194445: return XK_KP_7;
	case 4194446: return XK_KP_8;
	case 4194447: return XK_KP_9;
	default: break;
	}
	if (key >= 4194332 && key <= 4194366) {
		return (int)(XK_F1 + (key - 4194332));
	}
	return 0;
}
