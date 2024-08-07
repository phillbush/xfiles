/*
 * ctrlsel: API to own/convert/transfer X11 selections
 * Refer to the accompanying manual for a description of the interface.
 */
#ifndef _CTRLSEL_H_
#define _CTRLSEL_H_

#include <stddef.h>

ssize_t ctrlsel_request(
	Display        *display,
	Time            timestamp,
	Atom            selection,
	Atom            target,
	unsigned char **pbuf
);

ssize_t ctrlsel_gettargets(
	Display        *display,
	Time            timestamp,
	Atom            selection,
	Atom          **ptargets
);

Time ctrlsel_own(
	Display        *display,
	Window          owner,
	Time            timestamp,
	Atom            selection
);

int ctrlsel_answer(
	XEvent const   *event,
	Time            epoch,
	Atom const      targets[],
	size_t          ntargets,
	ssize_t       (*callback)(void *, Atom, unsigned char **),
	void           *arg
);

#endif /* _CTRLSEL_H_ */
