/*
 * ctrldnd: X11 drag-and-drop helper functions
 *
 * Refer to the accompanying manual for a description of the interface.
 */
#ifndef _CTRLDND_H_
#define _CTRLDND_H_

#include <stddef.h>

enum ctrldnd_action {
	CTRLDND_COPY      = 0x01,
	CTRLDND_MOVE      = 0x02,
	CTRLDND_LINK      = 0x04,
	CTRLDND_ASK       = 0x08,
	CTRLDND_PRIVATE   = 0x10,
	CTRLDND_ANYACTION = 0xFFFF,
};

struct ctrldnd_data {
	unsigned char  *data;
	ssize_t         size;
	Atom            target;
};

struct ctrldnd_drop {
	enum ctrldnd_action action;
	struct ctrldnd_data content;
	Time            time;
	Window          window;
	int             x, y;
};

void ctrldnd_announce(
	Display        *display,
	Window          dropsite
);

struct ctrldnd_drop ctrldnd_drag(
	Display        *display,
	int             screen,
	Time            timestamp,
	Window          icon,
	struct ctrldnd_data const contents[],
	size_t          ncontents,
	enum ctrldnd_action actions,
	Time            interval,
	int           (*callback)(XEvent *, void *),
	void           *arg
);

struct ctrldnd_drop ctrldnd_getdrop(
	XEvent         *climsg,
	Atom const      targets[],
	size_t          ntargets,
	enum ctrldnd_action actions,
	Time            interval,
	int           (*callback)(XEvent *, void *),
	void           *arg
);
#endif /* _CTRLDND_H_ */
