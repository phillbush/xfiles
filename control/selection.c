#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <control/selection.h>

#define ENUM(sym) sym,
#define NAME(sym) #sym,

#define ATOMS(X)                                          \
	X(TIMESTAMP)                                      \
	X(TARGETS)                                        \
	X(MULTIPLE)                                       \
	X(INCR)                                           \
	X(ATOM_PAIR)

#define SYNC_NTRIES 120 /* iterate at most this times */
#define SYNC_WAIT   6   /* wait at most this milliseconds between each try */
#define SYNC_EVENT(dpy, win, type, ev) for ( \
	int SYNC_TRY = 0; \
	SYNC_TRY < SYNC_NTRIES; \
	SYNC_TRY++, poll(&(struct pollfd){ \
		.fd = XConnectionNumber((dpy)), \
		.events = POLLIN, \
	}, 1, SYNC_WAIT) \
) while (XCheckTypedWindowEvent((dpy), (win), (type), (ev)))

enum errors {
	/* ctrlsel(3) functions returns negative error codes on error */
	CTRL_NOERROR    = 0,
	CTRL_ENOMEM     = -ENOMEM,
	CTRL_ETIMEDOUT  = -ETIMEDOUT,
	CTRL_EMSGSIZE   = -EMSGSIZE,
};

enum atoms {
	ATOMS(ENUM)
	NATOMS
};

static Atom atomtab[NATOMS];

static void
init(Display *display)
{
	static char *atomnames[NATOMS] = { ATOMS(NAME) };
	static Bool done = False;

	if (done)
		return;
	XInternAtoms(display, atomnames, NATOMS, False, atomtab);
	done = True;
}

static int
ignoreerror(Display *display, XErrorEvent *event)
{
	(void)display, (void)event;
	return 0;
}

static Window
createwindow(Display *display)
{
	return XCreateWindow(
		display, DefaultRootWindow(display),
		0, 0, 1, 1, 0,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWEventMask, &(XSetWindowAttributes){
			/* this window will get PropertyNotify events */
			.event_mask = PropertyChangeMask,
		}
	);
}

static Time
getservertime(Display *display)
{
	XEvent event;
	Window window;

	/*
	 * To get the server time, we append a zero-length data to a
	 * window's property (any can do), and get the timestamp from
	 * the server in the corresponding XPropertyEvent(3).
	 *
	 * We create (and then delete) a window for that, to not mess
	 * with the mask of events selected on any existing window by
	 * the client.
	 */
	if ((window = createwindow(display)) == None)
		return CurrentTime;
	(void)XChangeProperty(
		display, window,
		XA_WM_NAME, XA_STRING,
		8L, PropModeAppend,
		(void *)"", 0   /* zero-length data */
	);
	(void)XWindowEvent(display, window, PropertyChangeMask, &event);
	(void)XDestroyWindow(display, window);
	return event.xproperty.time;
}

static ssize_t
getmaxsize(Display *display)
{
	size_t n;

	/* compute maximum size for the payload of a ChangeProperty request */
	if ((n = XExtendedMaxRequestSize(display)) == 0)
		n = XMaxRequestSize(display);
	if (n < 0x1000)
		n = 0x1000;     /* requests are no smaller than that */
	n *= 4;                 /* request units are 4-byte long */
	n -= 24;                /* ignore ChangeProperty header */
	return n;               /* got sizeof ChangeProperty's payload */
}

static ssize_t
getcontent(Display *display, Window requestor, Atom property, unsigned char **pbuf)
{
	Atom type;
	unsigned long len, remain;
	ssize_t ret;
	int status, format;

	status = XGetWindowProperty(
		display, requestor, property,
		0, INT_MAX,
		True,   /* delete property after get */
		AnyPropertyType, &type, &format,
		&len, &remain, pbuf
	);
	ret = 0;
	if (status != Success)
		ret = CTRL_ENOMEM;
	else if (type == atomtab[INCR])
		ret = CTRL_EMSGSIZE;
	else if (*pbuf != NULL && len > 0)
		return len;
	XFree(*pbuf);
	*pbuf = NULL;
	return ret;
}

static ssize_t
getatompairs(XSelectionRequestEvent const *event, Atom **atoms)
{
	unsigned long nitems, remain;
	int status, format;
	Atom type;

	status = XGetWindowProperty(
		event->display, event->requestor, event->property,
		0, INT_MAX,
		True,   /* delete property after get */
		atomtab[ATOM_PAIR], &type, &format,
		&nitems, &remain, (void *)atoms
	);
	if (status == Success && format == 32 && *atoms != NULL && nitems > 0)
		return nitems;
	XFree(*atoms);
	*atoms = NULL;
	return status == BadAlloc ? -1 : 0;
}

static Bool
waitnotify(Display *display, Window window, Time timestamp,
		Atom selection, Atom target, XEvent *event)
{
	SYNC_EVENT(display, window, SelectionNotify, event) {
		XSelectionEvent *selevent = &event->xselection;

		if (selevent->selection != selection)
			continue;
		if (selevent->target != target)
			continue;
		if (selevent->time == CurrentTime)
			return True;
		if (selevent->time >= timestamp)
			return True;
	}
	return False;
}

static ssize_t
getatonce(Display *display, Window requestor, Time timestamp,
		Atom selection, Atom target, unsigned char **pbuf)
{
	XEvent event;

	*pbuf = NULL;
	if (target == None)
		return CTRL_NOERROR;
	if (timestamp == CurrentTime)
		timestamp = getservertime(display);
	if (timestamp == CurrentTime)
		return CTRL_NOERROR;
	XDeleteProperty(display, requestor, target);
	XSync(display, False);
	while(XCheckTypedWindowEvent(display, requestor, SelectionNotify, &event))
		;       /* drop any out-of-sync XSelectionEvent(3) */
	(void)XConvertSelection(
		display, selection,
		target, target,
		requestor, timestamp
	);
	if (!waitnotify(display, requestor, timestamp, selection, target, &event))
		return CTRL_ETIMEDOUT;
	if (event.xselection.property != target)
		return CTRL_NOERROR;    /* request not responded */
	return getcontent(display, requestor, target, pbuf);
}

static ssize_t
getbyparts(Display *display, Window requestor, Atom target,
		unsigned char **pbuf)
{
	FILE *stream;
	XEvent event;
	size_t size;
	ssize_t retval;

	if ((stream = open_memstream((char **)pbuf, &size)) == NULL)
		return -errno;
	SYNC_EVENT(display, requestor, PropertyNotify, &event) {
		ssize_t n;
		unsigned char *tmpbuf;

		if (event.xproperty.atom != target)
			continue;
		if (event.xproperty.state != PropertyNewValue)
			continue;
		n = getcontent(display, requestor, target, &tmpbuf);
		errno = 0;
		if (n > 0 && fwrite(tmpbuf, 1, n, stream) != (size_t)n)
			n = errno ? -errno : CTRL_ENOMEM;
		XFree(tmpbuf);
		if (n <= 0) {
			retval = n;
			goto done;
		}
	}
	retval = CTRL_ETIMEDOUT;
done:
	while (fclose(stream) == EOF) {
		if (errno != EINTR) {
			retval = -errno;
			break;
		}
	}
	if (retval == 0)
		return size;
	free(*pbuf);
	*pbuf = NULL;
	return retval;
}

ssize_t
ctrlsel_request(Display *display, Time timestamp, Atom selection,
		Atom target, unsigned char **pbuf)
{
	Window requestor;
	ssize_t retval;

	init(display);
	if (selection == None || target == None)
		return CTRL_NOERROR;
	if ((requestor = createwindow(display)) == None)
		return CTRL_NOERROR;
	retval = getatonce(display, requestor, timestamp, selection, target, pbuf);
	if (retval == CTRL_EMSGSIZE)    /* message is too large */
		retval = getbyparts(display, requestor, target, pbuf);
	(void)XDestroyWindow(display, requestor);
	return retval;
}

ssize_t
ctrlsel_gettargets(Display *display, Time timestamp, Atom selection, Atom **ptargets)
{
	init(display);
	return ctrlsel_request(
		display, timestamp, selection,
		atomtab[TARGETS], (unsigned char **)ptargets
	);
}

Time
ctrlsel_own(Display *display, Window owner, Time timestamp, Atom selection)
{
	if (selection == None)
		return 0;
	if (timestamp == CurrentTime)
		timestamp = getservertime(display);
	if (timestamp == CurrentTime)
		return 0;
	(void)XSetSelectionOwner(display, selection, owner, timestamp);
	if (XGetSelectionOwner(display, selection) == owner)
		return timestamp;
	return 0;
}

static int
answer(XSelectionRequestEvent const *event, Time time,
	Atom const targets[], size_t ntargets,
	ssize_t (*callback)(void *, Atom, unsigned char **), void *arg)
{
	enum { PAIR_TARGET, PAIR_PROPERTY, PAIR_LENGTH };
	ssize_t natoms = PAIR_LENGTH;
	int retval = CTRL_NOERROR;
	Atom storeat = None;    /* requestor's property where we'll store data */
	Atom *p = NULL;
	Atom *atoms = (Atom []){
		[PAIR_TARGET]   = event->target,
		[PAIR_PROPERTY] = event->property,
	};

	if (event->target == None)
		goto done;
	if (event->time != CurrentTime && event->time < time)
		goto done;      /* out-of-time request */
	if (event->target == atomtab[MULTIPLE]) {
		if (event->property == None)
			goto done;
		natoms = getatompairs(event, &p);
		if (natoms < 0)
			retval = CTRL_ENOMEM;
		if (natoms <= 0)
			goto done;
		atoms = p;
	} else if (event->property == None) {
		atoms[PAIR_PROPERTY] = event->target;
	}

	for (ssize_t i = 0; i < natoms; i += 2) {
		Atom *pair = &atoms[i];
		Atom target = pair[PAIR_TARGET];
		Atom property = pair[PAIR_PROPERTY];
		unsigned char *buf = NULL;
		ssize_t size;

		if (property == None) {
			continue;
		} else if (target == atomtab[TIMESTAMP]) {
			(void)XChangeProperty(
				event->display, event->requestor, property,
				XA_INTEGER, 32, PropModeReplace,
				(void *)&time, 1
			);
		} else if (target == atomtab[TARGETS]) {
			(void)XChangeProperty(
				event->display, event->requestor, property,
				XA_ATOM, 32, PropModeReplace,
				(void *)targets, ntargets
			);
			(void)XChangeProperty(
				event->display, event->requestor, property,
				XA_ATOM, 32, PropModeAppend,
				(void *)(Atom[3]){
					atomtab[TIMESTAMP],
					atomtab[TARGETS],
					atomtab[MULTIPLE],
				}, 3
			);
		} else if (target == atomtab[MULTIPLE] || target == None) {
			/* unsupported target */
			pair[PAIR_PROPERTY] = None;
		} else if ((size = callback(arg, target, &buf)) < 0) {
			pair[PAIR_PROPERTY] = None;
		} else if (size > getmaxsize(event->display)) {
			retval = CTRL_EMSGSIZE;
			pair[PAIR_PROPERTY] = None;
		} else {
			(void)XChangeProperty(
				event->display, event->requestor, property,
				target, 8, PropModeReplace,
				buf, size
			);
		}
	}

	if (event->target == atomtab[MULTIPLE]) {
		storeat = event->property;
		(void)XChangeProperty(
			event->display, event->requestor, storeat,
			atomtab[ATOM_PAIR], 32, PropModeReplace,
			(unsigned char *)atoms, natoms
		);
	} else {
		storeat = atoms[PAIR_PROPERTY];
	}

done:
	XFree(p);
	XSendEvent(
		event->display,
		event->requestor,
		False,
		NoEventMask,
		(XEvent *)&(XSelectionEvent){
			.type      = SelectionNotify,
			.display   = event->display,
			.requestor = event->requestor,
			.selection = event->selection,
			.time      = event->time,
			.target    = event->target,
			.property  = storeat,
		}
	);
	return retval;
}

static XErrorHandler
seterrfun(Display *display, XErrorHandler fun)
{
	(void)XSync(display, False);
	return XSetErrorHandler(fun);
}

int
ctrlsel_answer(XEvent const *ep, Time time,
	Atom const targets[], size_t ntargets,
	ssize_t (*callback)(void *, Atom, unsigned char **), void *arg)
{
	XErrorHandler oldhandler;
	int retval;

	if (ep->type != SelectionRequest)
		return 0;
	init(ep->xany.display);
	oldhandler = seterrfun(ep->xany.display, ignoreerror);
	retval = answer(&ep->xselectionrequest, time, targets, ntargets, callback, arg);
	(void)seterrfun(ep->xany.display, oldhandler);
	return retval;
}
