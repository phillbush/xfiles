#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xcursor/Xcursor.h>

#include "ctrlsel.h"

#define _TIMESTAMP_PROP "_TIMESTAMP_PROP"
#define TIMESTAMP       "TIMESTAMP"
#define ATOM_PAIR       "ATOM_PAIR"
#define MULTIPLE        "MULTIPLE"
#define MANAGER         "MANAGER"
#define TARGETS         "TARGETS"
#define INCR            "INCR"
#define SELDEFSIZE      0x4000
#define FLAG(f, b)      (((f) & (b)) == (b))
#define MOTION_TIME     32
#define DND_DISTANCE    8               /* distance from pointer to dnd miniwindow */
#define XDND_VERSION    5               /* XDND protocol version */
#define NCLIENTMSG_DATA 5               /* number of members on a the .data.l[] array of a XClientMessageEvent */

enum {
	CONTENT_INCR,
	CONTENT_ZERO,
	CONTENT_ERROR,
	CONTENT_SUCCESS,
};

enum {
	PAIR_TARGET,
	PAIR_PROPERTY,
	PAIR_LAST
};

enum {
	/* xdnd window properties */
	XDND_AWARE,

	/* xdnd selections */
	XDND_SELECTION,

	/* xdnd client messages */
	XDND_ENTER,
	XDND_POSITION,
	XDND_STATUS,
	XDND_LEAVE,
	XDND_DROP,
	XDND_FINISHED,

	/* xdnd actions */
	XDND_ACTION_COPY,
	XDND_ACTION_MOVE,
	XDND_ACTION_LINK,
	XDND_ACTION_ASK,
	XDND_ACTION_PRIVATE,

	XDND_ATOM_LAST,
};

enum {
	CURSOR_TARGET,
	CURSOR_PIRATE,
	CURSOR_DRAG,
	CURSOR_COPY,
	CURSOR_MOVE,
	CURSOR_LINK,
	CURSOR_NODROP,
	CURSOR_LAST,
};

struct Transfer {
	/*
 	 * When a client request the clipboard but its content is too
 	 * large, we perform incremental transfer.  We keep track of
 	 * each incremental transfer in a list of transfers.
 	 */
	struct Transfer *prev, *next;
	struct CtrlSelTarget *target;
	Window requestor;
	Atom property;
	unsigned long size;     /* how much have we transferred */
};

struct PredArg {
	CtrlSelContext *context;
	Window window;
	Atom message_type;
};

struct CtrlSelContext {
	Display *display;
	Window window;
	Atom selection;
	Time time;
	unsigned long ntargets;
	struct CtrlSelTarget *targets;

	/*
	 * Items below are used internally to keep track of any
	 * incremental transference in progress.
	 */
	unsigned long selmaxsize;
	unsigned long ndone;
	void *transfers;

	/*
	 * Items below are used internally for drag-and-dropping.
	 */
	Window dndwindow;
	unsigned int dndactions, dndresult;
};

static char *atomnames[XDND_ATOM_LAST] = {
	[XDND_AWARE]                 = "XdndAware",
	[XDND_SELECTION]             = "XdndSelection",
	[XDND_ENTER]                 = "XdndEnter",
	[XDND_POSITION]              = "XdndPosition",
	[XDND_STATUS]                = "XdndStatus",
	[XDND_LEAVE]                 = "XdndLeave",
	[XDND_DROP]                  = "XdndDrop",
	[XDND_FINISHED]              = "XdndFinished",
	[XDND_ACTION_COPY]           = "XdndActionCopy",
	[XDND_ACTION_MOVE]           = "XdndActionMove",
	[XDND_ACTION_LINK]           = "XdndActionLink",
	[XDND_ACTION_ASK]            = "XdndActionAsk",
	[XDND_ACTION_PRIVATE]        = "XdndActionPrivate",
};

static int
between(int x, int y, int x0, int y0, int w0, int h0)
{
	return x >= x0 && x < x0 + w0 && y >= y0 && y < y0 + h0;
}

static void
clientmsg(Display *dpy, Window win, Atom atom, long d[5])
{
	XEvent ev;

	ev.xclient.type = ClientMessage;
	ev.xclient.display = dpy;
	ev.xclient.serial = 0;
	ev.xclient.send_event = True;
	ev.xclient.message_type = atom;
	ev.xclient.window = win;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = d[0];
	ev.xclient.data.l[1] = d[1];
	ev.xclient.data.l[2] = d[2];
	ev.xclient.data.l[3] = d[3];
	ev.xclient.data.l[4] = d[4];
	(void)XSendEvent(dpy, win, False, 0x0, &ev);
}

static unsigned long
getselmaxsize(Display *display)
{
	unsigned long n;

	if ((n = XExtendedMaxRequestSize(display)) > 0)
		return n;
	if ((n = XMaxRequestSize(display)) > 0)
		return n;
	return SELDEFSIZE;
}

static int
getservertime(Display *display, Time *time)
{
	XEvent xev;
	Window window;
	Atom timeprop;

	/*
	 * According to ICCCM, a client wishing to acquire ownership of
	 * a selection should set the specfied time to some time between
	 * the current last-change time of the selection concerned and
	 * the current server time.
	 *
	 * Those clients should not set the time value to `CurrentTime`,
	 * because if they do so, they have no way of finding when they
	 * gained ownership of the selection.
	 *
	 * In the case that an event triggers the acquisition of the
	 * selection, this time value can be obtained from the event
	 * itself.
	 *
	 * In the case that the client must unconditionally acquire the
	 * ownership of a selection (which is our case), a zero-length
	 * append to a property is a way to obtain a timestamp for this
	 * purpose.  The timestamp is in the corresponding
	 * `PropertyNotify` event.
	 */

	if (time != CurrentTime)
		return 1;
	timeprop = XInternAtom(display, _TIMESTAMP_PROP, False);
	if (timeprop == None)
		goto error;
	window = XCreateWindow(
		display,
		DefaultRootWindow(display),
		0, 0, 1, 1, 0,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWEventMask,
		&(XSetWindowAttributes){
			.event_mask = PropertyChangeMask,
		}
	);
	if (window == None)
		goto error;
	XChangeProperty(
		display, window,
		timeprop, timeprop,
		8L, PropModeAppend, NULL, 0
	);
	while (!XWindowEvent(display, window, PropertyChangeMask, &xev)) {
		if (xev.type == PropertyNotify &&
		    xev.xproperty.window == window &&
		    xev.xproperty.atom == timeprop) {
			*time = xev.xproperty.time;
			break;
		}
	}
	(void)XDestroyWindow(display, window);
	return 1;
error:
	return 0;
}

static int
nbytes(int format)
{
	switch (format) {
	default: return sizeof(char);
	case 16: return sizeof(short);
	case 32: return sizeof(long);
	}
}

static int
getcontent(struct CtrlSelTarget *target, Display *display, Window window, Atom property)
{
	unsigned char *p, *q;
	unsigned long len, addsize, size;
	unsigned long dl;   /* dummy variable */
	int status;
	Atom incr;

	incr = XInternAtom(display, INCR, False),
	status = XGetWindowProperty(
		display,
		window,
		property,
		0L, 0x1FFFFFFF,
		True,
		AnyPropertyType,
		&target->type,
		&target->format,
		&len, &dl, &p
	);
	if (target->format != 32 && target->format != 16)
		target->format = 8;
	if (target->type == incr) {
		XFree(p);
		return CONTENT_INCR;
	}
	if (len == 0) {
		XFree(p);
		return CONTENT_ZERO;
	}
	if (status != Success) {
		XFree(p);
		return CONTENT_ERROR;
	}
	if (p == NULL) {
		XFree(p);
		return CONTENT_ERROR;
	}
	addsize = len * nbytes(target->format);
	size = addsize;
	if (target->buffer != NULL) {
		/* append buffer */
		size += target->bufsize;
		if ((q = realloc(target->buffer, size + 1)) == NULL) {
			XFree(p);
			return CONTENT_ERROR;
		}
		memcpy(q + target->bufsize, p, addsize);
		target->buffer = q;
		target->bufsize = size;
		target->nitems += len;
	} else {
		/* new buffer */
		if ((q = malloc(size + 1)) == NULL) {
			XFree(p);
			return CONTENT_ERROR;
		}
		memcpy(q, p, addsize);
		target->buffer = q;
		target->bufsize = size;
		target->nitems = len;
	}
	target->buffer[size] = '\0';
	XFree(p);
	return CONTENT_SUCCESS;
}

static void
deltransfer(CtrlSelContext *context, struct Transfer *transfer)
{
	if (transfer->prev != NULL) {
		transfer->prev->next = transfer->next;
	} else {
		context->transfers = transfer->next;
	}
	if (transfer->next != NULL) {
		transfer->next->prev = transfer->prev;
	}
}

static void
freetransferences(CtrlSelContext *context)
{
	struct Transfer *transfer;

	while (context->transfers != NULL) {
		transfer = (struct Transfer *)context->transfers;
		context->transfers = ((struct Transfer *)context->transfers)->next;
		XDeleteProperty(
			context->display,
			transfer->requestor,
			transfer->property
		);
		free(transfer);
	}
	context->transfers = NULL;
}

static void
freebuffers(CtrlSelContext *context)
{
	unsigned long i;

	for (i = 0; i < context->ntargets; i++) {
		free(context->targets[i].buffer);
		context->targets[i].buffer = NULL;
		context->targets[i].nitems = 0;
		context->targets[i].bufsize = 0;
	}
}

static unsigned long
getatomsprop(Display *display, Window window, Atom property, Atom type, Atom **atoms)
{
	unsigned char *p;
	unsigned long len;
	unsigned long dl;       /* dummy variable */
	int format;
	Atom gottype;
	unsigned long size;
	int success;

	success = XGetWindowProperty(
		display,
		window,
		property,
		0L, 0x1FFFFFFF,
		False,
		type, &gottype,
		&format, &len,
		&dl, &p
	);
	if (success != Success || len == 0 || p == NULL || format != 32)
		goto error;
	if (type != AnyPropertyType && type != gottype)
		goto error;
	size = len * sizeof(**atoms);
	if ((*atoms = malloc(size)) == NULL)
		goto error;
	memcpy(*atoms, p, size);
	XFree(p);
	return len;
error:
	XFree(p);
	*atoms = NULL;
	return 0;
}

static int
newtransfer(CtrlSelContext *context, struct CtrlSelTarget *target, Window requestor, Atom property)
{
	struct Transfer *transfer;

	transfer = malloc(sizeof(*transfer));
	if (transfer == NULL)
		return 0;
	*transfer = (struct Transfer){
		.prev = NULL,
		.next = (struct Transfer *)context->transfers,
		.requestor = requestor,
		.property = property,
		.target = target,
		.size = 0,
	};
	if (context->transfers != NULL)
		((struct Transfer *)context->transfers)->prev = transfer;
	context->transfers = transfer;
	return 1;
}

static Bool
convert(CtrlSelContext *context, Window requestor, Atom target, Atom property)
{
	Atom multiple, timestamp, targets, incr;
	Atom *supported;
	unsigned long i;
	int nsupported;

	incr = XInternAtom(context->display, INCR, False);
	targets = XInternAtom(context->display, TARGETS, False);
	multiple = XInternAtom(context->display, MULTIPLE, False);
	timestamp = XInternAtom(context->display, TIMESTAMP, False);
	if (target == multiple) {
		/* A MULTIPLE should be handled when processing a
		 * SelectionRequest event.  We do not support nested
		 * MULTIPLE targets.
		 */
		return False;
	}
	if (target == timestamp) {
		/*
		 * According to ICCCM, to avoid some race conditions, it
		 * is important that requestors be able to discover the
		 * timestamp the owner used to acquire ownership.
		 * Requestors do that by requesting selection owners to
		 * convert the `TIMESTAMP` target.  Selection owners
		 * must return the timestamp as an `XA_INTEGER`.
		 */
		XChangeProperty(
			context->display,
			requestor,
			property,
			XA_INTEGER, 32,
			PropModeReplace,
			(unsigned char *)&context->time,
			1
		);
		return True;
	}
	if (target == targets) {
		/*
		 * According to ICCCM, when requested for the `TARGETS`
		 * target, the selection owner should return a list of
		 * atoms representing the targets for which an attempt
		 * to convert the selection will (hopefully) succeed.
		 */
		nsupported = context->ntargets + 2;     /* +2 for MULTIPLE + TIMESTAMP */
		if ((supported = calloc(nsupported, sizeof(*supported))) == NULL)
			return False;
		for (i = 0; i < context->ntargets; i++) {
			supported[i] = context->targets[i].target;
		}
		supported[i++] = multiple;
		supported[i++] = timestamp;
		XChangeProperty(
			context->display,
			requestor,
			property,
			XA_ATOM, 32,
			PropModeReplace,
			(unsigned char *)supported,
			nsupported
		);
		free(supported);
		return True;
	}
	for (i = 0; i < context->ntargets; i++) {
		if (target == context->targets[i].target)
			goto found;
	}
	return False;
found:
	if (context->targets[i].bufsize > context->selmaxsize) {
		XSelectInput(
			context->display,
			requestor,
			StructureNotifyMask | PropertyChangeMask
		);
		XChangeProperty(
			context->display,
			requestor,
			property,
			incr,
			32L,
			PropModeReplace,
			(unsigned char *)context->targets[i].buffer,
			1
		);
		newtransfer(context, &context->targets[i], requestor, property);
	} else {
		XChangeProperty(
			context->display,
			requestor,
			property,
			target,
			context->targets[i].format,
			PropModeReplace,
			context->targets[i].buffer,
			context->targets[i].nitems
		);
	}
	return True;
}

static int
request(CtrlSelContext *context)
{
	Atom multiple, atom_pair;
	Atom *pairs;
	unsigned long i, size;

	for (i = 0; i < context->ntargets; i++) {
		context->targets[i].nitems = 0;
		context->targets[i].bufsize = 0;
		context->targets[i].buffer = NULL;
	}
	if (context->ntargets == 1) {
		(void)XConvertSelection(
			context->display,
			context->selection,
			context->targets[0].target,
			context->targets[0].target,
			context->window,
			context->time
		);
	} else if (context->ntargets > 1) {
		multiple = XInternAtom(context->display, MULTIPLE, False);
		atom_pair = XInternAtom(context->display, ATOM_PAIR, False);
		size = 2 * context->ntargets;
		pairs = calloc(size, sizeof(*pairs));
		if (pairs == NULL)
			return 0;
		for (i = 0; i < context->ntargets; i++) {
			pairs[i * 2 + 0] = context->targets[i].target;
			pairs[i * 2 + 1] = context->targets[i].target;
		}
		(void)XChangeProperty(
			context->display,
			context->window,
			multiple,
			atom_pair,
			32,
			PropModeReplace,
			(unsigned char *)pairs,
			size
		);
		(void)XConvertSelection(
			context->display,
			context->selection,
			multiple,
			multiple,
			context->window,
			context->time
		);
		free(pairs);
	}
	return 1;
}

void
ctrlsel_filltarget(
	Atom target,
	Atom type,
	int format,
	unsigned char *buffer,
	unsigned long size,
	struct CtrlSelTarget *fill
) {
	if (fill == NULL)
		return;
	if (format != 32 && format != 16)
		format = 8;
	*fill = (struct CtrlSelTarget){
		.target = target,
		.type = type,
		.action = None,
		.format = format,
		.nitems = size / nbytes(format),
		.buffer = buffer,
		.bufsize = size,
	};
}

CtrlSelContext *
ctrlsel_request(
	Display *display,
	Window window,
	Atom selection,
	Time time,
	struct CtrlSelTarget targets[],
	unsigned long ntargets
) {
	CtrlSelContext *context;

	if (!getservertime(display, &time))
		return NULL;
	if ((context = malloc(sizeof(*context))) == NULL)
		return NULL;
	*context = (CtrlSelContext){
		.display = display,
		.window = window,
		.selection = selection,
		.time = time,
		.targets = targets,
		.ntargets = ntargets,
		.selmaxsize = getselmaxsize(display),
		.ndone = 0,
		.transfers = NULL,
		.dndwindow = None,
		.dndactions = 0x00,
		.dndresult = 0x00,
	};
	if (ntargets == 0)
		return context;
	if (request(context))
		return context;
	free(context);
	return NULL;
}

CtrlSelContext *
ctrlsel_setowner(
	Display *display,
	Window window,
	Atom selection,
	Time time,
	int ismanager,
	struct CtrlSelTarget targets[],
	unsigned long ntargets
) {
	CtrlSelContext *context;
	Window root;

	root = DefaultRootWindow(display);
	if (!getservertime(display, &time))
		return NULL;
	if ((context = malloc(sizeof(*context))) == NULL)
		return NULL;
	*context = (CtrlSelContext){
		.display = display,
		.window = window,
		.selection = selection,
		.time = time,
		.targets = targets,
		.ntargets = ntargets,
		.selmaxsize = getselmaxsize(display),
		.ndone = 0,
		.transfers = NULL,
		.dndwindow = None,
		.dndactions = 0x00,
		.dndresult = 0x00,
	};
	(void)XSetSelectionOwner(display, selection, window, time);
	if (XGetSelectionOwner(display, selection) != window) {
		free(context);
		return NULL;
	}
	if (!ismanager)
		return context;

	/*
	 * According to ICCCM, a manager client (that is, a client
	 * responsible for managing shared resources) should take
	 * ownership of an appropriate selection.
	 *
	 * Immediately after a manager successfully acquires ownership
	 * of a manager selection, it should announce its arrival by
	 * sending a `ClientMessage` event.  (That is necessary for
	 * clients to be able to know when a specific manager has
	 * started: any client that wish to do so should select for
	 * `StructureNotify` on the root window and should watch for
	 * the appropriate `MANAGER` `ClientMessage`).
	 */
	(void)XSendEvent(
		display,
		root,
		False,
		StructureNotifyMask,
		(XEvent *)&(XClientMessageEvent){
			.type         = ClientMessage,
			.window       = root,
			.message_type = XInternAtom(display, MANAGER, False),
			.format       = 32,
			.data.l[0]    = time,           /* timestamp */
			.data.l[1]    = selection,      /* manager selection atom */
			.data.l[2]    = window,         /* window owning the selection */
			.data.l[3]    = 0,              /* manager-specific data */
			.data.l[4]    = 0,              /* manager-specific data */
		}
	);
	return context;
}

static int
receiveinit(CtrlSelContext *context, XEvent *xev)
{
	struct CtrlSelTarget *targetp;
	XSelectionEvent *xselev;
	Atom multiple, atom_pair;
	Atom *pairs;
	Atom pair[PAIR_LAST];
	unsigned long j, natoms;
	unsigned long i;
	int status, success;

	multiple = XInternAtom(context->display, MULTIPLE, False);
	atom_pair = XInternAtom(context->display, ATOM_PAIR, False);
	xselev = &xev->xselection;
	if (xselev->selection != context->selection)
		return CTRLSEL_NONE;
	if (xselev->requestor != context->window)
		return CTRLSEL_NONE;
	if (xselev->property == None)
		return CTRLSEL_ERROR;
	if (xselev->target == multiple) {
		natoms = getatomsprop(
			xselev->display,
			xselev->requestor,
			xselev->property,
			atom_pair,
			&pairs
		);
		if (natoms == 0 || pairs == NULL) {
			free(pairs);
			return CTRLSEL_ERROR;
		}
	} else {
		pair[PAIR_TARGET] = xselev->target;
		pair[PAIR_PROPERTY] = xselev->property;
		pairs = pair;
		natoms = 2;
	}
	success = 1;
	for (j = 0; j < natoms; j += 2) {
		targetp = NULL;
		for (i = 0; i < context->ntargets; i++) {
			if (pairs[j + PAIR_TARGET] == context->targets[i].target) {
				targetp = &context->targets[i];
				break;
			}
		}
		if (pairs[j + PAIR_PROPERTY] == None)
			pairs[j + PAIR_PROPERTY] = pairs[j + PAIR_TARGET];
		if (targetp == NULL) {
			success = 0;
			continue;
		}
		status = getcontent(
			targetp,
			xselev->display,
			xselev->requestor,
			pairs[j + PAIR_PROPERTY]
		);
		switch (status) {
		case CONTENT_ERROR:
			success = 0;
			break;
		case CONTENT_SUCCESS:
			/* fallthrough */
		case CONTENT_ZERO:
			context->ndone++;
			break;
		case CONTENT_INCR:
			if (!newtransfer(context, targetp, xselev->requestor, pairs[j + PAIR_PROPERTY]))
				success = 0;
			break;
		}
	}
	if (xselev->target == multiple)
		free(pairs);
	return success ? CTRLSEL_INTERNAL : CTRLSEL_ERROR;
}

static int
receiveincr(CtrlSelContext *context, XEvent *xev)
{
	struct Transfer *transfer;
	XPropertyEvent *xpropev;
	int status;

	xpropev = &xev->xproperty;
	if (xpropev->state != PropertyNewValue)
		return CTRLSEL_NONE;
	if (xpropev->window != context->window)
		return CTRLSEL_NONE;
	for (transfer = (struct Transfer *)context->transfers; transfer != NULL; transfer = transfer->next)
		if (transfer->property == xpropev->atom)
			goto found;
	return CTRLSEL_NONE;
found:
	status = getcontent(
		transfer->target,
		xpropev->display,
		xpropev->window,
		xpropev->atom
	);
	switch (status) {
	case CONTENT_ERROR:
	case CONTENT_INCR:
		return CTRLSEL_ERROR;
	case CONTENT_SUCCESS:
		return CTRLSEL_INTERNAL;
	case CONTENT_ZERO:
		context->ndone++;
		deltransfer(context, transfer);
		break;
	}
	return CTRLSEL_INTERNAL;
}

int
ctrlsel_receive(CtrlSelContext *context, XEvent *xev)
{
	int status;

	if (xev->type == SelectionNotify)
		status = receiveinit(context, xev);
	else if (xev->type == PropertyNotify)
		status = receiveincr(context, xev);
	else
		return CTRLSEL_NONE;
	if (status == CTRLSEL_INTERNAL) {
		if (context->ndone >= context->ntargets) {
			status = CTRLSEL_RECEIVED;
			goto done;
		}
	} else if (status == CTRLSEL_ERROR) {
		freebuffers(context);
		freetransferences(context);
	}
done:
	if (status == CTRLSEL_RECEIVED)
		freetransferences(context);
	return status;
}

static int
sendinit(CtrlSelContext *context, XEvent *xev)
{
	XSelectionRequestEvent *xreqev;
	XSelectionEvent xselev;
	unsigned long natoms, i;
	Atom *pairs;
	Atom pair[PAIR_LAST];
	Atom multiple, atom_pair;
	Bool success;

	xreqev = &xev->xselectionrequest;
	if (xreqev->selection != context->selection)
		return CTRLSEL_NONE;
	multiple = XInternAtom(context->display, MULTIPLE, False);
	atom_pair = XInternAtom(context->display, ATOM_PAIR, False);
	xselev = (XSelectionEvent){
		.type           = SelectionNotify,
		.display        = xreqev->display,
		.requestor      = xreqev->requestor,
		.selection      = xreqev->selection,
		.time           = xreqev->time,
		.target         = xreqev->target,
		.property       = None,
	};
	if (xreqev->time != CurrentTime && xreqev->time < context->time) {
		/*
		 * According to ICCCM, the selection owner
		 * should compare the timestamp with the period
		 * it has owned the selection and, if the time
		 * is outside, refuse the `SelectionRequest` by
		 * sending the requestor window a
		 * `SelectionNotify` event with the property set
		 * to `None` (by means of a `SendEvent` request
		 * with an empty event mask).
		 */
		goto done;
	}
	if (xreqev->target == multiple) {
		if (xreqev->property == None)
			goto done;
		natoms = getatomsprop(
			xreqev->display,
			xreqev->requestor,
			xreqev->property,
			atom_pair,
			&pairs
		);
	} else {
		pair[PAIR_TARGET] = xreqev->target;
		pair[PAIR_PROPERTY] = xreqev->property;
		pairs = pair;
		natoms = 2;
	}
	success = True;
	for (i = 0; i < natoms; i += 2) {
		if (!convert(context, xreqev->requestor,
		             pairs[i + PAIR_TARGET],
		             pairs[i + PAIR_PROPERTY])) {
			success = False;
			pairs[i + PAIR_PROPERTY] = None;
		}
	}
	if (xreqev->target == multiple) {
		XChangeProperty(
			xreqev->display,
			xreqev->requestor,
			xreqev->property,
			atom_pair,
			32, PropModeReplace,
			(unsigned char *)pairs,
			natoms
		);
		free(pairs);
	}
	if (success) {
		if (xreqev->property == None) {
			xselev.property = xreqev->target;
		} else {
			xselev.property = xreqev->property;
		}
	}
done:
	XSendEvent(
		xreqev->display,
		xreqev->requestor,
		False,
		NoEventMask,
		(XEvent *)&xselev
	);
	return CTRLSEL_INTERNAL;
}

static int
sendlost(CtrlSelContext *context, XEvent *xev)
{
	XSelectionClearEvent *xclearev;

	xclearev = &xev->xselectionclear;
	if (xclearev->selection == context->selection &&
	    xclearev->window == context->window) {
		return CTRLSEL_LOST;
	}
	return CTRLSEL_NONE;
}

static int
senddestroy(CtrlSelContext *context, XEvent *xev)
{
	struct Transfer *transfer;
	XDestroyWindowEvent *xdestroyev;

	xdestroyev = &xev->xdestroywindow;
	for (transfer = context->transfers; transfer != NULL; transfer = transfer->next)
		if (transfer->requestor == xdestroyev->window)
			deltransfer(context, transfer);
	return CTRLSEL_NONE;
}

static int
sendincr(CtrlSelContext *context, XEvent *xev)
{
	struct Transfer *transfer;
	XPropertyEvent *xpropev;
	unsigned long size;

	xpropev = &xev->xproperty;
	if (xpropev->state != PropertyDelete)
		return CTRLSEL_NONE;
	for (transfer = context->transfers; transfer != NULL; transfer = transfer->next)
		if (transfer->property == xpropev->atom &&
		    transfer->requestor == xpropev->window)
			goto found;
	return CTRLSEL_NONE;
found:
	if (transfer->size >= transfer->target->bufsize)
		transfer->size = transfer->target->bufsize;
	size = transfer->target->bufsize - transfer->size;
	if (size > context->selmaxsize)
		size = context->selmaxsize;
	XChangeProperty(
		xpropev->display,
		xpropev->window,
		xpropev->atom,
		transfer->target->target,
		transfer->target->format,
		PropModeReplace,
		transfer->target->buffer + transfer->size,
		size / nbytes(transfer->target->format)
	);
	if (transfer->size >= transfer->target->bufsize) {
		deltransfer(context, transfer);
	} else {
		transfer->size += size;
	}
	return CTRLSEL_INTERNAL;
}

int
ctrlsel_send(CtrlSelContext *context, XEvent *xev)
{
	int status;

	if (xev->type == SelectionRequest)
		status = sendinit(context, xev);
	else if (xev->type == SelectionClear)
		status = sendlost(context, xev);
	else if (xev->type == DestroyNotify)
		status = senddestroy(context, xev);
	else if (xev->type == PropertyNotify)
		status = sendincr(context, xev);
	else
		return CTRLSEL_NONE;
	if (status == CTRLSEL_LOST || status == CTRLSEL_ERROR) {
		status = CTRLSEL_LOST;
		freetransferences(context);
	}
	return status;
}

void
ctrlsel_cancel(CtrlSelContext *context)
{
	if (context == NULL)
		return;
	freebuffers(context);
	freetransferences(context);
	free(context);
}

void
ctrlsel_disown(CtrlSelContext *context)
{
	if (context == NULL)
		return;
	freetransferences(context);
	free(context);
}

static Bool
dndpred(Display *display, XEvent *event, XPointer p)
{
	struct PredArg *arg;
	struct Transfer *transfer;

	arg = (struct PredArg *)p;
	switch (event->type) {
	case KeyPress:
	case KeyRelease:
		if (event->xkey.display == display &&
		    event->xkey.window == arg->window)
			return True;
		break;
	case ButtonPress:
	case ButtonRelease:
		if (event->xbutton.display == display &&
		    event->xbutton.window == arg->window)
			return True;
		break;
	case MotionNotify:
		if (event->xmotion.display == display &&
		    event->xmotion.window == arg->window)
			return True;
		break;
	case DestroyNotify:
		if (event->xdestroywindow.display == display &&
		    event->xdestroywindow.window == arg->window)
			return True;
		break;
	case UnmapNotify:
		if (event->xunmap.display == display &&
		    event->xunmap.window == arg->window)
			return True;
		break;
	case SelectionClear:
		if (event->xselectionclear.display == display &&
		    event->xselectionclear.window == arg->window)
			return True;
		break;
	case SelectionRequest:
		if (event->xselectionrequest.display == display &&
		    event->xselectionrequest.owner == arg->window)
			return True;
		break;
	case ClientMessage:
		if (event->xclient.display == display &&
		    event->xclient.window == arg->window &&
		    event->xclient.message_type == arg->message_type)
			return True;
		break;
	case PropertyNotify:
		if (event->xproperty.display != display ||
		    event->xproperty.state != PropertyDelete)
			return False;
		for (transfer = arg->context->transfers;
		     transfer != NULL;
		     transfer = transfer->next) {
			if (transfer->property == event->xproperty.atom &&
			    transfer->requestor == event->xproperty.window) {
				return True;
			}
		}
		break;
	default:
		break;
	}
	return False;
}

#define SOME(a, b, c)      ((a) != None ? (a) : ((b) != None ? (b) : (c)))

static Cursor
getcursor(Cursor cursors[CURSOR_LAST], int type)
{
	switch (type) {
	case CURSOR_TARGET:
	case CURSOR_DRAG:
		return SOME(cursors[CURSOR_DRAG], cursors[CURSOR_TARGET], None);
	case CURSOR_PIRATE:
	case CURSOR_NODROP:
		return SOME(cursors[CURSOR_NODROP], cursors[CURSOR_PIRATE], None);
	case CURSOR_COPY:
		return SOME(cursors[CURSOR_COPY], cursors[CURSOR_DRAG], cursors[CURSOR_TARGET]);
	case CURSOR_MOVE:
		return SOME(cursors[CURSOR_MOVE], cursors[CURSOR_DRAG], cursors[CURSOR_TARGET]);
	case CURSOR_LINK:
		return SOME(cursors[CURSOR_LINK], cursors[CURSOR_DRAG], cursors[CURSOR_TARGET]);
	};
	return None;
}

static void
initcursors(Display *display, Cursor cursors[CURSOR_LAST])
{
	cursors[CURSOR_TARGET] = XCreateFontCursor(display, XC_target);
	cursors[CURSOR_PIRATE] = XCreateFontCursor(display, XC_pirate);
	cursors[CURSOR_DRAG] = XcursorLibraryLoadCursor(display, "dnd-none");
	cursors[CURSOR_COPY] = XcursorLibraryLoadCursor(display, "dnd-copy");
	cursors[CURSOR_MOVE] = XcursorLibraryLoadCursor(display, "dnd-move");
	cursors[CURSOR_LINK] = XcursorLibraryLoadCursor(display, "dnd-link");
	cursors[CURSOR_NODROP] = XcursorLibraryLoadCursor(display, "forbidden");
}

static void
freecursors(Display *display, Cursor cursors[CURSOR_LAST])
{
	int i;

	for (i = 0; i < CURSOR_LAST; i++) {
		if (cursors[i] != None) {
			XFreeCursor(display, cursors[i]);
		}
	}
}

static int
querypointer(Display *display, Window window, int *retx, int *rety, Window *retwin)
{
	Window root, child;
	unsigned int mask;
	int rootx, rooty;
	int x, y;
	int retval;

	retval = XQueryPointer(
		display,
		window,
		&root, &child,
		&rootx, &rooty,
		&x, &y,
		&mask
	);
	if (retwin != NULL)
		*retwin = child;
	if (retx != NULL)
		*retx = x;
	if (rety != NULL)
		*rety = y;
	return retval;
}

static Window
getdndwindowbelow(Display *display, Window root, Atom aware, Atom *version)
{
	Atom *p;
	Window window;

	/*
	 * Query pointer location and return the window below it,
	 * and the version of the XDND protocol it uses.
	 */
	*version = None;
	window = root;
	p = NULL;
	while (querypointer(display, window, NULL, NULL, &window)) {
		if (window == None)
			break;
		p = NULL;
		if (getatomsprop(display, window, aware, AnyPropertyType, &p) > 0) {
			*version = *p;
			XFree(p);
			return window;
		}
	}
	XFree(p);
	return None;
}

CtrlSelContext *
ctrlsel_dndwatch(
	Display *display,
	Window window,
	unsigned int actions,
	struct CtrlSelTarget targets[],
	unsigned long ntargets
) {
	CtrlSelContext *context;
	Atom version = XDND_VERSION;    /* yes, version is an Atom */
	Atom xdndaware, xdndselection;

	xdndaware = XInternAtom(display, atomnames[XDND_AWARE], False);
	if (xdndaware == None)
		return NULL;
	xdndselection = XInternAtom(display, atomnames[XDND_SELECTION], False);
	if (xdndselection == None)
		return NULL;
	if ((context = malloc(sizeof(*context))) == NULL)
		return NULL;
	*context = (CtrlSelContext){
		.display = display,
		.window = window,
		.selection = xdndselection,
		.time = CurrentTime,
		.targets = targets,
		.ntargets = ntargets,
		.selmaxsize = getselmaxsize(display),
		.ndone = 0,
		.transfers = NULL,
		.dndwindow = None,
		.dndactions = actions,
		.dndresult = 0x00,
	};
	(void)XChangeProperty(
		display,
		window,
		xdndaware,
		XA_ATOM, 32,
		PropModeReplace,
		(unsigned char *)&version,
		1
	);
	return context;
}

static void
finishdrop(CtrlSelContext *context)
{
	long d[NCLIENTMSG_DATA];
	unsigned long i;
	Atom finished;

	if (context->dndwindow == None)
		return;
	finished = XInternAtom(context->display, atomnames[XDND_FINISHED], False);
	if (finished == None)
		return;
	for (i = 0; i < context->ntargets; i++)
		context->targets[i].action = context->dndresult;
	d[0] = context->window;
	d[1] = d[2] = d[3] = d[4] = 0;
	clientmsg(context->display, context->dndwindow, finished, d);
	context->dndwindow = None;
}

int
ctrlsel_dndreceive(CtrlSelContext *context, XEvent *event)
{
	Atom atoms[XDND_ATOM_LAST];
	Atom action;
	long d[NCLIENTMSG_DATA];
	
	if (!XInternAtoms(context->display, atomnames, XDND_ATOM_LAST, False, atoms))
		return CTRLSEL_NONE;
	switch (ctrlsel_receive(context, event)) {
	case CTRLSEL_RECEIVED:
		finishdrop(context);
		return CTRLSEL_RECEIVED;
	case CTRLSEL_INTERNAL:
	case CTRLSEL_ERROR:
		return CTRLSEL_INTERNAL;
	default:
		break;
	}
	if (event->type != ClientMessage)
		return CTRLSEL_NONE;
	if (event->xclient.message_type == atoms[XDND_ENTER]) {
		context->dndwindow = (Window)event->xclient.data.l[0];
		context->dndresult = 0x00;
	} else if (event->xclient.message_type == atoms[XDND_LEAVE]) {
		if ((Window)event->xclient.data.l[0] == None ||
		    (Window)event->xclient.data.l[0] != context->dndwindow)
			return CTRLSEL_NONE;
		context->dndwindow = None;
	} else if (event->xclient.message_type == atoms[XDND_DROP]) {
		if ((Window)event->xclient.data.l[0] == None ||
		    (Window)event->xclient.data.l[0] != context->dndwindow)
			return CTRLSEL_NONE;
		context->time = (Time)event->xclient.data.l[2];
		(void)request(context);
	} else if (event->xclient.message_type == atoms[XDND_POSITION]) {
		if ((Window)event->xclient.data.l[0] == None ||
		    (Window)event->xclient.data.l[0] != context->dndwindow)
			return CTRLSEL_NONE;
		if (((Atom)event->xclient.data.l[4] == atoms[XDND_ACTION_COPY] &&
		     context->dndactions & CTRLSEL_COPY) ||
		    ((Atom)event->xclient.data.l[4] == atoms[XDND_ACTION_MOVE] &&
		     context->dndactions & CTRLSEL_MOVE) ||
		    ((Atom)event->xclient.data.l[4] == atoms[XDND_ACTION_LINK] &&
		     context->dndactions & CTRLSEL_LINK) ||
		    ((Atom)event->xclient.data.l[4] == atoms[XDND_ACTION_ASK] &&
		     context->dndactions & CTRLSEL_ASK) ||
		    ((Atom)event->xclient.data.l[4] == atoms[XDND_ACTION_PRIVATE] &&
		     context->dndactions & CTRLSEL_PRIVATE)) {
			action = (Atom)event->xclient.data.l[4];
		} else {
			action = atoms[XDND_ACTION_COPY];
		}
		d[0] = context->window;
		d[1] = 0x1;
		d[2] = 0;               /* our rectangle is the entire screen */
		d[3] = 0xFFFFFFFF;      /* so we do not get lots of messages */
		d[4] = action;
		if (action == atoms[XDND_ACTION_PRIVATE])
			context->dndresult = CTRLSEL_PRIVATE;
		else if (action == atoms[XDND_ACTION_ASK])
			context->dndresult = CTRLSEL_ASK;
		else if (action == atoms[XDND_ACTION_LINK])
			context->dndresult = CTRLSEL_LINK;
		else if (action == atoms[XDND_ACTION_MOVE])
			context->dndresult = CTRLSEL_MOVE;
		else
			context->dndresult = CTRLSEL_COPY;
		clientmsg(
			context->display,
			(Window)event->xclient.data.l[0],
			atoms[XDND_STATUS],
			d
		);
	} else {
		return CTRLSEL_NONE;
	}
	return CTRLSEL_INTERNAL;
}

void
ctrlsel_dndclose(CtrlSelContext *context)
{
	if (context == NULL)
		return;
	finishdrop(context);
	freebuffers(context);
	freetransferences(context);
	free(context);
}

void
ctrlsel_dnddisown(CtrlSelContext *context)
{
	ctrlsel_disown(context);
}

int
ctrlsel_dndsend(CtrlSelContext *context, XEvent *event)
{
	Atom finished;

	finished = XInternAtom(context->display, atomnames[XDND_FINISHED], False);
	if (event->type == ClientMessage &&
	    event->xclient.message_type == finished &&
	    (Window)event->xclient.data.l[0] == context->dndwindow) {
		ctrlsel_dnddisown(context);
		return CTRLSEL_SENT;
	}
	return ctrlsel_send(context, event);
}

int
ctrlsel_dndown(
	Display *display,
	Window window,
	Window miniature,
	Time time,
	struct CtrlSelTarget targets[],
	unsigned long ntargets,
	CtrlSelContext **context_ret
) {
	CtrlSelContext *context;
	struct PredArg arg;
	XWindowAttributes wattr;
	XEvent event;
	Atom atoms[XDND_ATOM_LAST];
	Cursor cursors[CURSOR_LAST] = { None, None };
	Cursor cursor;
	Window lastwin, winbelow;
	Atom lastaction, action, version;
	long d[NCLIENTMSG_DATA];
	int sendposition, retval, status, inside;
	int x, y, w, h;

	*context_ret = NULL;
	if (display == NULL || window == None)
		return CTRLSEL_ERROR;
	if (!XGetWindowAttributes(display, window, &wattr))
		return CTRLSEL_ERROR;
	if ((wattr.your_event_mask & StructureNotifyMask) == 0x00)
		return CTRLSEL_ERROR;
	if (wattr.map_state != IsViewable)
		return CTRLSEL_ERROR;
	if (!XInternAtoms(display, atomnames, XDND_ATOM_LAST, False, atoms))
		return CTRLSEL_ERROR;
	context = ctrlsel_setowner(
		display,
		window,
		atoms[XDND_SELECTION],
		time,
		0,
		targets,
		ntargets
	);
	if (context == NULL)
		return CTRLSEL_ERROR;
	d[0] = window;
	sendposition = 1;
	x = y = w = h = 0;
	retval = CTRLSEL_ERROR;
	lastaction = action = None;
	lastwin = None;
	arg = (struct PredArg){
		.context = context,
		.window = window,
		.message_type = atoms[XDND_STATUS],
	};
	initcursors(display, cursors);
	status = XGrabPointer(
		display,
		window,
		True,
		ButtonPressMask | ButtonMotionMask |
		ButtonReleaseMask | PointerMotionMask,
		GrabModeAsync,
		GrabModeAsync,
		None,
		None,
		time
	);
	if (status != GrabSuccess)
		goto done;
	status = XGrabKeyboard(
		display,
		window,
		True,
		GrabModeAsync,
		GrabModeAsync,
		time
	);
	if (status != GrabSuccess)
		goto done;
	if (miniature != None)
		XMapRaised(display, miniature);
	cursor = getcursor(cursors, CURSOR_DRAG);
	for (;;) {
		(void)XIfEvent(display, &event, &dndpred, (XPointer)&arg);
		switch (ctrlsel_send(context, &event)) {
		case CTRLSEL_LOST:
			retval = CTRLSEL_NONE;
			goto done;
		case CTRLSEL_INTERNAL:
			continue;
		default:
			break;
		}
		switch (event.type) {
		case KeyPress:
		case KeyRelease:
			if (event.xkey.keycode != 0 &&
			    event.xkey.keycode == XKeysymToKeycode(display, XK_Escape)) {
				retval = CTRLSEL_NONE;
				goto done;
			}
			break;
		case ButtonPress:
		case ButtonRelease:
			if (lastwin == None) {
				retval = CTRLSEL_NONE;
			} else if (lastwin == window) {
				retval = CTRLSEL_DROPSELF;
			} else {
				retval = CTRLSEL_DROPOTHER;
				d[1] = d[3] = d[4] = 0;
				d[2] = event.xbutton.time;
				clientmsg(display, lastwin, atoms[XDND_DROP], d);
				context->dndwindow = lastwin;
			}
			goto done;
		case MotionNotify:
			if (event.xmotion.time - time < MOTION_TIME)
				break;
			if (miniature != None) {
				XMoveWindow(
					display,
					miniature,
					event.xmotion.x_root + DND_DISTANCE,
					event.xmotion.y_root + DND_DISTANCE
				);
			}
			inside = between(event.xmotion.x, event.xmotion.y, x, y, w, h);
			if ((lastaction != action || sendposition || !inside)
			    && lastwin != None) {
				if (lastaction != None)
					d[4] = lastaction;
				else if (FLAG(event.xmotion.state, ControlMask|ShiftMask))
					d[4] = atoms[XDND_ACTION_LINK];
				else if (FLAG(event.xmotion.state, ShiftMask))
					d[4] = atoms[XDND_ACTION_MOVE];
				else if (FLAG(event.xmotion.state, ControlMask))
					d[4] = atoms[XDND_ACTION_COPY];
				else
					d[4] = atoms[XDND_ACTION_ASK];
				d[1] = 0;
				d[2] = event.xmotion.x_root << 16;
				d[2] |= event.xmotion.y_root & 0xFFFF;
				d[3] = event.xmotion.time;
				clientmsg(display, lastwin, atoms[XDND_POSITION], d);
				sendposition = 1;
			}
			time = event.xmotion.time;
			lastaction = action;
			winbelow = getdndwindowbelow(display, wattr.root, atoms[XDND_AWARE], &version);
			if (winbelow == lastwin)
				break;
			sendposition = 1;
			x = y = w = h = 0;
			if (version > XDND_VERSION)
				version = XDND_VERSION;
			if (lastwin != None && lastwin != window) {
				d[1] = d[2] = d[3] = d[4] = 0;
				clientmsg(display, lastwin, atoms[XDND_LEAVE], d);
			}
			if (winbelow != None && winbelow != window) {
				d[1] = version;
				d[1] <<= 24;
				d[2] = ntargets > 0 ? targets[0].target : None;
				d[3] = ntargets > 1 ? targets[1].target : None;
				d[4] = ntargets > 2 ? targets[2].target : None;
				clientmsg(display, winbelow, atoms[XDND_ENTER], d);
			}
			if (winbelow == None)
				cursor = getcursor(cursors, CURSOR_NODROP);
			else if (FLAG(event.xmotion.state, ControlMask|ShiftMask))
				cursor = getcursor(cursors, CURSOR_LINK);
			else if (FLAG(event.xmotion.state, ShiftMask))
				cursor = getcursor(cursors, CURSOR_MOVE);
			else if (FLAG(event.xmotion.state, ControlMask))
				cursor = getcursor(cursors, CURSOR_COPY);
			else
				cursor = getcursor(cursors, CURSOR_DRAG);
			XDefineCursor(display, window, cursor);
			lastwin = winbelow;
			lastaction = action = None;
			break;
		case ClientMessage:
			if ((Window)event.xclient.data.l[0] != lastwin)
				break;
			sendposition = (event.xclient.data.l[1] & 0x02);
			if (event.xclient.data.l[1] & 0x01)
				XDefineCursor(display, window, cursor);
			else
				XDefineCursor(display, window, getcursor(cursors, CURSOR_NODROP));
			x = event.xclient.data.l[2] >> 16;
			y = event.xclient.data.l[2] & 0xFFF;
			w = event.xclient.data.l[3] >> 16;
			h = event.xclient.data.l[3] & 0xFFF;
			if ((Atom)event.xclient.data.l[4] != None)
				action = (Atom)event.xclient.data.l[4];
			else
				action = atoms[XDND_ACTION_COPY];
			break;
		case DestroyNotify:
		case UnmapNotify:
			XPutBackEvent(display, &event);
			retval = CTRLSEL_ERROR;
			goto done;
		default:
			break;
		}
	}
done:
	XUndefineCursor(display, window);
	if (miniature != None)
		XUnmapWindow(display, miniature);
	XUngrabPointer(display, CurrentTime);
	XUngrabKeyboard(display, CurrentTime);
	freecursors(display, cursors);
	if (retval != CTRLSEL_DROPOTHER) {
		ctrlsel_dnddisown(context);
		context = NULL;
	}
	*context_ret = context;
	return retval;
}
