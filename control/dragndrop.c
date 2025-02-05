#include <errno.h>
#include <poll.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/xpm.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/shape.h>

#include <control/selection.h>
#include <control/dragndrop.h>

#include "icons/file.xpm"

#define GRAB_MASK (ButtonPressMask|ButtonReleaseMask|PointerMotionMask)
#define MAX_VERSION     5
#define MIN_VERSION     3
#define LEN(a) (sizeof(a) / sizeof((a)[0]))
#define ENUM(num, str) num,
#define NAME(num, str) str,

#define ATOMS(X)                                       \
	/* atom index       atom name                */\
	X(AWARE,            "XdndAware"               )\
	X(SELECTION,        "XdndSelection"           )\
	X(MESSAGE_ENTER,    "XdndEnter"               )\
	X(MESSAGE_POSITION, "XdndPosition"            )\
	X(MESSAGE_STATUS,   "XdndStatus"              )\
	X(MESSAGE_LEAVE,    "XdndLeave"               )\
	X(MESSAGE_DROP,     "XdndDrop"                )\
	X(MESSAGE_FINISHED, "XdndFinished"            )\
	X(ACTION_COPY,      "XdndActionCopy"          )\
	X(ACTION_MOVE,      "XdndActionMove"          )\
	X(ACTION_LINK,      "XdndActionLink"          )\
	X(ACTION_ASK,       "XdndActionAsk"           )\
	X(ACTION_PRIVATE,   "XdndActionPrivate"       )\
	X(PROPERTY_TARGETS, "XdndTypeList"            )\
	X(PROPERTY_ACTIONS, "XdndActionList"          )\
	X(PROPERTY_OPACITY, "_NET_WM_WINDOW_OPACITY"  )\
	X(PROPERTY_TYPE,    "_NET_WM_WINDOW_TYPE"     )\
	X(TYPE_DND,         "_NET_WM_WINDOW_TYPE_DND" )

enum atoms {
	ATOMS(ENUM)
	NATOMS
};

#define CURSORS(X)                              \
	/* cursor index         cursor name   */\
	X(CURSOR_NODROP,        "dnd-no-drop"  )\
	X(CURSOR_DRAG,          "dnd-none"     )\
	X(CURSOR_COPY,          "dnd-copy"     )\
	X(CURSOR_MOVE,          "dnd-move"     )\
	X(CURSOR_LINK,          "dnd-link"     )\
	X(CURSOR_ASK,           "dnd-ask"      )

enum cursors {
	CURSORS(ENUM)
	NCURSORS
};

struct selection {
	/* Converted selection data.  It is packed together in order
	 * to be passed (as an opaque pointer) to ctrlsel_answer()'s
	 * callback function.
	 */
	struct ctrldnd_data const *contents;
	Atom const *targets;
	size_t ntargets;
};

static struct ctrldnd_drop const NO_DROP = {
	.action  = 0,
	.window  = None,
	.time    = 0,
	.content = {
		.data   = NULL,
		.size   = 0,
		.target = None,
	},
};

static Atom atomtab[NATOMS];

static void
init(Display *display)
{
	static char *atomnames[] = { ATOMS(NAME) };
	static Bool done = False;

	if (done)
		return;
	XInternAtoms(display, atomnames, NATOMS, False, atomtab);
	done = True;
}

static int
no_op(XEvent *event, void *arg)
{
	(void)arg;
	(void)event;
	return 0;
}

static int
ignore_error(Display *display, XErrorEvent *event)
{
	(void)display, (void)event;
	return 0;
}

static XErrorHandler
set_error_handler(Display *display, XErrorHandler fun)
{
	/*
	 * The only type of XLib error we should get are BadWindow when
	 * the window of a client we are communicating with is suddenly
	 * destroyed.
	 *
	 * To avoid crashing during IPC due to misbehavior of the other
	 * client, we set the XLib's error handler to a no-op function,
	 * perform the IPC, and then restore its original value:
	 *
	 * > XErrorHandler oldhandler = set_error_handler(display, ignore_error);
	 * > DO_SOMETHING(...);
	 * > (void)set_error_handler(display, oldhandler);
	 *
	 * We can still get other kind of errors when our own window is
	 * destroyed by another client.  We are already screwed in this
	 * case, so there's no need to catch and handle the error, just
	 * fail and terminate.
	 */
	(void)XSync(display, False);
	return XSetErrorHandler(fun);
}

static Bool
grab_input(Display *display, int screen, Time time, Cursor cursor)
{
	Window root = RootWindow(display, screen);

	return XGrabPointer(
		display, root, False, GRAB_MASK,
		GrabModeAsync, GrabModeAsync, None, cursor, time
	) == GrabSuccess && XGrabKeyboard(
		display, root, False,
		GrabModeAsync, GrabModeAsync, time
	) == GrabSuccess;
}

static void
compress_motion(Display *display, XEvent *event)
{
	XEvent next;

	if (event->type != MotionNotify)
		return;
	while (XPending(display)) {
		XPeekEvent(display, &next);
		if (next.type != MotionNotify)
			break;
		if (next.xmotion.window != event->xmotion.window)
			break;
		if (next.xmotion.subwindow != event->xmotion.subwindow)
			break;
		XNextEvent(display, event);
	}
}

static ssize_t
answer(void *arg, Atom target, unsigned char **pbuf)
{
	struct selection *dnddata = arg;

	for (size_t i = 0; i < dnddata->ntargets; i++) {
		if (dnddata->contents[i].target == target) {
			*pbuf = (unsigned char *)dnddata->contents[i].data;
			return dnddata->contents[i].size;
		}
	}
	return -1;
}

static void
send_xmessage(Display *display, Window window, Atom atom, long a, long b, long c, long d, long e)
{
	XErrorHandler oldhandler;

	oldhandler = set_error_handler(display, ignore_error);
	(void)XSendEvent(
		display, window, False, 0x0,
		&(XEvent){ .xclient = {
			.type = ClientMessage,
			.display = display,
			.serial = 0,
			.send_event = True,
			.message_type = atom,
			.window = window,
			.format = 32,
			.data.l = {a, b, c, d, e},
		}}
	);
	(void)set_error_handler(display, oldhandler);
}

static Window   /* returns parent's child winodw below pointer cursor */
get_pointed_child(Display *display, Window root, Window parent, int x, int y)
{
	Window child;
	int n;

	if (XTranslateCoordinates(display, root, parent, x, y, &n, &n, &child))
		return child;
	return None;
}

static ssize_t
get_property(Display *display, Window window, Atom property,
	Atom type, int format, void **data_ret)
{
	int status;
	Atom actual_type;
	int actual_format;
	unsigned long nitems, remainder;
	unsigned char *data = NULL;

	status = XGetWindowProperty(
		display, window, property, 0, 0xFFFFFF, False, type,
		&actual_type, &actual_format, &nitems, &remainder, &data
	);
	if (status != Success || data == NULL)
		goto error;
	if (type != AnyPropertyType && type != actual_type)
		goto error;
	if (format != 0 && format != actual_format)
		goto error;
	if (nitems <= 0)
		goto error;
	*data_ret = data;
	return nitems;
error:
	XFree(data);
	*data_ret = NULL;
	return status == BadAlloc ? -1 : 0;
}

static Bool
is_dndaware(Display *display, Window window, long *version)
{
	Atom *data = NULL;
	Bool isaware = False;

	if (get_property(display, window, atomtab[AWARE], XA_ATOM, 32, (void *)&data) == 1) {
		*version = *data;
		isaware = True;
	}
	XFree(data);
	return isaware;
}

static Window
create_dndowner(Display *display, int screen, struct selection *dnddata)
{
	Pixmap icon = None;
	Pixmap mask = None;
	Window dndowner = None;
	Window root = RootWindow(display, screen);
	XpmAttributes xpmattr = { 0 };
	unsigned int width, height;

	if (XpmCreatePixmapFromData(
		display, root, file_xpm, &icon, &mask, &xpmattr
	) != XpmSuccess)
		goto error;
	width = xpmattr.width;
	height = xpmattr.height;
	dndowner = XCreateWindow(
		display, root,
		-2 * width, -2 * height, /* off-screen to assure it stays hidden */
		width, height, 0,
		CopyFromParent, InputOutput, CopyFromParent,
		CWBackPixel|CWBorderPixel|CWBackPixmap|CWOverrideRedirect,
		&(XSetWindowAttributes){
			.background_pixmap = icon,
			.override_redirect = True,
		}
	);
	if (dndowner == None)
		goto error;
	XShapeCombineMask(display, dndowner, ShapeBounding, 0, 0, mask, ShapeSet);
	(void)XChangeProperty(
		display, dndowner, atomtab[PROPERTY_TARGETS],
		XA_ATOM, 32, PropModeReplace,
		(void *)dnddata->targets,
		dnddata->ntargets
	);
	(void)XChangeProperty(
		display, dndowner, atomtab[PROPERTY_ACTIONS],
		XA_ATOM, 32, PropModeReplace,
		(void *)(Atom[]){
			atomtab[ACTION_ASK],
			atomtab[ACTION_COPY],
			atomtab[ACTION_MOVE],
			atomtab[ACTION_LINK],
		}, 4
	);
error:
	if (icon != None) XFreePixmap(display, icon);
	if (mask != None) XFreePixmap(display, mask);
	return dndowner;
}

static Bool
is_dnd_message(Atom atom)
{
	static int const msgindices[] = {
		MESSAGE_ENTER,
		MESSAGE_POSITION,
		MESSAGE_STATUS,
		MESSAGE_LEAVE,
		MESSAGE_DROP,
		MESSAGE_FINISHED,
	};

	for (size_t i = 0; i < LEN(msgindices); i++)
		if (atom == atomtab[msgindices[i]])
			return True;
	return False;
}

static Bool
filter_event(XEvent *event, Window dndowner, Window icon,
	Time epoch, struct selection *dnddata,
	int (*callback)(XEvent *, void *), void *arg)
{
	switch (event->type) {
	case ClientMessage:
		if (event->xclient.message_type == atomtab[MESSAGE_FINISHED])
			return True;    /* broken client sent us; filter out */
		if (is_dnd_message(event->xclient.message_type))
			return False;
		break;
	case ButtonPress: case ButtonRelease:
	case KeyPress: case KeyRelease:
		return False;
	case MotionNotify:
		compress_motion(event->xmotion.display, event);
#define DND_DISTANCE    8  /* distance from pointer to dragged icon */
		XMoveWindow(
			event->xmotion.display, icon,
			event->xmotion.x_root + DND_DISTANCE,
			event->xmotion.y_root + DND_DISTANCE
		);
		return False;
	case SelectionClear:
		if (event->xselectionclear.window != dndowner)
			break;
		if (event->xselectionclear.selection != atomtab[SELECTION])
			break;
		return False;
	case SelectionRequest:
		if (event->xselectionrequest.owner != dndowner)
			break;
		if (event->xselectionrequest.selection != atomtab[SELECTION])
			break;
		(void)ctrlsel_answer(
			event, epoch,
			dnddata->targets, dnddata->ntargets,
			answer, dnddata
		);
		return True;
	default:
		break;
	}
	callback(event, arg);
	return True;
}

static int
xpoll(Display *display, Time timeout)
{
	int retval;
	struct pollfd pfd = {
		.fd = XConnectionNumber(display),
		.events = POLLIN,
	};

	if (XPending(display))
		return 1;
	while ((retval = poll(&pfd, 1, timeout)) == -1)
		if (errno != EINTR)
			break;
	return retval;
}

static Time
timediff_msec(struct timespec *start)
{
	struct timespec now;
	Time diff;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	diff = (now.tv_sec - start->tv_sec) * 1000;
	diff += now.tv_nsec / 1000000;
	diff -= start->tv_nsec / 1000000;
	*start = now;
	return diff;
}

static Bool
next_drag_event(Display *display, Window dndowner, Window icon,
	Time epoch, Time interval, XEvent *savedpos, struct timespec *now,
	struct selection *dnddata,
	int (*callback)(XEvent *, void *), void *arg, XEvent *event)
{
	for (;;) {
		static Time lasttime = 0;
		int nevents = xpoll(display, interval);

		savedpos->xmotion.time += timediff_msec(now);
		if (interval > 0 && savedpos->xmotion.time - lasttime >= interval) {
			callback(savedpos, arg);
			lasttime = savedpos->xmotion.time;
		}
		if (nevents == 0)
			continue;
		if (nevents != 1)
			return False;
		(void)XNextEvent(display, event);
		if (!filter_event(event, dndowner, icon, epoch,
			dnddata, callback, arg)) {
			return True;
		}
	}
}

static Cursor
get_cursor(Atom const cursors[], Atom action)
{
	if (action == None)
		return cursors[CURSOR_NODROP];
	if (action == atomtab[ACTION_COPY])
		return cursors[CURSOR_COPY];
	if (action == atomtab[ACTION_MOVE])
		return cursors[CURSOR_MOVE];
	if (action == atomtab[ACTION_LINK])
		return cursors[CURSOR_LINK];
	if (action == atomtab[ACTION_ASK])
		return cursors[CURSOR_ASK];
	return cursors[CURSOR_DRAG];
}

static Atom
get_action_from_modifier(enum ctrldnd_action action_mask,
	unsigned int modifier)
{
	if (action_mask == 0)
		return atomtab[CTRLDND_COPY];
	switch (modifier & (ControlMask|ShiftMask)) {
	case ControlMask:
		if ((action_mask & CTRLDND_COPY) == 0)
			return None;
		return atomtab[ACTION_COPY];
	case ShiftMask:
		if ((action_mask & CTRLDND_MOVE) == 0)
			return None;
		return atomtab[ACTION_MOVE];
	case ControlMask|ShiftMask:
		if ((action_mask & CTRLDND_LINK) == 0)
			return None;
		return atomtab[ACTION_LINK];
	default:
		if ((action_mask & CTRLDND_ASK) == 0)
			return None;
		return atomtab[ACTION_ASK];
	}
}

static enum ctrldnd_action
get_action_const(Atom action)
{
	if (action == atomtab[ACTION_COPY])
		return CTRLDND_COPY;
	if (action == atomtab[ACTION_MOVE])
		return CTRLDND_MOVE;
	if (action == atomtab[ACTION_LINK])
		return CTRLDND_LINK;
	if (action == atomtab[ACTION_ASK])
		return CTRLDND_ASK;
	if (action == atomtab[ACTION_PRIVATE])
		return CTRLDND_PRIVATE;
	return 0;
}

static Bool
is_inside(XRectangle rect, int x, int y)
{
	if (x < rect.x)
		return False;
	if (x >= rect.x + rect.width)
		return False;
	if (y < rect.y)
		return False;
	if (y >= rect.y + rect.height)
		return False;
	return True;
}

static Bool
translate_motionev(XEvent *event, Window dropsite)
{
	if (event->type != MotionNotify)
		return False;
	event->xmotion.window = dropsite;
	return XTranslateCoordinates(
		event->xmotion.display, event->xmotion.root, dropsite,
		event->xmotion.x_root, event->xmotion.y_root,
		&event->xmotion.x, &event->xmotion.y, &(Window){0}
	);
}

static XEvent
get_motionev_from_positionmsg(Display *display, Window root, XClientMessageEvent *climsg)
{
	int x, y;
	int x_root = climsg->data.l[2] >> 16;
	int y_root = climsg->data.l[2] & 0xFFFF;
	Window dropsite = climsg->window;

	(void)XTranslateCoordinates(
		display, root, dropsite,
		x_root, y_root, &x, &y, &(Window){0}
	);
	return (XEvent){ .xmotion = {
		.type           = MotionNotify,
		.display        = climsg->display,
		.send_event     = False,
		.window         = dropsite,
		.root           = root,
		.state          = 0,
		.same_screen    = True,
		.time           = climsg->data.l[3],
		.x_root         = x_root,
		.y_root         = y_root,
		.x              = x,
		.y              = y,
		.subwindow      = None,
		.is_hint        = NotifyNormal,
	}};
}

static XEvent
get_crossingev_from_motionev(XMotionEvent *motion, int type)
{
	return (XEvent){ .xcrossing = {
		.type           = type == LeaveNotify ? type : EnterNotify,
		.display        = motion->display,
		.send_event     = motion->send_event,
		.window         = motion->window,
		.root           = motion->root,
		.state          = motion->state,
		.time           = motion->time,
		.x_root         = motion->x_root,
		.y_root         = motion->y_root,
		.x              = motion->x,
		.y              = motion->y,
		.subwindow      = None,
		.mode           = NotifyNormal,
		.detail         = NotifyNonlinear,
		.same_screen    = True,
		.focus          = False,
	}};
}

static void
handle_self_message(Display *display, int screen, Bool ignorepos,
	XClientMessageEvent *climsg,
	int (*callback)(XEvent *, void *), void *arg)
{
	Window dndowner, dropsite, root;
	XEvent event;

	root = RootWindow(display, screen);
	dropsite = climsg->window;
	dndowner = climsg->data.l[0];
	if (climsg->message_type == atomtab[MESSAGE_POSITION]) {
		event = get_motionev_from_positionmsg(
			display, root, climsg
		);
		callback(&event, arg);
		if (ignorepos)
			return;
		send_xmessage(
			display, dndowner, atomtab[MESSAGE_STATUS],
			dropsite, 0x3, 0, 0, climsg->data.l[4]
		);
	} else if (climsg->message_type == atomtab[MESSAGE_ENTER]) {
		event = get_crossingev_from_motionev(
			&event.xmotion, EnterNotify
		);
		callback(&event, arg);
	} else if (climsg->message_type == atomtab[MESSAGE_LEAVE]) {
		event = get_crossingev_from_motionev(
			&event.xmotion, LeaveNotify
		);
		callback(&event, arg);
	} else if (climsg->message_type == atomtab[MESSAGE_DROP]) {
		send_xmessage(
			display, dndowner, atomtab[MESSAGE_FINISHED],
			dropsite, 1, None, 0, 0
		);
	}
}

static Bool
wait_finished_message(Display *display, int screen,
	Window dndowner, Window dropsite, Time epoch,
	struct selection *dnddata,
	int (*callback)(XEvent *, void *), void *arg)
{
	enum {
		WAIT_NTRIES = 6,
		WAIT_TIME = 128,
	};
	XEvent event;
	Window sender;
	Atom type;

	if (dropsite == None)
		return True;
	for (
		int try = 0; try < WAIT_NTRIES;
		try++, xpoll(display, WAIT_TIME)
	) while (XPending(display)) {
		(void)XNextEvent(display, &event);
		switch (event.type) {
		case ClientMessage:
			sender = event.xclient.data.l[0];
			type = event.xclient.message_type;

			if (!is_dnd_message(event.xclient.message_type))
				break;
			if (sender == dndowner) {
				handle_self_message(
					display, screen, True,
					&event.xclient,
					callback, arg
				);
				continue;
			}
			if (sender != dropsite)
				continue;
			if (type != atomtab[MESSAGE_FINISHED])
				continue;
			return True;
		case ButtonPress: case ButtonRelease: case MotionNotify:
		case KeyPress: case KeyRelease:
			continue;
		case SelectionClear:
			if (event.xselectionclear.window != dndowner)
				break;
			if (event.xselectionclear.selection != atomtab[SELECTION])
				break;
			continue;
		case SelectionRequest:
			if (event.xselectionrequest.owner != dndowner)
				break;
			if (event.xselectionrequest.selection != atomtab[SELECTION])
				break;
			(void)ctrlsel_answer(
				&event, epoch,
				dnddata->targets, dnddata->ntargets,
				answer, dnddata
			);
			continue;
		default:
			break;
		}
		callback(&event, arg);
	}
	return False;
}

static Window
get_where_dragging(Display *display, int screen, int x, int y, long *version_ret)
{
	XErrorHandler oldhandler;
	Window root, parent, child;
	long version;

	root = RootWindow(display, screen);
	oldhandler = set_error_handler(display, ignore_error);
	for (
		parent = root;
		(child = get_pointed_child(display, root, parent, x, y)) != None;
		parent = child
	) {
		if (!is_dndaware(display, child, &version))
			continue;
		if (version < MIN_VERSION)
			break;
		if (version > MAX_VERSION)
			version = MAX_VERSION;
		goto found;
	}
	version = 0;
	child = None;
found:
	(void)set_error_handler(display, oldhandler);
	if (version_ret != NULL)
		*version_ret = version;
	return child;
}

static struct ctrldnd_drop
get_where_dropped(Display *display, int screen, Window dndowner, Window icon,
	Time epoch, Cursor const cursors[],
	struct selection *dnddata, enum ctrldnd_action actions, Time interval,
	int (*callback)(XEvent *, void *), void *arg)
{
	struct timespec now;
	XEvent event;
	Window dropsite, olddropsite;
	Atom action, oldaction;
	long version;
	Bool gotstatus, notifypos, accepted;
	XRectangle dropzone;
	XEvent savedpos;
	KeySym key;

	/* from libX11 but declared in X11/XKBlib.h */
	unsigned int XkbKeysymToModifiers(Display *, KeySym);

	dropsite = None;
loop:
	while (dropsite == None) {
		(void)XNextEvent(display, &event);
		if (filter_event(&event, dndowner, icon, epoch,
			dnddata, callback, arg))
			continue;
		if (event.type == KeyPress || event.type == KeyRelease) {
			if (XLookupKeysym(&event.xkey, 0) == XK_Escape)
				return NO_DROP;
			continue;
		}
		if (event.type == ClientMessage)
			continue;       /* out-of-time message */
		if (event.type != MotionNotify)
			return NO_DROP;
		dropsite = get_where_dragging(
			display, screen,
			event.xmotion.x_root,
			event.xmotion.y_root,
			&version
		);
	}
	(void)XChangeActivePointerGrab(
		display, GRAB_MASK,
		cursors[CURSOR_DRAG],
		CurrentTime
	);
	send_xmessage(
		display, dropsite, atomtab[MESSAGE_ENTER],
		dndowner,
		(version << 24) | (dnddata->ntargets > 3 ? 0x1 : 0x0),
		dnddata->ntargets > 0 ? dnddata->targets[0] : None,
		dnddata->ntargets > 1 ? dnddata->targets[1] : None,
		dnddata->ntargets > 2 ? dnddata->targets[2] : None
	);
	savedpos = event;
	translate_motionev(&savedpos, dropsite);
	action = get_action_from_modifier(
		actions,
		savedpos.xmotion.state
	);
reset_status:
	gotstatus = False;
	notifypos = False;
	accepted = False;
	dropzone = (XRectangle){ 0 };
resend_position:
	send_xmessage(
		display, dropsite, atomtab[MESSAGE_POSITION],
		dndowner,
		0,                      /* scrolling buttons; we dont support */
		(long)savedpos.xmotion.x_root << 16 | savedpos.xmotion.y_root,
		savedpos.xmotion.time,
		action
	);
	callback(&savedpos, arg);
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	while (next_drag_event(
		display, dndowner, icon, epoch, interval,
		&savedpos, &now, dnddata, callback, arg, &event
	)) switch (event.type) {
	case ClientMessage:
		if (event.xclient.message_type != atomtab[MESSAGE_STATUS]) {
			handle_self_message(
				display, screen, notifypos,
				&event.xclient,
				callback, arg
			);
			continue;
		}
		if ((Window)event.xclient.data.l[0] != dropsite)
			continue;       /* out-of-time message */
		accepted = (event.xclient.data.l[1] & 0x01) != 0;
		notifypos = (event.xclient.data.l[1] & 0x02) != 0;
		dropzone.x = event.xclient.data.l[2] >> 16;
		dropzone.y = event.xclient.data.l[2] & 0xFFFF;
		dropzone.width  = event.xclient.data.l[3] >> 16;
		dropzone.height = event.xclient.data.l[3] & 0xFFFF;
		action = accepted ? event.xclient.data.l[4] : None;
		if (action == None)
			accepted = False;
		(void)XChangeActivePointerGrab(
			display, GRAB_MASK,
			get_cursor(cursors, action),
			CurrentTime
		);
		gotstatus = True;
		continue;
	case MotionNotify:
		olddropsite = dropsite;
		oldaction = action;

		dropsite = get_where_dragging(
			display, screen,
			event.xmotion.x_root,
			event.xmotion.y_root,
			&version
		);
		action = get_action_from_modifier(actions, event.xmotion.state);
		if (dropsite == None) {
			(void)XChangeActivePointerGrab(
				display, GRAB_MASK,
				cursors[CURSOR_NODROP],
				CurrentTime
			);
		}
		if (dropsite == None || dropsite != olddropsite) {
			send_xmessage(
				display, dropsite, atomtab[MESSAGE_LEAVE],
				dndowner, 0, 0, 0, 0
			);
			goto loop;
		}
		if (translate_motionev(&event, dropsite))
			savedpos = event;
		if (!gotstatus)
			continue;
		if (action != oldaction)
			goto reset_status;
		if (notifypos)
			goto resend_position;
		if (!is_inside(
			dropzone,
			savedpos.xmotion.x_root, savedpos.xmotion.y_root
		))
			goto resend_position;
		continue;
	case KeyPress: case KeyRelease:
		key = XLookupKeysym(&event.xkey, 0);
		if (key == XK_Escape)
			return NO_DROP;
		/*
		 * The modifier state of a XKeyEvent is that before the
		 * event happened (why tho?).  Then, if the pressed (or
		 * released) key is a modifier key we must add (remove)
		 * that modifier in (from) the state bitset.
		 *
		 * This is the only time we need a XKB function.
		 */
		if (event.type == KeyPress)
			event.xkey.state |= XkbKeysymToModifiers(display, key);
		else
			event.xkey.state &= ~XkbKeysymToModifiers(display, key);
		oldaction = action;
		action = get_action_from_modifier(actions, event.xkey.state);
		if (action != oldaction)
			goto reset_status;
		continue;
	case ButtonPress: case ButtonRelease:
		if (accepted) {
			int x, y;

			send_xmessage(
				display, dropsite, atomtab[MESSAGE_DROP],
				dndowner, 0, event.xbutton.time, 0, 0
			);
			(void)wait_finished_message(
				display, screen,
				dndowner, dropsite, epoch,
				dnddata, callback, arg
			);
			(void)XTranslateCoordinates(
				display, savedpos.xmotion.root, dropsite,
				savedpos.xmotion.x_root,
				savedpos.xmotion.y_root,
				&x, &y, &(Window){0}
			);
			return (struct ctrldnd_drop){
				.window = dropsite,
				.action = get_action_const(action),
				.time = event.xbutton.time,
				.x = x,
				.y = y,
			};
		}
		/* FALLTHROUGH */
	default:
		send_xmessage(
			display, dropsite, atomtab[MESSAGE_LEAVE],
			dndowner, 0, 0, 0, 0
		);
		return NO_DROP;
	}
	return NO_DROP;
}

static Bool
map_icon(Display *display, Window root, Window icon)
{
	unsigned int width = 0, height = 0;

	/* map icon off-screen for now; it will follow pointer cursor later */
	if (!XGetGeometry(
		display, icon, &(Window){0}, &(int){0}, &(int){0},
		&width, &height, &(unsigned){0}, &(unsigned){0}
	) || width == 0 || height == 0) return False;
	(void)XUnmapWindow(display, icon);
	(void)XChangeWindowAttributes(
		display, icon, CWOverrideRedirect,
		&(XSetWindowAttributes){.override_redirect = True}
	);
	(void)XReparentWindow(display, icon, root, -2 * width, -2 * height);
	(void)XChangeProperty(
		display, icon, atomtab[PROPERTY_OPACITY],
		XA_CARDINAL, 32, PropModeReplace,
		(void *)&(long){
			/*
			 * Opacity for dragged icon's window.
			 * Only relevant when a compositor is running.
			 */
			0x7FFFFFFF,
		}, 1
	);
	(void)XChangeProperty(
		display, icon, atomtab[PROPERTY_TYPE],
		XA_ATOM, 32, PropModeReplace,
		(void *)&atomtab[TYPE_DND], 1
	);
	(void)XMapRaised(display, icon);
	(void)XSync(display, False);
	return True;
}

static void
unmap_icon(Display *display, Window icon)
{
	(void)XDeleteProperty(display, icon, atomtab[PROPERTY_OPACITY]);
	(void)XDeleteProperty(display, icon, atomtab[PROPERTY_TYPE]);
	(void)XUnmapWindow(display, icon);
	(void)XSync(display, False);
}

struct ctrldnd_drop
ctrldnd_drag(Display *display, int screen, Time epoch, Window icon,
	struct ctrldnd_data const contents[], size_t ncontents,
	enum ctrldnd_action actions, Time interval,
	int (*callback)(XEvent *, void *), void *arg)
{
	Cursor cursors[NCURSORS] = { 0 };
	Window dndowner = None;
	struct ctrldnd_drop drop;
	Atom targets[32];       /* hardcoded maximum */
	struct selection dnddata = {
		.targets  = targets,
		.contents = contents,
		.ntargets = 0,
	};

	init(display);
	for (size_t i = 0; i < ncontents && i < LEN(targets); i++) {
		targets[i] = contents[i].target;
		dnddata.ntargets++;
	}
	for (int i = 0; i < NCURSORS; i++) {
		static char const *cursornames[] = { CURSORS(NAME) };

		cursors[i] = XcursorLibraryLoadCursor(display, cursornames[i]);
		if (cursors[i] == None) {
			/*
			 * Cursor font does not have DND cursors.
			 * Fallback to an X11 cursor every system has.
			 */
			cursors[i] = XCreateFontCursor(display, XC_hand1);
		}
	}
	if (!grab_input(display, screen, epoch, cursors[CURSOR_DRAG]))
		goto done;
	if ((dndowner = create_dndowner(display, screen, &dnddata)) == None)
		goto done;
	if ((epoch = ctrlsel_own(display, dndowner, epoch, atomtab[SELECTION])) == 0)
		goto done;
	if (icon == None)
		icon = dndowner;
	if (!map_icon(display, RootWindow(display, screen), icon))
		goto done;
	if (callback == NULL)
		callback = no_op;
	drop = get_where_dropped(
		display, screen, dndowner, icon, epoch, cursors,
		&dnddata, actions, interval, callback, arg
	);
done:
	/*
	 * There is no need to call XSetSelectionOwner(3) to disown selection.
	 * We will automatically lose ownership when destroying the owner window.
	 */
	(void)XUngrabPointer(display, CurrentTime);
	(void)XUngrabKeyboard(display, CurrentTime);
	for (int i = 0; i < NCURSORS; i++)
		if (cursors[i] != None)
			XFreeCursor(display, cursors[i]);
	if (icon != None)
		unmap_icon(display, icon);
	if (dndowner != None)
		XDestroyWindow(display, dndowner);
	return drop;
}

static Bool
has_atom(Atom const atoms[], size_t natoms, Atom atom)
{
	if (atom == None)
		return False;
	for (size_t i = 0; i < natoms; i++)
		if (atom == atoms[i])
			return True;
	return False;
}

static Atom
find_best_target(XClientMessageEvent *climsg,
	Atom const targets[], size_t ntargets)
{
	XErrorHandler oldhandler;
	Atom target;
	Atom *atoms;
	ssize_t len;

	if (ntargets == 0)
		return None;
	for (int i = 2; i < 5; i++)
		if (has_atom(targets, ntargets, climsg->data.l[i]))
			return climsg->data.l[i];
	if ((climsg->data.l[1] & 0x1) == 0)
		return None;    /* dndowner supports no more than 3 targets */
	oldhandler = set_error_handler(climsg->display, ignore_error);
	len = get_property(
		climsg->display, climsg->window,
		atomtab[PROPERTY_TARGETS],
		XA_ATOM, 32, (void *)&atoms
	);
	target = None;
	for (ssize_t i = 0; i < len; i++) {
		if (has_atom(targets, ntargets, atoms[i])) {
			target = atoms[i];
			break;
		}
	}
	XFree(atoms);
	(void)set_error_handler(climsg->display, oldhandler);
	return target;
}

struct ctrldnd_drop
ctrldnd_getdrop(XEvent *climsg, Atom const targets[], size_t ntargets,
	enum ctrldnd_action actions, Time interval,
	int (*callback)(XEvent *, void *), void *arg)
{
	Display *display;
	XEvent event;
	Atom target, action;
	Window root, dndowner, dropsite;
	XEvent savedpos;
	Bool gotposition, do_accept;
	XRectangle dropzone;
	struct ctrldnd_drop drop;
	struct timespec now;

	if (climsg->type != ClientMessage)
		return NO_DROP;
	if (ntargets == 0)
		return NO_DROP;
	display = climsg->xclient.display;
	init(display);
	if (climsg->xclient.message_type != atomtab[MESSAGE_ENTER])
		return NO_DROP;
	dropsite = climsg->xclient.window;
	dndowner = climsg->xclient.data.l[0];
	{
		XWindowAttributes attr;
		int x, y;

		if (!XGetWindowAttributes(display, dropsite, &attr))
			return NO_DROP;
		root = attr.root;
		(void)XTranslateCoordinates(
			display, dropsite, attr.root,
			attr.x, attr.y, &x, &y, &(Window){0}
		);
		dropzone.x = x;
		dropzone.y = y;
		dropzone.width  = attr.width;
		dropzone.height = attr.height;
	}
	target = find_best_target(&climsg->xclient, targets, ntargets);
	if (callback == NULL)
		callback = no_op;
	gotposition = False;
	do_accept = False;
	action = None;
	drop = NO_DROP;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	for (;;) {
		static Time lasttime = 0;
		int nevents = xpoll(display, interval);

		if (gotposition && interval > 0) {
			savedpos.xmotion.time += timediff_msec(&now);
			if (savedpos.xmotion.time - lasttime >= interval) {
				callback(&savedpos, arg);
				lasttime = savedpos.xmotion.time;
			}
		}
		if (nevents == 0)
			continue;
		if (nevents != 1)
			return NO_DROP;

		(void)XNextEvent(display, &event);
		switch (event.type) {
		case KeyPress: case KeyRelease:
		case ButtonPress: case ButtonRelease: case MotionNotify:
		case EnterNotify: case LeaveNotify:
		case FocusIn: case FocusOut:
			XPutBackEvent(display, &event);
			send_xmessage(
				display, dndowner, atomtab[MESSAGE_STATUS],
				dropsite, 0, 0, 0, None
			);
			return NO_DROP;
		case ConfigureNotify:
			if (event.xconfigure.window == dropsite) {
				int x, y;

				(void)XTranslateCoordinates(
					display, dropsite, root,
					event.xconfigure.x, event.xconfigure.y,
					&x, &y, &(Window){0}
				);
				dropzone.x = x;
				dropzone.y = y;
				dropzone.width  = event.xconfigure.width;
				dropzone.height = event.xconfigure.height;
			}
			callback(&event, arg);
			continue;
		case ClientMessage:
			if (is_dnd_message(event.xclient.message_type))
				break;
			/* FALLTHROUGH */
		default:
			callback(&event, arg);
			continue;
		}
		if (event.type != ClientMessage)
			continue;       /* sanity check */

		/* got a dnd message */
		if (event.xclient.window != dropsite)
			continue;
		if ((Window)event.xclient.data.l[0] != dndowner)
			continue;
		if (event.xclient.message_type == atomtab[MESSAGE_POSITION]) {
			savedpos = get_motionev_from_positionmsg(
				display, root,
				&event.xclient
			);
			if (!gotposition) {
				XEvent enter;

				enter = get_crossingev_from_motionev(
					&savedpos.xmotion, EnterNotify
				);
				callback(&enter, arg);
			}
			gotposition = True;
			if (target == None) {
				do_accept = False;
				action = None;
			} else if (actions == 0) {
				do_accept = True;
				action = atomtab[ACTION_COPY];
			} else {
				do_accept = actions & get_action_const(
					event.xclient.data.l[4]
				);
				action = do_accept ? event.xclient.data.l[4] : None;
			}
			send_xmessage(
				display, dndowner, atomtab[MESSAGE_STATUS],
				dropsite,
				0x2 | (do_accept ? 0x1 : 0x0),
				((long)dropzone.x << 16) | (dropzone.y & 0xFFFF),
				((long)dropzone.width << 16) | (dropzone.height & 0xFFFF),
				action
			);
			callback(&savedpos, arg);
			continue;
		}
		if (event.xclient.message_type == atomtab[MESSAGE_LEAVE])
			break;
		if (event.xclient.message_type == atomtab[MESSAGE_DROP])
			break;
	}
	if (event.type != ClientMessage)
		return NO_DROP;         /* sanity check */

	if (gotposition) {
		XEvent leave;

		leave = get_crossingev_from_motionev(
			&savedpos.xmotion, LeaveNotify
		);
		callback(&leave, arg);
	}
	if (event.xclient.message_type != atomtab[MESSAGE_DROP])
		return NO_DROP;

	/* got drop message */
	if (do_accept) {
		drop = (struct ctrldnd_drop) {
			.window   = dropsite,
			.action   = get_action_const(action),
			.time     = event.xclient.data.l[2],
			.x        = savedpos.xmotion.x,
			.y        = savedpos.xmotion.y,
			.content = {
				.target = target,
				.data   = NULL,
				.size   = 0,
			},
		};
		drop.content.size = ctrlsel_request(
			display, drop.time,
			atomtab[SELECTION],
			target, &drop.content.data
		);
	}
	send_xmessage(
		display, dndowner, atomtab[MESSAGE_FINISHED],
		dropsite,
		do_accept ? 0x1 : 0x0,
		action,
		0, 0
	);
	return drop;
}

void
ctrldnd_announce(Display *display, Window dropsite)
{
	init(display);
	(void)XChangeProperty(
		display, dropsite,
		atomtab[AWARE], XA_ATOM, 32,
		PropModeReplace,
		(void *)&(Atom){MAX_VERSION},
		1
	);
}
