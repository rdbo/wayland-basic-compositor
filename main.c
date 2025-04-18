#include <wayland-server.h>
#include <wlr/util/log.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output.h>
#include <stdlib.h>
#include <stdbool.h>

struct state {
        struct wl_display *display;
        struct wl_event_loop *event_loop;

        struct wlr_backend *backend;
        struct wlr_renderer *renderer;
        struct wlr_allocator *allocator;

        struct wlr_compositor *compositor;
        struct wlr_subcompositor *subcompositor;

        struct wlr_data_device_manager *ddm;

        struct wlr_output_layout *output_layout;
        struct wl_list outputs;
        struct wl_listener listener_new_output;

        struct wlr_scene *scene;
        struct wlr_scene_output_layout *scene_layout;

        struct wlr_xdg_shell *xdg_shell;
        struct wl_listener listener_xdg_new_toplevel;
        struct wl_listener listener_xdg_new_popup;
        struct wl_list toplevels;

        struct wlr_cursor *cursor;
        struct wlr_xcursor_manager *xcursor_manager;
        struct wl_listener listener_cursor_motion;
        struct wl_listener listener_cursor_motion_absolute;
        struct wl_listener listener_cursor_button;
        struct wl_listener listener_cursor_axis;
        struct wl_listener listener_cursor_frame;

        struct wlr_seat *seat;
        struct wl_list keyboards;
        struct wl_listener listener_new_input;
        struct wl_listener listener_request_set_cursor;
        struct wl_listener listener_request_set_selection;

        char *socket;
};

struct output_info {
        struct wl_list link; // This node will be linked to our original outputs wl_list
                             // so we can access this object from anywhere through the
                             // server state
        struct state *state;
        struct wlr_output *output;
        struct wl_listener listener_frame;
        struct wl_listener listener_request_state;
        struct wl_listener listener_destroy;
};

struct keyboard_info {
        struct wl_list link;
        struct state *state;
        struct wlr_keyboard *keyboard;
        struct wl_listener listener_modifiers;
        struct wl_listener listener_key;
        struct wl_listener listener_destroy;
};

struct toplevel_info {
        struct wl_list link;
        struct state *state;
        struct wlr_xdg_toplevel *xdg_toplevel;
        struct wlr_scene_tree *scene_tree;
        struct wl_listener listener_map;
        struct wl_listener listener_unmap;
        struct wl_listener listener_commit;
        struct wl_listener listener_destroy;
};

void handle_output_frame(struct wl_listener *listener, void *data)
{
        struct output_info *output_info = wl_container_of(listener, output_info, listener_frame);
        struct wlr_output_event_frame *event = (struct wlr_output_event_frame *)data;
        struct wlr_scene_output *scene_output;
        struct timespec now;

        wlr_log(WLR_INFO, "Output frame");

        // Get the scene output for this output and commit changes (if needed)
        scene_output = wlr_scene_get_scene_output(output_info->state->scene, output_info->output);
        wlr_scene_output_commit(scene_output, NULL);

        // Complete the queued frame callbacks for all surfaces of this scene output
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

void handle_output_request_state(struct wl_listener *listener, void *data)
{
        struct output_info *output_info = wl_container_of(listener, output_info, listener_request_state);
        struct wlr_output_event_request_state *event = (struct wlr_output_event_request_state *)data;
        
        wlr_log(WLR_INFO, "Output request state");

        // Simply commit the requested output state, no questions asked
        wlr_output_commit_state(output_info->output, event->state);
}

void handle_output_destroy(struct wl_listener *listener, void *data)
{
        struct output_info *output_info = wl_container_of(listener, output_info, listener_destroy);
        struct state *state = output_info->state;

        wlr_log(WLR_INFO, "Output destroy");

        wl_list_remove(&output_info->listener_frame.link);
        wl_list_remove(&output_info->listener_request_state.link);
        wl_list_remove(&output_info->listener_destroy.link);

        // Remove output from outputs
        wl_list_remove(&output_info->link);

        free(output_info);

        // Quit if there aren't any displays left
        // TODO: Check if this can lead to issues or not
        if (wl_list_empty(&state->outputs))
                wl_display_terminate(state->display);
}

void handle_new_output(struct wl_listener *listener, void *data)
{
        // NOTE: wl_container_of is basically a fancy `offsetof`
        struct state *state = (struct state *)wl_container_of(listener, state, listener_new_output);
        struct wlr_output *output = (struct wlr_output *)data;
        struct wlr_output_state output_state;
        struct wlr_output_mode *output_mode;
        struct output_info *output_info;
        struct wlr_output_layout_output *layout_output;
        struct wlr_scene_output *scene_output;

        wlr_log(WLR_INFO, "New output");

        // Configure output to use our renderer and allocator
        wlr_output_init_render(output, state->allocator, state->renderer);

        // Turn on the output if it's disabled
        wlr_output_state_init(&output_state);
        wlr_output_state_set_enabled(&output_state, true);

        // Use output's preferred mode (if it exists and the backend supports it)
        output_mode = wlr_output_preferred_mode(output);
        if (output_mode)
                wlr_output_state_set_mode(&output_state, output_mode);

        // Commit the output state
        wlr_output_commit_state(output, &output_state);
        wlr_output_state_finish(&output_state);

        // Allocate new custom state for this output
        output_info = (struct output_info *)malloc(sizeof(*output_info));
        output_info->state = state;
        output_info->output = output;

        // Setup listeners for new output
        output_info->listener_frame.notify = handle_output_frame; // render frames
        wl_signal_add(&output->events.frame, &output_info->listener_frame);

        output_info->listener_request_state.notify = handle_output_request_state; // handles resolution change and other output state requests
        wl_signal_add(&output->events.request_state, &output_info->listener_request_state);

        output_info->listener_destroy.notify = handle_output_destroy;
        wl_signal_add(&output->events.destroy, &output_info->listener_destroy);

        // Link this output on the outputs linked list
        wl_list_insert(&state->outputs, &output_info->link);

        // Add this output to output layout
        layout_output = wlr_output_layout_add_auto(state->output_layout, output);

        // Create wlr_scene_output to handle how the wlr_scene should be rendered on this output
        scene_output = wlr_scene_output_create(state->scene, output);
        wlr_scene_output_layout_add_output(state->scene_layout, layout_output, scene_output);
}


void handle_cursor_motion(struct wl_listener *listener, void *data)
{
        // This event triggers when we have a relative mouse motion (delta)
        struct state *state = (struct state *)wl_container_of(listener, state, listener_cursor_motion);
	struct wlr_pointer_motion_event *event = (struct wlr_pointer_motion_event *)data;

        wlr_log(WLR_INFO, "Cursor motion");

        wlr_cursor_set_xcursor(state->cursor, state->xcursor_manager, "default");
        wlr_cursor_move(state->cursor, &event->pointer->base, event->delta_x, event->delta_y);
        wlr_seat_pointer_clear_focus(state->seat);
}

void handle_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
        // This event triggers when we have an absolute mouse motion, which
        // can happen in things like running the compositor inside another
        // Wayland compositor (which can set the mouse absolute position)
        struct state *state = wl_container_of(listener, state, listener_cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = (struct wlr_pointer_motion_absolute_event *)data;

        wlr_log(WLR_INFO, "Cursor absolute motion");

	wlr_cursor_set_xcursor(state->cursor, state->xcursor_manager, "default");
	wlr_cursor_warp_absolute(state->cursor, &event->pointer->base, event->x, event->y);
        wlr_seat_pointer_clear_focus(state->seat);
}

void handle_cursor_button(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "Cursor button");

        // TODO: Implement
}

void handle_cursor_axis(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "Cursor axis");

        // TODO: Implement
}

void handle_cursor_frame(struct wl_listener *listener, void *data)
{
        struct state *state = wl_container_of(listener, state, listener_cursor_frame);
        static bool is_first_frame = true;

        wlr_log(WLR_INFO, "Cursor frame");

        // Set default xcursor theme on first frame
        // (without this it will only trigger on first mouse motion)
        if (is_first_frame) {
                wlr_cursor_set_xcursor(state->cursor, state->xcursor_manager, "default");
                is_first_frame = false;
        }

        // Notify focused client of the mouse frame event
        wlr_seat_pointer_notify_frame(state->seat);
}

void handle_keyboard_modifiers(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "Keyboard modifiers");

        struct keyboard_info *keyboard_info = wl_container_of(listener, keyboard_info, listener_modifiers);

        // There is a limitation in the wayland protocol which only allows one keyboard per seat.
        // But it doesn't matter that much because we can just set the current keyboard to the
        // one that triggered an event and process it normally.
        wlr_seat_set_keyboard(keyboard_info->state->seat, keyboard_info->keyboard);
        wlr_seat_keyboard_notify_modifiers(keyboard_info->state->seat, &keyboard_info->keyboard->modifiers);
}

void handle_keyboard_key(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "Keyboard key");

        struct keyboard_info *keyboard_info = wl_container_of(listener, keyboard_info, listener_key);
        struct state *state = keyboard_info->state;
        struct wlr_keyboard_key_event *event = (struct wlr_keyboard_key_event *)data;
        struct wlr_seat *seat = (struct wlr_seat *)state->seat;
        uint32_t keycode;
        const xkb_keysym_t *syms;
        int nsyms;
        uint32_t modifiers;

        // Convert libinput keycode to xkbcommon keycode
        keycode = event->keycode + 8;
        nsyms = xkb_state_key_get_syms(keyboard_info->keyboard->xkb_state, keycode, &syms);

        modifiers = wlr_keyboard_get_modifiers(keyboard_info->keyboard);

        // TODO: Skip modifiers on key listener
        if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                int i;
                char buf[128];

                xkb_state_key_get_utf8(keyboard_info->keyboard->xkb_state, keycode, buf, sizeof(buf));
                wlr_log(WLR_INFO, "Handling key press internally (modifier was set): %s", buf);

                for (i = 0; i < nsyms; ++i) {
                        switch (syms[i]) {
                        case XKB_KEY_q:
                        case XKB_KEY_Q:
                                wl_display_terminate(state->display);
                                return;
                        default:
                                break;
                        }
                }
        }

        // If we didn't handle the key event internally, we forward it to the client
        wlr_seat_set_keyboard(seat, keyboard_info->keyboard);
	wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
        wlr_log(WLR_INFO, "Key event forwarded to seat");
}

void handle_keyboard_destroy(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "Keyboard destroy");

        struct keyboard_info *keyboard_info = wl_container_of(listener, keyboard_info, listener_destroy);
	wl_list_remove(&keyboard_info->listener_modifiers.link);
	wl_list_remove(&keyboard_info->listener_key.link);
	wl_list_remove(&keyboard_info->listener_destroy.link);
	wl_list_remove(&keyboard_info->link);
	free(keyboard_info);
}

void setup_new_keyboard(struct state *state, struct wlr_input_device *device)
{
        struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(device);
        struct keyboard_info *keyboard_info = malloc(sizeof(*keyboard_info));
        struct xkb_context *xkb_context;
        struct xkb_keymap *xkb_keymap;

        keyboard_info->state = state;
        keyboard_info->keyboard = keyboard;

        // Setup XKB keymap for this keyboard with its defaults
        // (e.g US layout)
        xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_keymap = xkb_keymap_new_from_names(xkb_context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(keyboard, xkb_keymap);
        wlr_keyboard_set_repeat_info(keyboard, 50, 300); // Key repeat frequency and delay

        // Setup keyboard event listeners
        keyboard_info->listener_modifiers.notify = handle_keyboard_modifiers;
        wl_signal_add(&keyboard->events.modifiers, &keyboard_info->listener_modifiers);
        keyboard_info->listener_key.notify = handle_keyboard_key;
        wl_signal_add(&keyboard->events.key, &keyboard_info->listener_key);
        keyboard_info->listener_destroy.notify = handle_keyboard_destroy;
        wl_signal_add(&device->events.destroy, &keyboard_info->listener_destroy);

        // Finish keyboard setup
        wlr_seat_set_keyboard(state->seat, keyboard_info->keyboard);
        wl_list_insert(&state->keyboards, &keyboard_info->link);
}

void handle_new_input(struct wl_listener *listener, void *data)
{
        struct state *state = wl_container_of(listener, state, listener_new_input);
        struct wlr_input_device *device = (struct wlr_input_device *)data;
                
        wlr_log(WLR_INFO, "New input");

        switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
                setup_new_keyboard(state, device);
                break;
        case WLR_INPUT_DEVICE_POINTER:
                wlr_cursor_attach_input_device(state->cursor, device);
                break;
        default:
                break;
        }
}

void handle_request_set_cursor(struct wl_listener *listener, void *data)
{
        struct state *state = wl_container_of(listener, state, listener_request_set_cursor);
        struct wlr_seat_pointer_request_set_cursor_event *event = (struct wlr_seat_pointer_request_set_cursor_event *)data;
        struct wlr_seat_client *focused_client = state->seat->pointer_state.focused_client;

        wlr_log(WLR_INFO, "Request set cursor");

        // Anyone can send this request, but we must only accept
        // cursor changes by the seat client
        if (focused_client != event->seat_client)
                return;

        wlr_cursor_set_surface(state->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

void handle_request_set_selection(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "Request set selection");

        // TODO: Implement
}

void handle_xdg_toplevel_map(struct wl_listener *listener, void *data)
{
	struct toplevel_info *toplevel_info = wl_container_of(listener, toplevel_info, listener_map);
	struct state *state = toplevel_info->state;
	struct wlr_seat *seat = state->seat;
	struct wlr_keyboard *keyboard;

        wlr_log(WLR_INFO, "XDG toplevel map");

        wlr_scene_node_raise_to_top(&toplevel_info->scene_tree->node);

	// Activate the toplevel surface
	wlr_xdg_toplevel_set_activated(toplevel_info->xdg_toplevel, true);

	// Move keyboard focus to window
	keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, toplevel_info->xdg_toplevel->base->surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}

}

void handle_xdg_toplevel_unmap(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "XDG toplevel unmap");
}

void handle_xdg_toplevel_commit(struct wl_listener *listener, void *data)
{
        struct toplevel_info *toplevel_info = wl_container_of(listener, toplevel_info, listener_commit);

        wlr_log(WLR_INFO, "XDG toplevel commit");

	if (toplevel_info->xdg_toplevel->base->initial_commit) {
		// The compostor has to reply with a configure when the xdg_surface does an initial commit
		// so the client can map its surface
		wlr_xdg_toplevel_set_size(toplevel_info->xdg_toplevel, 0, 0);
	}

}

void handle_xdg_toplevel_destroy(struct wl_listener *listener, void *data)
{
        struct toplevel_info *toplevel_info = (struct toplevel_info *)wl_container_of(listener, toplevel_info, listener_destroy);

        wlr_log(WLR_INFO, "XDG toplevel destroy");

        wl_list_remove(&toplevel_info->listener_map.link);
	wl_list_remove(&toplevel_info->listener_unmap.link);
	wl_list_remove(&toplevel_info->listener_commit.link);
	wl_list_remove(&toplevel_info->listener_destroy.link);

        wl_list_remove(&toplevel_info->link);
        free(toplevel_info);
}

void handle_xdg_new_toplevel(struct wl_listener *listener, void *data)
{
        struct state *state = (struct state *)wl_container_of(listener, state, listener_xdg_new_toplevel);
        struct wlr_xdg_toplevel *xdg_toplevel = (struct wlr_xdg_toplevel *)data;
        struct toplevel_info *toplevel_info;
        
        wlr_log(WLR_INFO, "XDG new toplevel");

        toplevel_info = (struct toplevel_info *)malloc(sizeof(*toplevel_info));
        toplevel_info->state = state;
        toplevel_info->xdg_toplevel = xdg_toplevel;
        toplevel_info->scene_tree = wlr_scene_xdg_surface_create(&toplevel_info->state->scene->tree, xdg_toplevel->base);
        toplevel_info->scene_tree->node.data = toplevel_info;
	xdg_toplevel->base->data = toplevel_info->scene_tree;

        // Setup events
        toplevel_info->listener_map.notify = handle_xdg_toplevel_map;
        wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel_info->listener_map);

        toplevel_info->listener_unmap.notify = handle_xdg_toplevel_unmap;
        wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel_info->listener_unmap);

        toplevel_info->listener_commit.notify = handle_xdg_toplevel_commit;
        wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel_info->listener_commit);

        toplevel_info->listener_destroy.notify = handle_xdg_toplevel_destroy;
        wl_signal_add(&xdg_toplevel->events.destroy, &toplevel_info->listener_destroy);

        wl_list_insert(&state->toplevels, &toplevel_info->link);
}

void handle_xdg_new_popup(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "XDG new popup");

        // TODO: Implement
}

int main()
{
        struct state state = { 0 };

        // Set up logger
        wlr_log_init(WLR_INFO, NULL);
        wlr_log(WLR_INFO, "Initializing...");

        // Create wayland display
        state.display = wl_display_create();
        state.event_loop = wl_display_get_event_loop(state.display);

        // Create wlroots backend (handles hardware IO), renderer (drawing) and
        // allocator (bridge for renderer to backend)
        state.backend = wlr_backend_autocreate(state.event_loop, NULL);
        state.renderer = wlr_renderer_autocreate(state.backend);
	wlr_renderer_init_wl_display(state.renderer, state.display); // Initializes handler and shared memory buffer

        state.allocator = wlr_allocator_autocreate(state.backend, state.renderer);

        // Create compositor (allows clients to allocate surfaces) and
        // subcompositor (allows assigning roles of subsurfaces to surfaces)
        state.compositor = wlr_compositor_create(state.display, 5, state.renderer);
        state.subcompositor = wlr_subcompositor_create(state.display);

        // Create data device manager (handles clipboard)
        state.ddm = wlr_data_device_manager_create(state.display);

        // Create output layout (arranges screens in a physical layout)
        state.output_layout = wlr_output_layout_create(state.display);

        // Setup listener for new outputs
        // NOTE: This wl_list wont allocate anything, it will just set
        //       up the initial pointers which will point to valid data
        //       allocated elsewhere. We dont have to do cleanup.
        wl_list_init(&state.outputs);
        state.listener_new_output.notify = handle_new_output;
        wl_signal_add(&state.backend->events.new_output, &state.listener_new_output);

        // Create a wlr_scene to automatically manage rendering and damage tracking,
        // and also create a wlr_scene_output_layout which would allow features such
        // as multiple workspaces, multi-monitor support, etc.
        state.scene = wlr_scene_create();
        state.scene_layout = wlr_scene_attach_output_layout(state.scene, state.output_layout);

        // Create a wlr_xdg_shell which handles roles for application windows
        state.xdg_shell = wlr_xdg_shell_create(state.display, 3);

        // Setup toplevel and popup surface handlers
        // NOTE: a toplevel surface is the "main window" of a graphical application
        state.listener_xdg_new_toplevel.notify = handle_xdg_new_toplevel;
        wl_signal_add(&state.xdg_shell->events.new_toplevel, &state.listener_xdg_new_toplevel);
        state.listener_xdg_new_popup.notify = handle_xdg_new_popup;
        wl_signal_add(&state.xdg_shell->events.new_popup, &state.listener_xdg_new_popup);
        wl_list_init(&state.toplevels);

        // Create a wlr_cursor to track the cursor and an xcursor manager
        // to handle Xcursor themes and cursor scaling (HiDPI)
        state.cursor = wlr_cursor_create();
        state.xcursor_manager = wlr_xcursor_manager_create(NULL, 24);
        wlr_cursor_attach_output_layout(state.cursor, state.output_layout);

        // Setup cursor listeners
        // Events:
        //   - motion -> mouse movement
        //   - absolute motion -> processed mouse movement into X and Y coordinates
        //   - button -> mouse button press/release/click
        //   - axis -> mouse scrolling input
        //   - cursor frame -> cursor rendering needs to be updated (moved, icon changed, etc)
        state.listener_cursor_motion.notify = handle_cursor_motion;
        wl_signal_add(&state.cursor->events.motion, &state.listener_cursor_motion);
        state.listener_cursor_motion_absolute.notify = handle_cursor_motion_absolute;
        wl_signal_add(&state.cursor->events.motion_absolute, &state.listener_cursor_motion_absolute);
        state.listener_cursor_button.notify = handle_cursor_button;
        wl_signal_add(&state.cursor->events.button, &state.listener_cursor_button);
        state.listener_cursor_axis.notify = handle_cursor_axis;
        wl_signal_add(&state.cursor->events.axis, &state.listener_cursor_axis);
        state.listener_cursor_frame.notify = handle_cursor_frame;
        wl_signal_add(&state.cursor->events.frame, &state.listener_cursor_frame);

        // Setup a seat (handle HID devices) and its listeners
        state.seat = wlr_seat_create(state.display, "seat0");
        wl_list_init(&state.keyboards);
        state.listener_new_input.notify = handle_new_input;
        wl_signal_add(&state.backend->events.new_input, &state.listener_new_input);
        state.listener_request_set_cursor.notify = handle_request_set_cursor;
        wl_signal_add(&state.seat->events.request_set_cursor, &state.listener_request_set_cursor);
        state.listener_request_set_selection.notify = handle_request_set_selection;
        wl_signal_add(&state.seat->events.request_set_selection, &state.listener_request_set_selection);

        // Create Unix socket to the Wayland display
        state.socket = (char *)wl_display_add_socket_auto(state.display);
        if (!state.socket) {
                wlr_log(WLR_ERROR, "Failed to create Unix socket");
                goto CLEAN_EXIT;
        }
        setenv("WAYLAND_DISPLAY", state.socket, true);
        wlr_log(WLR_INFO, "Wayland socket: %s", state.socket);

        if (!wlr_backend_start(state.backend)) {
                wlr_log(WLR_ERROR, "Failed to start backend");
                goto CLEAN_EXIT;
        }

        // Run the Wayland event loop
        wlr_log(WLR_INFO, "Running event loop...");
        wl_display_run(state.display);

        /******************** Clean up ********************/
CLEAN_EXIT:
        wlr_seat_destroy(state.seat);
        
        wlr_xcursor_manager_destroy(state.xcursor_manager);
        wlr_cursor_destroy(state.cursor);
        
        // wlr_xdg_shell_destroy(state.xdg_shell);
        
        wlr_output_layout_destroy(state.output_layout);

        // wlr_data_device_manager_destroy(state.ddm);

        // wlr_subcompositor_destroy(state.subcompositor);
        // wlr_compositor_destroy(state.compositor);
        
        wlr_allocator_destroy(state.allocator);
        wlr_renderer_destroy(state.renderer);
        wlr_backend_destroy(state.backend);

        wl_display_destroy(state.display);

        return 0;
}
