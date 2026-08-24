// Send WM_DELETE_WINDOW to the first window whose WM_CLASS contains the
// given string — simulates a clean window-manager close for tests.
// (WM_CLASS is stable; WM_NAME changes with the shell's OSC titles.)
// Usage: wm_close <class>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static xcb_atom_t atom(xcb_connection_t *c, const char *name) {
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static xcb_window_t find_window(xcb_connection_t *c, xcb_window_t root, const char *prefix) {
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(c, xcb_query_tree(c, root), NULL);
    if (!tree) return XCB_NONE;
    xcb_window_t *kids = xcb_query_tree_children(tree);
    xcb_window_t found = XCB_NONE;
    for (int i = 0; i < xcb_query_tree_children_length(tree) && !found; i++) {
        xcb_get_property_reply_t *p = xcb_get_property_reply(
            c, xcb_get_property(c, 0, kids[i], XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 256), NULL);
        if (p && xcb_get_property_value_length(p) > 0) {
            // WM_CLASS is two NUL-terminated strings (instance, class)
            int len = xcb_get_property_value_length(p);
            const char *v = xcb_get_property_value(p);
            for (int off = 0; off < len; off += (int)strlen(v + off) + 1)
                if (!strcmp(v + off, prefix)) { found = kids[i]; break; }
        }
        free(p);
        if (!found) found = find_window(c, kids[i], prefix);
    }
    free(tree);
    return found;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: wm_close <name-prefix>\n"); return 2; }
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) { fprintf(stderr, "no display\n"); return 2; }
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t win = find_window(c, screen->root, argv[1]);
    if (!win) { fprintf(stderr, "window '%s*' not found\n", argv[1]); return 1; }

    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = win;
    ev.type = atom(c, "WM_PROTOCOLS");
    ev.data.data32[0] = atom(c, "WM_DELETE_WINDOW");
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(c, 0, win, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
    xcb_flush(c);
    printf("sent WM_DELETE_WINDOW to 0x%x\n", win);
    xcb_disconnect(c);
    return 0;
}
