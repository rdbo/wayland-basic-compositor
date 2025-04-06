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

        wlr_log(WLR_INFO, "Output destroy");

        wl_list_remove(&output_info->listener_frame.link);
        wl_list_remove(&output_info->listener_request_state.link);
        wl_list_remove(&output_info->listener_destroy.link);

        // Remove output from outputs
        wl_list_remove(&output_info->link);

        free(output_info);
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
        wlr_log(WLR_INFO, "Cursor motion");

        // TODO: Implement
}

void handle_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "Cursor absolute motion");

        // TODO: Implement
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
        wlr_log(WLR_INFO, "Cursor frame");

        // TODO: Implement
}

void handle_new_input(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "New input");

        // TODO: Implement
}

void handle_request_set_cursor(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "Request set cursor");

        // TODO: Implement
}

void handle_request_set_selection(struct wl_listener *listener, void *data)
{
        wlr_log(WLR_INFO, "Request set selection");

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
