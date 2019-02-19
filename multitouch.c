#if HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <poll.h>
#include <stdint.h>
#include <math.h>

#include <errno.h>

#include <cairo.h>
#include <cairo-xlib.h>

#include <X11/Xlib.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/XInput2.h>

#define MAX_TOUCHES 10

enum TouchState {
    TSTATE_END = 0,
    TSTATE_BEGIN,
    TSTATE_UPDATE,
};

struct touchpoint {
    enum TouchState state;
    uint32_t touchid; /* 0 == unused */
    double x, y;   /* last recorded x/y position */
};

enum TouchFlags {
    TFLAG_ACCEPTED = (1 << 0),
};

struct multitouch {
    Display *dpy;
    int screen_no;
    Screen* screen;
    Window root;
    Window win;
    Visual *visual;
    int xi_opcode;

    int width;
    int height;

    cairo_t *cr;
    cairo_t *cr_win;
    cairo_t *cr_grabs;
    cairo_surface_t *surface;
    cairo_surface_t *surface_win;
    cairo_surface_t *surface_grabs;

    struct touchpoint touches[MAX_TOUCHES];
    int ntouches;
};

static int error(const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "E: ");

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    return EXIT_FAILURE;
}

static int running = 1;
static void sighandler(int signal)
{
    running = 0;
    error("signal received, shutting down\n");
}

static int init_x11(struct multitouch *mt, int width, int height)
{
    Display *dpy;
    int major = 2, minor = 2;
    int xi_opcode, xi_error, xi_event;

    dpy = XOpenDisplay(NULL);
    if (!dpy)
        return error("Invalid DISPLAY.\n");

    if (!XQueryExtension(dpy, INAME, &xi_opcode, &xi_event, &xi_error))
        return error("No X Input extension\n");

    if (XIQueryVersion(dpy, &major, &minor) != Success ||
        major * 10 + minor < 22)
        return error("Need XI 2.2\n");

    mt->dpy       = dpy;
    mt->screen_no = DefaultScreen(dpy);
    mt->screen    = DefaultScreenOfDisplay(dpy);
    mt->root      = DefaultRootWindow(dpy);
    mt->visual    = DefaultVisual(dpy, mt->screen_no);
    mt->xi_opcode = xi_opcode;
    mt->width     = width;
    mt->height    = height;

    Window win;
    //~ XEvent event;
    XIEventMask evmask;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)];

    win = XCreateSimpleWindow(mt->dpy, mt->root, 0, 0, mt->width, mt->height,
                              0, 0, WhitePixel(mt->dpy, mt->screen_no));
    mt->win = win;
    if (!mt->win)
        return error("Failed to create window.\n");

    evmask.mask = mask;
    evmask.mask_len = sizeof(mask);
    memset(mask, 0, sizeof(mask));
    evmask.deviceid = XIAllMasterDevices;

    /* select for touch or pointer events on main window */
    XISetMask(mask, XI_TouchBegin);
    XISetMask(mask, XI_TouchUpdate);
    XISetMask(mask, XI_TouchEnd);
    XISelectEvents(mt->dpy, win, &evmask, 1);

    XSelectInput(mt->dpy, win, ExposureMask);
    XMapSubwindows(mt->dpy, win);
    XMapWindow(mt->dpy, win);

    XFlush(mt->dpy);
    return EXIT_SUCCESS;
}

static int init_cairo(struct multitouch *mt)
{
    cairo_surface_t *surface;
    cairo_t *cr;

    /* frontbuffer */
    surface = cairo_xlib_surface_create(mt->dpy, mt->win,
                                        mt->visual, mt->width, mt->height);
    if (!surface)
        return error("Failed to create cairo surface\n");

    mt->surface_win = surface;

    cr = cairo_create(surface);
    if (!cr)
        return error("Failed to create cairo context\n");

    mt->cr_win = cr;

    /* grab drawing backbuffer */
    surface = cairo_surface_create_similar(surface,
                                           CAIRO_CONTENT_COLOR_ALPHA,
                                           mt->width, mt->height);
    if (!surface)
        return error("Failed to create cairo surface\n");

    mt->surface_grabs = surface;

    cr = cairo_create(surface);
    if (!cr)
        return error("Failed to create cairo context\n");

    mt->cr_grabs = cr;

    /* backbuffer */
    surface = cairo_surface_create_similar(surface,
                                           CAIRO_CONTENT_COLOR_ALPHA,
                                           mt->width, mt->height);
    if (!surface)
        return error("Failed to create cairo surface\n");

    mt->surface = surface;

    cr = cairo_create(surface);
    if (!cr)
        return error("Failed to create cairo context\n");

    cairo_set_line_width(cr, 1);
    cairo_set_source_rgb(cr, .85, .85, .85);
    cairo_rectangle(cr, 0, 0, mt->width, mt->height);
    cairo_fill(cr);

    mt->cr = cr;

    return EXIT_SUCCESS;
}

static void dumpDevices(Display *dpy)
{
    int ndevices, i, j;
    XIDeviceInfo *info = XIQueryDevice(dpy, XIAllDevices, &ndevices);
    for (i = 0; i < ndevices; i++)
    {
        XIDeviceInfo *dev = &info[i];
        printf("Device name %s\n", dev->name);
        for (j = 0; j < dev->num_classes; j++)
        {
            XIAnyClassInfo *class = dev->classes[j];
            XITouchClassInfo *t = (XITouchClassInfo*)class;

            if (class->type != XITouchClass)
                continue;

            printf("   %s touch device, supporting %d touches.\n",
                   (t->mode == XIDirectTouch) ?  "direct" : "dependent",
                   t->num_touches);
        }
    }
}

static void print_coordinates_column(int col, int id, float x, float y, float rx, float ry, char* annotation)
{
    printf("%c[%dG%d:%6.1f,%6.1f (%6.1f,%6.1f) %s", 27, col*40, id, x, y, rx, ry, annotation);
}

static void print_event(struct multitouch *mt, XIDeviceEvent* event)
{
    static uint32_t minTouchID = 0;
    static int active_touches = 0;
    switch(event->evtype)
    {
        case XI_TouchBegin:
            if (minTouchID == 0)
                minTouchID = event->detail;
            print_coordinates_column(event->detail - minTouchID, event->detail, event->event_x, event->event_y, event->root_x, event->root_y, "begin");
            ++active_touches;
            break;
        case XI_TouchUpdate:
            if (event->detail == minTouchID)
                printf("\n");
            print_coordinates_column(event->detail - minTouchID, event->detail, event->event_x, event->event_y, event->root_x, event->root_y, "");
            break;
        case XI_TouchEnd:
            print_coordinates_column(event->detail - minTouchID, event->detail, event->event_x, event->event_y, event->root_x, event->root_y, "end");
            --active_touches;
            if (active_touches < 0) {
                printf("ERROR: received more TouchEnd than TouchBegin");
                active_touches = 0;
            }
            if (active_touches == 0)
                minTouchID = 0;
            break;
    }
    fflush(stdout);
}

static void expose(struct multitouch *mt, int x1, int y1, int x2, int y2)
{
    cairo_set_source_surface(mt->cr_win, mt->surface, 0, 0);
    cairo_paint(mt->cr_win);

    cairo_save(mt->cr_win);
    cairo_set_source_surface(mt->cr_win, mt->surface_grabs, 0, 0);
    cairo_mask_surface(mt->cr_win, mt->surface_grabs, 0, 0);
    cairo_restore(mt->cr_win);
}

static struct touchpoint* find_touch(struct multitouch *mt, uint32_t touchid)
{
    int i;

    for (i = 0; i < mt->ntouches; i++)
        if (mt->touches[i].state != TSTATE_END &&
            mt->touches[i].touchid == touchid)
            return &mt->touches[i];

    return NULL;
}

static void paint_touch_begin(struct multitouch *mt, XIDeviceEvent *event)
{
    int i;
    int radius = 30;
    struct touchpoint *t = NULL;

    for (i = 0; i < mt->ntouches; i++)
    {
        if (mt->touches[i].state == TSTATE_END)
        {
            t = &mt->touches[i];
            break;
        }
    }
    if (!t)
    {
        error("Too many touchpoints, skipping\n");
        return;
    }

    t->touchid = event->detail;
    t->x = event->event_x;
    t->y = event->event_y;
    t->state = TSTATE_BEGIN;

    cairo_save(mt->cr);
    cairo_set_source_rgba(mt->cr, 0, 0, 0, .5);
    cairo_arc(mt->cr, t->x, t->y, radius, 0, 2 * M_PI);
    cairo_stroke(mt->cr);
    cairo_restore(mt->cr);
    expose(mt, t->x - radius, t->y - radius, t->x + radius, t->y + radius);
}

static void paint_touch_update(struct multitouch *mt, XIDeviceEvent *event)
{
    struct touchpoint *t = find_touch(mt, event->detail);

    if (!t)
    {
        error("Could not find touch in %s\n", __FUNCTION__);
        return;
    }

    cairo_save(mt->cr);
    cairo_set_source_rgba(mt->cr, 0, 0, 0, .5);
    cairo_move_to(mt->cr, t->x, t->y);
    cairo_line_to(mt->cr, event->event_x, event->event_y);
    cairo_stroke(mt->cr);
    cairo_restore(mt->cr);
    expose(mt, t->x, t->y, event->event_x, event->event_y);

    t->x = event->event_x;
    t->y = event->event_y;
    t->state = TSTATE_UPDATE;
}

static void paint_touch_end(struct multitouch *mt, XIDeviceEvent *event)
{
    int rsize = 30;
    struct touchpoint *t = find_touch(mt, event->detail);

    if (!t)
    {
        error("Could not find touch in %s\n", __FUNCTION__);
        return;
    }

    t->x = event->event_x;
    t->y = event->event_y;
    t->state = TSTATE_END;

    cairo_save(mt->cr);
    cairo_set_source_rgba(mt->cr, 0, 0, 0, .5);
    cairo_rectangle(mt->cr, t->x - rsize/2, t->y - rsize/2, rsize, rsize);
    cairo_stroke(mt->cr);
    cairo_restore(mt->cr);
    expose(mt, t->x - rsize/2, t->y - rsize/2, t->x + rsize/2, t->y + rsize/2);
}

static void paint_event(struct multitouch *mt, XIDeviceEvent *event)
{
    switch(event->evtype)
    {
        case XI_TouchBegin: paint_touch_begin(mt, event); break;
        case XI_TouchUpdate: paint_touch_update(mt, event); break;
        case XI_TouchEnd: paint_touch_end(mt, event); break;
    }
}

int main(int argc, char **argv)
{
    int rc;
    struct multitouch mt;
    struct pollfd fd;

    memset(&mt, 0, sizeof(mt));
    mt.ntouches = MAX_TOUCHES;

    rc = init_x11(&mt, 800, 600);
    if (rc != EXIT_SUCCESS)
        return rc;
    dumpDevices(mt.dpy);

    rc = init_cairo(&mt);
    if (rc != EXIT_SUCCESS)
        return rc;

    signal(SIGINT, sighandler);

    fd.fd = ConnectionNumber(mt.dpy);
    fd.events = POLLIN;
    while (running)
    {
        if (poll(&fd, 1, 500) <= 0)
            continue;

        while (XPending(mt.dpy)) {
            XEvent ev;
            XGenericEventCookie *cookie = &ev.xcookie;

            XNextEvent(mt.dpy, &ev);
            if (ev.type == Expose)
            {
                expose(&mt, ev.xexpose.x, ev.xexpose.y,
                           ev.xexpose.x + ev.xexpose.width,
                           ev.xexpose.y + ev.xexpose.height);
            }
            else if (ev.type == GenericEvent &&
                XGetEventData(mt.dpy, cookie) &&
                cookie->type == GenericEvent &&
                cookie->extension == mt.xi_opcode)
            {
                print_event(&mt, cookie->data);
                paint_event(&mt, cookie->data);
            }

            XFreeEventData(mt.dpy, cookie);
        }
    }

    if (mt.win)
        XUnmapWindow(mt.dpy, mt.win);
    XCloseDisplay(mt.dpy);

    return EXIT_SUCCESS;
}
