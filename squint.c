#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <X11/Xlib.h>          /* of course */
#ifdef HAVE_XI
#include <X11/extensions/XInput2.h>
#endif
#ifdef COPY_CURSOR
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>


GtkWidget* gtkwin = NULL;
GdkWindow* gdkwin = NULL;
Window root_window = 0;
Window window = 0;
Pixmap pixmap = -1;
int depth = -1, screen = -1;
GC gc = NULL;
GC gc_white = NULL;
Display* display = NULL;
int raised = 0;

int opt_full = 1;


GdkRectangle rect;
GdkPoint offset;
GdkPoint cursor = {0, 0};

#ifdef HAVE_XI
int track_cursor = 0;
int xi_opcode = 0;
#endif

#ifdef COPY_CURSOR
#define CURSOR_SIZE 64
int xfixes_event_base;

int copy_cursor = 0;
Window cursor_window = 0;
Pixmap cursor_pixmap = 0;
XImage* cursor_image = NULL;
Pixmap  cursor_mask_pixmap = 0;
XImage* cursor_mask_image = NULL;
GC	cursor_mask_gc = NULL;
int cursor_xhot=0;
int cursor_yhot=0;
uint32_t cursor_pixels[CURSOR_SIZE*CURSOR_SIZE];
uint8_t  cursor_mask_pixels[CURSOR_SIZE*CURSOR_SIZE];
#endif


void
error (const char* msg)
{
	fprintf(stderr, "%s\n", msg);
}

void show()
{
	if (!raised)
	{
		raised = 1;
		gdk_window_raise(gdkwin);
		if (opt_full) {
			gdk_window_fullscreen(gdkwin);
		}
	}
}

void do_hide()
{
	raised = 0;
	if (opt_full) {
		gdk_window_unfullscreen(gdkwin);
	}
	gdk_window_lower(gdkwin);
}

void hide()
{
	if(raised)
	{
		do_hide();
	}
}

gboolean on_window_button_press_event(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	do_hide();
	return FALSE;
}

void refresh_cursor_location()
{
	Window root_return, w;
	int wx, wy, mask;
	GdkPoint c;
	XQueryPointer(display, root_window, &root_return, &w,
			&c.x, &c.y, &wx, &wy, &mask);

	c.x -= rect.x;
	c.y -= rect.y;

	if ((c.x<0) | (c.y<0) | (c.x>=rect.width) | (c.y>=rect.height))
	{
		// cursor is outside the duplicated screen
		c.x = c.y = -1;
	}

	if (memcmp(&cursor, &c, sizeof(cursor)) == 0) {
		// nothing to do
		return;
	}

	// cursor was really moved
	cursor = c;

#ifdef COPY_CURSOR
	if (copy_cursor) {
		// move the cursor window to the new location of the cursor
		XMoveWindow (display, cursor_window,
				cursor.x - cursor_xhot,
				cursor.y - cursor_yhot);

		// force redrawing the window
		XClearWindow(display, cursor_window);
	}
#endif

	if (cursor.x >= 0) {
		/* raise the window when the pointer enters the duplicated screen */
		show();
	} else {
		/* lower the window when the pointer leaves the duplicated screen */
		hide();
	}
}

gboolean
refresh_image (gpointer data)
{
	int code;
	int inside_rect=0;

#ifdef HAVE_XI
	if (!track_cursor)
#endif
	{
		refresh_cursor_location();
	}

	XCopyArea (display, root_window, pixmap, gc,
					rect.x, rect.y,
					rect.width, rect.height,
					0, 0);

	// draw the cursor (crosshair)
	if (!copy_cursor && (cursor.x >= 0)) {
		#define LEN 3
		XDrawLine (display, pixmap, gc_white, cursor.x-(LEN+1), cursor.y, cursor.x+(LEN+2), cursor.y);
		XDrawLine (display, pixmap, gc_white, cursor.x, cursor.y-(LEN+1), cursor.x, cursor.y+(LEN+2));
		XDrawLine (display, pixmap, gc, cursor.x-LEN, cursor.y, cursor.x+LEN, cursor.y);
		XDrawLine (display, pixmap, gc, cursor.x, cursor.y-LEN, cursor.x, cursor.y+LEN);
	}

	// force refreshing the window's background
	XClearWindow(display, window);


	XFlush (display);

	return TRUE;
}


#ifdef COPY_CURSOR
void refresh_cursor_image()
{
	XFixesCursorImage* img = XFixesGetCursorImage (display);
	if (!img)
		return;

	int width  = (img->width  < CURSOR_SIZE) ? img->width  : CURSOR_SIZE;
	int height = (img->height < CURSOR_SIZE) ? img->height : CURSOR_SIZE;

	int x, y;
	// copy the cursor image
	for(x=0 ; x<width ; x++)
	{
		for(y=0 ; y<height ; y++)
		{
			cursor_pixels[y*CURSOR_SIZE + x] = img->pixels[y*img->width + x];
		}
	}

	// copy the cursor mask
	memset(cursor_mask_pixels, 0, sizeof(cursor_mask_pixels));
	for(x=0 ; x<width ; x++)
	{
		for(y=0 ; y<height ; y++)
		{
			if(img->pixels[y*img->width + x] >> 24)
			{
				cursor_mask_pixels[y*(CURSOR_SIZE/8) + (x/8)] |= 1 << (x % 8);
			}
		}

	}

	// upload the image & mask
	XPutImage(display, cursor_pixmap, XDefaultGC(display, screen),
			cursor_image, 0, 0, 0, 0, width, height);
	XPutImage(display, cursor_mask_pixmap, cursor_mask_gc,
			cursor_mask_image, 0, 0, 0, 0, CURSOR_SIZE, CURSOR_SIZE);

	// apply the new mask
	XShapeCombineMask(display, cursor_window, ShapeBounding, 0, 0, cursor_mask_pixmap,
			ShapeSet);

	// force redrawing the cursor window
	XClearWindow(display, cursor_window);

	cursor_xhot = img->xhot;
	cursor_yhot = img->yhot;

	XFree(img);
}

void init_copy_cursor()
{
	// ensure we are in true color
	if (XDefaultDepth(display, screen) != 24) {
		return;
	}

	// check if xfixes and xshape are available on this display
	int major, minor, error_base;
	if ((	   !XFixesQueryExtension(display, &xfixes_event_base, &error_base)
		|| !XFixesQueryVersion(display, &major, &minor)
		|| (major<1)
		|| !XShapeQueryVersion(display, &major, &minor)
		|| !(((major==1) && (minor >= 1)) || (major >= 2))
	)) {
		return;
	}

	// create an image for storing the cursor
	cursor_image = XCreateImage (display, NULL, 24, ZPixmap, 0, (char*)cursor_pixels,
				CURSOR_SIZE, CURSOR_SIZE, 32, 256);
	if (!cursor_image) {
		error("XCreateImage() failed");
		return;
	}

	// create a pixmap for storing the cursor
	cursor_pixmap = XCreatePixmap (display, root_window,
				CURSOR_SIZE, CURSOR_SIZE,
				XDefaultDepth(display, screen));

	// create a pixmap for storing the mask
	cursor_mask_pixmap = XCreatePixmap (display, root_window,
				CURSOR_SIZE, CURSOR_SIZE, 1);

	// create an image for storing the mask
	cursor_mask_image = XCreateImage (display, NULL, 1, ZPixmap, 0, (char*)cursor_mask_pixels,
				CURSOR_SIZE, CURSOR_SIZE, 8, CURSOR_SIZE/8);
	if (!cursor_mask_image) {
		error("XCreateImage() failed");
		return;
	}

	// create a context for manipulating the mask
	cursor_mask_gc = XCreateGC(display, cursor_mask_pixmap, 0, NULL); 
	if(!cursor_mask_gc) {
		error ("XCreateGC() failed");
		return;
	}

	// create a sub-window displaying the cursor
	XSetWindowAttributes attr;
	attr.background_pixmap = cursor_pixmap;
	cursor_window = XCreateWindow (display,
				gdk_x11_window_get_xid(gdkwin),
				10, 10,
				CURSOR_SIZE, CURSOR_SIZE,
				0, CopyFromParent,
				InputOutput, CopyFromParent,
				CWBackPixmap, &attr);

	// refresh the cursor
	refresh_cursor_image();

	XMapWindow(display, cursor_window);

	// request cursor change notifications
	XFixesSelectCursorInput(display, gdk_x11_window_get_xid(gdkwin), XFixesCursorNotify);

	copy_cursor = 1;
}
#endif

GdkFilterReturn on_x11_event (GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
	XEvent* ev = (XEvent*)xevent;
	XGenericEventCookie *cookie = &ev->xcookie;

#ifdef HAVE_XI
	if(track_cursor)
	{
		if (	(cookie->type == GenericEvent)
		    &&	(cookie->extension == xi_opcode)
		    &&	(cookie->evtype == XI_RawMotion))
		{
			// cursor was moved
			refresh_cursor_location();
			return GDK_FILTER_REMOVE;
		}
	}
#endif

#ifdef COPY_CURSOR
	if(copy_cursor)
	{
		if (ev->type == xfixes_event_base + XFixesCursorNotify) {
			refresh_cursor_image();

			return GDK_FILTER_REMOVE;
		}
	}
#endif
	return GDK_FILTER_CONTINUE;
}

#ifdef HAVE_XI
void init_cursor_tracking()
{
	// inspired from: http://keithp.com/blogs/Cursor_tracking/
	
	int event, error;
	if (!XQueryExtension(display, "XInputExtension", &xi_opcode, &event, &error))
		return;
	

	int major=2, minor=2;
	if (XIQueryVersion(display, &major, &minor) != Success)
		return;

	XIEventMask evmasks[1];
	unsigned char mask1[(XI_LASTEVENT + 7)/8];
	memset(mask1, 0, sizeof(mask1));

	// select for button and key events from all master devices
	XISetMask(mask1, XI_RawMotion);

	evmasks[0].deviceid = XIAllMasterDevices;
	evmasks[0].mask_len = sizeof(mask1);
	evmasks[0].mask = mask1;

	XISelectEvents(display, root_window, evmasks, 1);

	track_cursor = 1;
}
#endif

void print_help()
{
	printf(
		"usage: squint [-w] [MonitorName]\n"
		"\n"
		"	MonitorName	name of the monitor to be duplicated (from xrandr)\n"
		"	-v		display version information and exit\n"
		"	-w		run inside a window instead of going fullscreen\n"
	);
	exit(1);
}

int
main (int argc, char *argv[])
{
	const char* monitor_name;

	gtk_init (&argc, &argv);

	int opt;
	while ((opt = getopt(argc, argv, "vw")) != -1)
	{
		switch(opt)
		{
		case 'v':
			puts(APPNAME " " VERSION);
			return 0;
		case 'w':
			opt_full = 0;
			break;
		default:
			print_help();
		}
	}

	switch (argc-optind)
	{
		case 0:
			monitor_name = NULL;
			break;
		case 1:
			monitor_name = argv[optind];
			break;
		default:
			print_help();
	}

	GdkDisplay* gdisplay = gdk_display_get_default();
	if (!gdisplay) {
		fprintf (stderr, "error: no display available\n");
		return 1;
	}
	GdkScreen* gscreen = gdk_display_get_default_screen (gdisplay);

	{
		int n = gdk_screen_get_n_monitors (gscreen);
		if ((n < 2) && !monitor_name) {
			fprintf (stderr, "error: there is only *one* monitor here, what am I supposed to do?\n");
			return 1;
		}

		int i;
		int min_area = INT_MAX;
		GdkRectangle r;

		if (monitor_name == NULL)
		{
			// find the smallest screen
			for (i=0 ; i<n ; i++)
			{
				gdk_screen_get_monitor_geometry (gscreen, i, &r);
				int area = r.width * r.height;
				if (area < min_area)
					min_area = area;
			}
		}

		for (i=0 ; i<n ; i++)
		{
			gdk_screen_get_monitor_geometry (gscreen, i, &r);
			int area = r.width * r.height;

			if (monitor_name == NULL) {
				if (area == min_area)
					break;
			} else {
				if (strcmp (monitor_name, gdk_screen_get_monitor_plug_name (gscreen, i)) == 0)
					break;
			}
		}

		if (i == n)
		{
			fprintf (stderr, "error: invalid monitor\n");
			return 1;
		}

		rect = r;
		printf("Using monitor %d (%s) %dx%d  +%d+%d\n",
			i, gdk_screen_get_monitor_plug_name (gscreen, i),
			rect.width, rect.height,
			rect.x, rect.y
		);
	}


	gtkwin = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	
	{
		// load the icon
		GError* err = NULL;
		GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file (PREFIX "/share/squint/squint.png", &err);
		if (pixbuf)
		{
			gtk_window_set_icon (GTK_WINDOW(gtkwin), pixbuf);
			g_object_unref (pixbuf);
		} else {
			fprintf (stderr, "warning: no icon: %s\n", err->message);
			g_error_free (err);
		}
	}

	display = gdk_x11_get_default_xdisplay();
	//printf("display: %p\n", display);

	screen = DefaultScreen (display);
	//printf("screen: %d\n", screen);

	depth = XDefaultDepth (display, screen);
	//printf("depth: %d\n", depth);

	// get the root window
	root_window = XDefaultRootWindow(display);
	
	{
		XGCValues values;
		values.subwindow_mode = IncludeInferiors;

		gc = XCreateGC (display, root_window, GCSubwindowMode, &values);
		if(!gc) {
			error ("XCreateGC() failed");
			return 1;
		}

		values.line_width = 3;
		values.foreground = 0xe0e0e0;
		gc_white = XCreateGC (display, root_window, GCLineWidth | GCForeground, &values);
		if(!gc_white) {
			error ("XCreateGC() failed");
			return 1;
		}
	}

	// create my own window
	gtk_widget_show_all (gtkwin);

	gdkwin = gtk_widget_get_window(gtkwin);

	offset.x = 0;
	offset.y = 0;
	if (opt_full) {
		// get the monitor on which the window is displayed
		int mon = gdk_screen_get_monitor_at_window(gscreen, gdkwin);
		GdkRectangle wa;
		gdk_screen_get_monitor_workarea(gscreen, mon, &wa);

		if ((rect.x == wa.x) && (rect.y == wa.y)) {
			// same as the source monitor
			// -> do NOT go fullscreen
			opt_full = 0;
			fprintf(stderr, "error: cannot duplicate the output on the same monitor, falling back to window mode\n");
		} else {
			// black background
			GdkRGBA black = {0,0,0,1};
			gtk_widget_override_background_color(gtkwin, 0, &black);

			// go full screen
			gdk_window_fullscreen (gdkwin);

			// adjust the offset to draw the screen in the center of the window
			int margin_x = wa.width - rect.width;
			if (margin_x > 0) {
				offset.x = margin_x / 2;
			}
			int margin_y = wa.height - rect.height;
			if (margin_y > 0) {
				offset.y = margin_y /2;
			}
		}
	}

	// quit on window closed
	g_signal_connect (gtkwin, "destroy", G_CALLBACK (gtk_main_quit), NULL);

	// hide the window on click
	g_signal_connect (gtkwin, "button-press-event", G_CALLBACK (on_window_button_press_event), NULL);
	gdk_window_set_events (gdkwin, gdk_window_get_events(gdkwin) | GDK_BUTTON_PRESS_MASK);

	// create the pixmap
	pixmap = XCreatePixmap (display, root_window, rect.width, rect.height, depth);
	
	// create the sub-window
	{
		XSetWindowAttributes attr;
		attr.background_pixmap = pixmap;
		window = XCreateWindow (display,
					gdk_x11_window_get_xid(gdkwin),
					offset.x, offset.y,
					rect.width, rect.height,
					0, CopyFromParent,
					InputOutput, CopyFromParent,
					CWBackPixmap, &attr);
		XMapWindow(display, window);
	}

#ifdef HAVE_XI
	init_cursor_tracking();
#endif

#ifdef COPY_CURSOR
	init_copy_cursor();
#endif

	XFlush (display);

	GValue v = G_VALUE_INIT;
	g_value_init (&v, G_TYPE_INT);

	g_value_set_int (&v, rect.width);
	g_object_set_property (G_OBJECT(gtkwin), "width-request", &v);

	g_value_set_int (&v, rect.height);
	g_object_set_property (G_OBJECT(gtkwin), "height-request", &v);

	// catch all X11 events
	gdk_window_add_filter(NULL, on_x11_event, NULL);

	g_timeout_add (40, &refresh_image, NULL);

	gtk_main ();

	XFreePixmap (display, pixmap);

	return 0;
}
