#include "window_manager.h"

extern struct process_manager g_process_manager;

static TABLE_HASH_FUNC(hash_wm)
{
    unsigned long result = *(uint32_t *) key;
    result = (result + 0x7ed55d16) + (result << 12);
    result = (result ^ 0xc761c23c) ^ (result >> 19);
    result = (result + 0x165667b1) + (result << 5);
    result = (result + 0xd3a2646c) ^ (result << 9);
    result = (result + 0xfd7046c5) + (result << 3);
    result = (result ^ 0xb55a4f09) ^ (result >> 16);
    return result;
}

static TABLE_COMPARE_FUNC(compare_wm)
{
    return *(uint32_t *) key_a == *(uint32_t *) key_b;
}

void window_manager_center_mouse(struct window_manager *wm, struct ax_window *window)
{
    if (!wm->enable_mff) return;

    CGPoint cursor;
    SLSGetCurrentCursorLocation(g_connection, &cursor);

    CGRect frame = window_frame(window);
    if (CGRectContainsPoint(frame, cursor)) return;

    uint32_t did = window_display_id(window);
    if (!did) return;

    CGPoint center = {
        frame.origin.x + frame.size.width / 2,
        frame.origin.y + frame.size.height / 2
    };

    CGRect bounds = display_bounds(did);
    if (!CGRectContainsPoint(bounds, center)) return;

    CGWarpMouseCursorPosition(center);
}

struct view *window_manager_find_managed_window(struct window_manager *wm, struct ax_window *window)
{
    return table_find(&wm->managed_window, &window->id);
}

void window_manager_remove_managed_window(struct window_manager *wm, struct ax_window *window)
{
    table_remove(&wm->managed_window, &window->id);
}

void window_manager_add_managed_window(struct window_manager *wm, struct ax_window *window, struct view *view)
{
    table_add(&wm->managed_window, &window->id, view);
}

void window_manager_move_window(struct ax_window *window, float x, float y)
{
#if 0
    int sockfd;
    char message[255];

    if (socket_connect_in(&sockfd, 5050)) {
        snprintf(message, sizeof(message), "window_move %d %.2f %.2f", window->id, x, y);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
#else
    CGPoint position = CGPointMake(x, y);
    CFTypeRef position_ref = AXValueCreate(kAXValueTypeCGPoint, (void *) &position);
    if (!position_ref) return;

    AXUIElementSetAttributeValue(window->ref, kAXPositionAttribute, position_ref);
    CFRelease(position_ref);
#endif
}

void window_manager_resize_window(struct ax_window *window, float width, float height)
{
    CGSize size = CGSizeMake(width, height);
    CFTypeRef size_ref = AXValueCreate(kAXValueTypeCGSize, (void *) &size);
    if (!size_ref) return;

    AXUIElementSetAttributeValue(window->ref, kAXSizeAttribute, size_ref);
    CFRelease(size_ref);
}

void window_manager_purify_window(struct ax_window *window)
{
    int sockfd;
    char message[255];

    if (socket_connect_in(&sockfd, 5050)) {
        snprintf(message, sizeof(message), "window_shadow %d 0", window->id);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
}

struct ax_window *window_manager_find_window_at_point(struct window_manager *wm, CGPoint point)
{
    uint32_t window_id = 0;
    CGPoint window_point;
    int window_cid;

    SLSFindWindowByGeometry(g_connection, 0, 1, 0, &point, &window_point, &window_id, &window_cid);
    return window_manager_find_window(wm, window_id);
}

static void send_de_event(ProcessSerialNumber *window_psn, uint32_t window_id)
{
    uint8_t bytes[0xf8] = {
        [0x04] = 0xf8,
        [0x08] = 0x0d,
        [0x8a] = 0x02
    };

    memcpy(bytes + 0x3c, &window_id, sizeof(uint32_t));
    SLPSPostEventRecordTo(window_psn, bytes);
}

static void send_re_event(ProcessSerialNumber *window_psn, uint32_t window_id)
{
    uint8_t bytes[0xf8] = {
        [0x04] = 0xf8,
        [0x08] = 0x0d,
        [0x8a] = 0x01
    };

    memcpy(bytes + 0x3c, &window_id, sizeof(uint32_t));
    SLPSPostEventRecordTo(window_psn, bytes);
}

static void send_pre_event(ProcessSerialNumber *window_psn, uint32_t window_id)
{
    uint8_t bytes[0xf8] = {
        [0x04] = 0xf8,
        [0x08] = 0x0d,
        [0x8a] = 0x09
    };

    memcpy(bytes + 0x3c, &window_id, sizeof(uint32_t));
    SLPSPostEventRecordTo(window_psn, bytes);
}

static void send_post_event(ProcessSerialNumber *window_psn, uint32_t window_id)
{
    uint8_t bytes1[0xf8] = {
        [0x04] = 0xF8,
        [0x08] = 0x01,
        [0x3a] = 0x10
    };

    uint8_t bytes2[0xf8] = {
        [0x04] = 0xF8,
        [0x08] = 0x02,
        [0x3a] = 0x10
    };

    memcpy(bytes1 + 0x3c, &window_id, sizeof(uint32_t));
    memcpy(bytes2 + 0x3c, &window_id, sizeof(uint32_t));
    SLPSPostEventRecordTo(window_psn, bytes1);
    SLPSPostEventRecordTo(window_psn, bytes2);
}

void window_manager_focus_window_without_raise(uint32_t window_id)
{
    int window_connection;
    ProcessSerialNumber window_psn;
    pid_t window_pid;

    SLSGetWindowOwner(g_connection, window_id, &window_connection);
    SLSGetConnectionPSN(window_connection, &window_psn);
    SLSConnectionGetPID(window_connection, &window_pid);

    send_pre_event(&window_psn, window_id);
    if (g_window_manager.focused_window_pid != window_pid) {
        _SLPSSetFrontProcessWithOptions(&window_psn, window_id, kCPSUserGenerated);
    } else {
        send_de_event(&window_psn, g_window_manager.focused_window_id);
        send_re_event(&window_psn, window_id);
    }
    send_post_event(&window_psn, window_id);
}

void window_manager_focus_window_with_raise(uint32_t window_id)
{
#if 1
    int sockfd;
    char message[255];

    if (socket_connect_in(&sockfd, 5050)) {
        snprintf(message, sizeof(message), "window_focus %d", window_id);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
#else
    struct ax_window *window = window_manager_find_window(&g_window_manager, window_id);
    if (!window) return;

    AXUIElementPerformAction(window->ref, kAXRaiseAction);
    _SLPSSetFrontProcessWithOptions(&window->application->psn, 0, kCPSNoWindows);
#endif
}

struct ax_window *window_manager_focused_window(struct window_manager *wm)
{
    ProcessSerialNumber psn = {};
    _SLPSGetFrontProcess(&psn);

    struct process *process = process_manager_find_process(&g_process_manager, &psn);
    if (!process) return NULL;

    struct ax_application *application = window_manager_find_application(wm, process->pid);
    if (!application) return NULL;

    uint32_t window_id = application_main_window(application);
    return window_manager_find_window(wm, window_id);
}

struct ax_application *window_manager_focused_application(struct window_manager *wm)
{
    ProcessSerialNumber psn = {};
    _SLPSGetFrontProcess(&psn);

    struct process *process = process_manager_find_process(&g_process_manager, &psn);
    if (!process) return NULL;

    return window_manager_find_application(wm, process->pid);
}

bool window_manager_find_lost_focused_event(struct window_manager *wm, uint32_t window_id)
{
    return table_find(&wm->window_lost_focused_event, &window_id) != NULL;
}

void window_manager_remove_lost_focused_event(struct window_manager *wm, uint32_t window_id)
{
    table_remove(&wm->window_lost_focused_event, &window_id);
}

void window_manager_add_lost_focused_event(struct window_manager *wm, uint32_t window_id, enum event_type type)
{
    table_add(&wm->window_lost_focused_event, &window_id, &type);
}

struct ax_window *window_manager_find_window(struct window_manager *wm, uint32_t window_id)
{
    struct ax_window **it = table_find(&wm->window, &window_id);
    return it ? *it : NULL;
}

void window_manager_remove_window(struct window_manager *wm, uint32_t window_id)
{
    table_remove(&wm->window, &window_id);
}

void window_manager_add_window(struct window_manager *wm, struct ax_window *window)
{
    if (wm->purify_mode != PURIFY_DISABLED) {
        window_manager_purify_window(window);
    }

    table_add(&wm->window, &window->id, &window);
}

struct ax_application *window_manager_find_application(struct window_manager *wm, pid_t pid)
{
    struct ax_application **it = table_find(&wm->application, &pid);
    return it ? *it : NULL;
}

void window_manager_remove_application(struct window_manager *wm, pid_t pid)
{
    table_remove(&wm->application, &pid);
}

void window_manager_add_application(struct window_manager *wm, struct ax_application *application)
{
    table_add(&wm->application, &application->pid, &application);
}

struct ax_window **window_manager_find_application_windows(struct window_manager *wm, struct ax_application *application, int *count)
{
    int window_count = 0;
    uint32_t window_list[255] = {};

    for (int window_index = 0; window_index < wm->window.capacity; ++window_index) {
        struct bucket *bucket = wm->window.buckets[window_index];
        while (bucket) {
            if (bucket->value) {
                struct ax_window *window = *(struct ax_window **) bucket->value;
                if (window->application == application) {
                    window_list[window_count++] = window->id;
                }
            }

            bucket = bucket->next;
        }
    }

    struct ax_window **result = malloc(sizeof(struct ax_window *) * window_count);
    *count = window_count;

    for (int i = 0; i < window_count; ++i) {
        result[i] = window_manager_find_window(wm, window_list[i]);
    }

    return result;
}

void window_manager_add_application_windows(struct window_manager *wm, struct ax_application *application)
{
    int window_count;
    struct ax_window *window;
    struct ax_window **window_list;

    window_list = application_window_list(application, &window_count);
    if (!window_list) return;

    for (int window_index = 0; window_index < window_count; ++window_index) {
        window = window_list[window_index];

        if (!window->id) {
            goto free_win;
        }

        if (window_manager_find_window(wm, window->id)) {
            goto free_win;
        }

        if (!window_observe(window)) {
            goto uobs_win;
        }

        window_manager_add_window(wm, window);
        goto next;

uobs_win:
        window_unobserve(window);
free_win:
        window_destroy(window);
next:;
    }

    free(window_list);
}

void window_manager_check_for_windows_on_space(struct space_manager *sm, struct window_manager *wm, uint64_t sid)
{
    int window_count;
    uint32_t *window_list = space_window_list(sid, &window_count);
    if (!window_list) return;

    for (int i = 0; i < window_count; ++i) {
        struct ax_window *window = window_manager_find_window(&g_window_manager, window_list[i]);
        if (!window || !window_is_standard(window)) continue;

        struct view *existing_view = window_manager_find_managed_window(&g_window_manager, window);
        if (existing_view) continue;

        struct view *view = space_manager_tile_window_on_space(&g_space_manager, window, sid);
        window_manager_add_managed_window(&g_window_manager, window, view);
    }

    free(window_list);
}

void window_manager_init(struct window_manager *wm)
{
    wm->system_element = AXUIElementCreateSystemWide();
    AXUIElementSetMessagingTimeout(wm->system_element, 1.0);

    wm->ffm_mode = FFM_DISABLED;
    wm->purify_mode = PURIFY_ALWAYS;
    wm->enable_mff = true;
    wm->enable_window_border = true;
    wm->window_border_width = 4;
    wm->active_window_border_color = 0xff775759;
    wm->normal_window_border_color = 0xff555555;

    table_init(&wm->application, 150, hash_wm, compare_wm);
    table_init(&wm->window, 150, hash_wm, compare_wm);
    table_init(&wm->managed_window, 150, hash_wm, compare_wm);
    table_init(&wm->floating_window, 150, hash_wm, compare_wm);
    table_init(&wm->window_lost_focused_event, 150, hash_wm, compare_wm);
}

void window_manager_begin(struct window_manager *wm)
{
    for (int process_index = 0; process_index < g_process_manager.process.capacity; ++process_index) {
        struct bucket *bucket = g_process_manager.process.buckets[process_index];
        while (bucket) {
            struct process *process = *(struct process **) bucket->value;
            struct ax_application *application = application_create(process);

            if (application_observe(application)) {
                window_manager_add_application(wm, application);
                window_manager_add_application_windows(wm, application);
            } else {
                application_unobserve(application);
                application_destroy(application);
            }

            bucket = bucket->next;
        }
    }

    struct ax_window *window = window_manager_focused_window(wm);
    wm->focused_window_id = window->id;
    wm->focused_window_pid = window->application->pid;
    border_window_activate(window);
}
