#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wordexp.h>
#include "cairo.h"
#include "comm.h"
#include "log.h"
#include "loop.h"
#include "pool-buffer.h"
#include "seat.h"
#include "swaylock.h"
#include "wlr-input-inhibitor-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

int lenient_strcmp(char *a, char *b) {
	if (a == b) {
		return 0;
	} else if (!a) {
		return -1;
	} else if (!b) {
		return 1;
	} else {
		return strcmp(a, b);
	}
}

/*
static void daemonize(void) {
	int fds[2];
	if (pipe(fds) != 0) {
		swaylock_log(LOG_ERROR, "Failed to pipe");
		exit(1);
	}
	if (fork() == 0) {
		setsid();
		close(fds[0]);
		int devnull = open("/dev/null", O_RDWR);
		dup2(STDOUT_FILENO, devnull);
		dup2(STDERR_FILENO, devnull);
		close(devnull);
		uint8_t success = 0;
		if (chdir("/") != 0) {
			write(fds[1], &success, 1);
			exit(1);
		}
		success = 1;
		if (write(fds[1], &success, 1) != 1) {
			exit(1);
		}
		close(fds[1]);
	} else {
		close(fds[1]);
		uint8_t success;
		if (read(fds[0], &success, 1) != 1 || !success) {
			swaylock_log(LOG_ERROR, "Failed to daemonize");
			exit(1);
		}
		close(fds[0]);
		exit(0);
	}
}*/

static void destroy_surface(struct swaylock_surface *surface) {
	wl_list_remove(&surface->link);
	if (surface->layer_surface != NULL) {
		zwlr_layer_surface_v1_destroy(surface->layer_surface);
	}
	if (surface->surface != NULL) {
		wl_surface_destroy(surface->surface);
	}
	destroy_buffer(&surface->buffers[0]);
	destroy_buffer(&surface->buffers[1]);
	wl_output_destroy(surface->output);
	free(surface);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener;

static void create_layer_surface(struct swaylock_surface *surface) {
	struct swaylock_state *state = surface->state;

	surface->surface = wl_compositor_create_surface(state->compositor);
	assert(surface->surface);

	surface->child = wl_compositor_create_surface(state->compositor);
	assert(surface->child);
	surface->subsurface = wl_subcompositor_get_subsurface(state->subcompositor, surface->child, surface->surface);
	assert(surface->subsurface);
	wl_subsurface_set_sync(surface->subsurface);

	surface->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			state->layer_shell, surface->surface, surface->output,
			ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "lockscreen");
	assert(surface->layer_surface);

	zwlr_layer_surface_v1_set_size(surface->layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_anchor(surface->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_exclusive_zone(surface->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
			surface->layer_surface, true);
	zwlr_layer_surface_v1_add_listener(surface->layer_surface,
			&layer_surface_listener, surface);

	wl_surface_commit(surface->surface);
}

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct swaylock_surface *surface = data;
	surface->width = width;
	surface->height = height;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	render_frame_background(surface);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *layer_surface) {
	struct swaylock_surface *surface = data;
	destroy_surface(surface);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static const struct wl_callback_listener surface_frame_listener;

static void surface_frame_handle_done(void *data, struct wl_callback *callback,
		uint32_t time) {
	struct swaylock_surface *surface = data;

	wl_callback_destroy(callback);
	surface->frame_pending = false;

	if (surface->dirty) {
		// Schedule a frame in case the surface is damaged again
		struct wl_callback *callback = wl_surface_frame(surface->surface);
		wl_callback_add_listener(callback, &surface_frame_listener, surface);
		surface->frame_pending = true;

		render_frame_background(surface);
		surface->dirty = false;
	}
}

static const struct wl_callback_listener surface_frame_listener = {
	.done = surface_frame_handle_done,
};

void damage_surface(struct swaylock_surface *surface) {
	surface->dirty = true;
	if (surface->frame_pending) {
		return;
	}

	struct wl_callback *callback = wl_surface_frame(surface->surface);
	wl_callback_add_listener(callback, &surface_frame_listener, surface);
	surface->frame_pending = true;
	wl_surface_commit(surface->surface);
}

void damage_state(struct swaylock_state *state) {
	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		damage_surface(surface);
	}
}

static void handle_wl_output_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t width_mm, int32_t height_mm,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	struct swaylock_surface *surface = data;
	surface->subpixel = subpixel;
	if (surface->state->run_display) {
		damage_surface(surface);
	}
}

static void handle_wl_output_mode(void *data, struct wl_output *output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	// Who cares
}

static void handle_wl_output_done(void *data, struct wl_output *output) {
	// Who cares
}

static void handle_wl_output_scale(void *data, struct wl_output *output,
		int32_t factor) {
	struct swaylock_surface *surface = data;
	surface->scale = factor;
	if (surface->state->run_display) {
		damage_surface(surface);
	}
}

struct wl_output_listener _wl_output_listener = {
	.geometry = handle_wl_output_geometry,
	.mode = handle_wl_output_mode,
	.done = handle_wl_output_done,
	.scale = handle_wl_output_scale,
};

static void handle_xdg_output_logical_size(void *data, struct zxdg_output_v1 *output,
		int width, int height) {
	// Who cares
}

static void handle_xdg_output_logical_position(void *data,
		struct zxdg_output_v1 *output, int x, int y) {
	// Who cares
}

static void handle_xdg_output_name(void *data, struct zxdg_output_v1 *output,
		const char *name) {
	swaylock_log(LOG_DEBUG, "output name is %s", name);
	struct swaylock_surface *surface = data;
	surface->xdg_output = output;
	surface->output_name = strdup(name);
}

static void handle_xdg_output_description(void *data, struct zxdg_output_v1 *output,
		const char *description) {
	// Who cares
}

static void handle_xdg_output_done(void *data, struct zxdg_output_v1 *output) {
	// Who cares
}

struct zxdg_output_v1_listener _xdg_output_listener = {
	.logical_position = handle_xdg_output_logical_position,
	.logical_size = handle_xdg_output_logical_size,
	.done = handle_xdg_output_done,
	.name = handle_xdg_output_name,
	.description = handle_xdg_output_description,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct swaylock_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 3);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		state->subcompositor = wl_registry_bind(registry, name,
				&wl_subcompositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name,
				&wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *seat = wl_registry_bind(
				registry, name, &wl_seat_interface, 3);
		struct swaylock_seat *swaylock_seat =
			calloc(1, sizeof(struct swaylock_seat));
		swaylock_seat->state = state;
		wl_seat_add_listener(seat, &seat_listener, swaylock_seat);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell = wl_registry_bind(
				registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, zwlr_input_inhibit_manager_v1_interface.name) == 0) {
		state->input_inhibit_manager = wl_registry_bind(
				registry, name, &zwlr_input_inhibit_manager_v1_interface, 1);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		state->zxdg_output_manager = wl_registry_bind(
				registry, name, &zxdg_output_manager_v1_interface, 2);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct swaylock_surface *surface =
			calloc(1, sizeof(struct swaylock_surface));
		surface->state = state;
		surface->output = wl_registry_bind(registry, name,
				&wl_output_interface, 3);
		surface->output_global_name = name;
		wl_output_add_listener(surface->output, &_wl_output_listener, surface);
		wl_list_insert(&state->surfaces, &surface->link);

		if (state->run_display) {
			create_layer_surface(surface);
			wl_display_roundtrip(state->display);
		}
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct swaylock_state *state = data;
	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state->surfaces, link) {
		if (surface->output_global_name == name) {
			destroy_surface(surface);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static void set_default_colors(struct swaylock_colors *colors) {
	colors->background = 0x000000FF;
	colors->wrong = 0xCC3333FF;
	colors->input = 0x005577FF;
	colors->validate = 0x9400D3FF;
}


static struct swaylock_state state;

static void display_in(int fd, short mask, void *data) {
	if (wl_display_dispatch(state.display) == -1) {
		state.run_display = false;
	}
}

static void comm_in(int fd, short mask, void *data) {
	if (read_comm_reply()) {
		// Authentication succeeded
		state.run_display = false;
	} else {
		state.auth_state = AUTH_STATE_INVALID;
		//schedule_indicator_clear(&state);
		++state.failed_attempts;
		damage_state(&state);
	}
}

int main(int argc, char **argv) {
	swaylock_log_init(LOG_ERROR);
	initialize_pw_backend(argc, argv);

	state.failed_attempts = 0;
	wl_list_init(&state.images);
	set_default_colors(&state.args.colors);


#ifdef __linux__
	// Most non-linux platforms require root to mlock()
	if (mlock(state.password.buffer, sizeof(state.password.buffer)) != 0) {
		swaylock_log(LOG_ERROR, "Unable to mlock() password memory.");
		return EXIT_FAILURE;
	}
#endif

	wl_list_init(&state.surfaces);
	state.xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	state.display = wl_display_connect(NULL);
	if (!state.display) {
		swaylock_log(LOG_ERROR, "Unable to connect to the compositor. "
				"If your compositor is running, check or set the "
				"WAYLAND_DISPLAY environment variable.");
		return EXIT_FAILURE;
	}

	struct wl_registry *registry = wl_display_get_registry(state.display);
	wl_registry_add_listener(registry, &registry_listener, &state);
	wl_display_roundtrip(state.display);
	assert(state.compositor && state.layer_shell && state.shm);
	if (!state.input_inhibit_manager) {
		swaylock_log(LOG_ERROR, "Compositor does not support the input "
				"inhibitor protocol, refusing to run insecurely");
		return 1;
	}

	if (wl_list_empty(&state.surfaces)) {
		swaylock_log(LOG_ERROR, "Exiting - no outputs to show on.");
		return 0;
	}

	zwlr_input_inhibit_manager_v1_get_inhibitor(state.input_inhibit_manager);
	if (wl_display_roundtrip(state.display) == -1) {
		swaylock_log(LOG_ERROR, "Exiting - failed to inhibit input:"
				" is another lockscreen already running?");
		return 2;
	}

	if (state.zxdg_output_manager) {
		struct swaylock_surface *surface;
		wl_list_for_each(surface, &state.surfaces, link) {
			surface->xdg_output = zxdg_output_manager_v1_get_xdg_output(
						state.zxdg_output_manager, surface->output);
			zxdg_output_v1_add_listener(
					surface->xdg_output, &_xdg_output_listener, surface);
		}
		wl_display_roundtrip(state.display);
	} else {
		swaylock_log(LOG_INFO, "Compositor does not support zxdg output "
				"manager, images assigned to named outputs will not work");
	}

	struct swaylock_surface *surface;
	wl_list_for_each(surface, &state.surfaces, link) {
		create_layer_surface(surface);
	}

	wl_display_roundtrip(state.display);
	//daemonize();

	state.eventloop = loop_create();
	loop_add_fd(state.eventloop, wl_display_get_fd(state.display), POLLIN,
			display_in, NULL);

	loop_add_fd(state.eventloop, get_comm_reply_fd(), POLLIN, comm_in, NULL);

	state.run_display = true;
	while (state.run_display) {
		errno = 0;
		if (wl_display_flush(state.display) == -1 && errno != EAGAIN) {
			break;
		}
		loop_poll(state.eventloop);
	}

	return 0;
}
