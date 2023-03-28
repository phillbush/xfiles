/*
 * ctrlsel: X11 selection ownership and request helper functions
 *
 * Refer to the accompanying manual for a description of the interface.
 */
#ifndef _CTRLSEL_H_
#define _CTRLSEL_H_

enum {
	CTRLSEL_NONE,
	CTRLSEL_INTERNAL,
	CTRLSEL_RECEIVED,
	CTRLSEL_SENT,
	CTRLSEL_DROPSELF,
	CTRLSEL_DROPOTHER,
	CTRLSEL_ERROR,
	CTRLSEL_LOST
};

enum {
	CTRLSEL_COPY    = 0x01,
	CTRLSEL_MOVE    = 0x02,
	CTRLSEL_LINK    = 0x04,
	CTRLSEL_ASK     = 0x08,
	CTRLSEL_PRIVATE = 0x10,
};

typedef struct CtrlSelContext CtrlSelContext;

struct CtrlSelTarget {
	Atom target;
	Atom type;
	int format;
	unsigned int action;
	unsigned long nitems;
	unsigned long bufsize;
	unsigned char *buffer;
};

void
ctrlsel_filltarget(
	Atom target,
	Atom type,
	int format,
	unsigned char *buffer,
	unsigned long size,
	struct CtrlSelTarget *fill
);

CtrlSelContext *
ctrlsel_request(
	Display *display,
	Window window,
	Atom selection,
	Time time,
	struct CtrlSelTarget targets[],
	unsigned long ntargets
);

CtrlSelContext *
ctrlsel_setowner(
	Display *display,
	Window window,
	Atom selection,
	Time time,
	int ismanager,
	struct CtrlSelTarget targets[],
	unsigned long ntargets
);

int ctrlsel_receive(struct CtrlSelContext *context, XEvent *event);

int ctrlsel_send(struct CtrlSelContext *context, XEvent *event);

void ctrlsel_cancel(struct CtrlSelContext *context);

void ctrlsel_disown(struct CtrlSelContext *context);

CtrlSelContext *
ctrlsel_dndwatch(
	Display *display,
	Window window,
	unsigned int actions,
	struct CtrlSelTarget targets[],
	unsigned long ntargets
);

int ctrlsel_dndreceive(struct CtrlSelContext *context, XEvent *event);

void ctrlsel_dndclose(struct CtrlSelContext *context);

int
ctrlsel_dndown(
	Display *display,
	Window window,
	Window miniature,
	Time time,
	struct CtrlSelTarget targets[],
	unsigned long ntargets,
	CtrlSelContext **context
);

int ctrlsel_dndsend(struct CtrlSelContext *context, XEvent *event);

void ctrlsel_dnddisown(struct CtrlSelContext *context);

#endif /* _CTRLSEL_H_ */
