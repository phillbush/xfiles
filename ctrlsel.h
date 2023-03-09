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

struct CtrlSelTarget {
	Atom target;
	Atom type;
	int format;
	unsigned int action;
	unsigned long nitems;
	unsigned long bufsize;
	unsigned char *buffer;
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

/*
 * Fill in a CtrlSelTarget structure with the given information.
 *
 * Arguments are as follows:
 * - TARGET:    The Atom naming the target.
 *
 * - TYPE:      The Atom naming the type of the target
 *              (usually the same as TARGET).
 *
 * - FORMAT:    8, 16 or 32
 *
 * - BUFFER:    The content of the selection on the given target.
 *
 * - SIZE:      Size, in bytes, of the buffer.
 *
 * - FILL:      Pointer to target structure to be filled.
 */
void
ctrlsel_filltarget(
	Atom target,
	Atom type,
	int format,
	unsigned char *buffer,
	unsigned long size,
	struct CtrlSelTarget *fill
);

/*
 * Request the conversion of the content of the given selection into
 * the given targets.  The BUFFER member of each TARGETS[i] will be
 * dynamically allocated and can be used by the caller after either
 * getting a CTRLSEL_RECEIVED from ctrlsel_receive(), or cancelling
 * the request with ctrlsel_cancel().   The allocated buffer should
 * be freed by the caller.
 *
 * This function fills in a selection context that can be passed later
 * to ctrlsel_receive (to actually receive the requested selections),
 * and ctrlsel_cancel (to cancel the request).
 *
 * Arguments are as follows:
 * - DISPLAY:   Pointer to Display returned by XOpenDisplay(3).
 *
 * - WINDOW:    ID of window returned by XCreateWindow(3).  The client
 *              must have selected StructureNotify and PropertyChange
 *              events from the window.
 *
 * - SELECTION: Atom for the selection whose content is requested.
 *
 * - TIME:      Time (X11 timestamp) of the requestion.
 *
 * - TARGETS:   Array of target structures, each one of which must
 *              have been filled by ctrlsel_filltarget().  The BUFFER,
 *              NITEMS and BUFSIZE members will be zeroed.
 *
 * - NTARGETS:  Number of elements in the TARGETS array.
 *
 * - CONTEXT:   Pointer to an existing context to be filled.
 *
 * Return 0 on error; nonzero otherwise.
 */
int
ctrlsel_request(
	Display *display,
	Window window,
	Atom selection,
	Time time,
	struct CtrlSelTarget targets[],
	unsigned long ntargets,
	struct CtrlSelContext *context
);

/*
 * Give ownership of the selection to the given window.  The BUFFER
 * member of each TARGETS[i] must point to allocated addresses that
 * must not be freed until getting a CTRLSEL_LOST from ctrlsel_send(),
 * or cancelling the ownership with ctrlsel_disown().
 *
 * This function fills in a selection context that can be passed later
 * to ctrlsel_send (to actually supply the selections to requestors),
 * and ctrlsel_disown (to cancel the ownership).
 *
 * Arguments are as follows:
 * - DISPLAY:   Pointer to Display returned by XOpenDisplay(3).
 *
 * - WINDOW:    ID of window returned by XCreateWindow(3).  The client
 *              must have selected StructureNotify and PropertyChange
 *              events from the window.
 *
 * - SELECTION: Atom for the selection to be owned.
 *
 * - TIME:      Time (X11 timestamp) of the ownership.
 *
 * - ISMANAGER: Nonzero if the selection is a manager selection; zero
 *              otherwise.
 *
 * - TARGETS:   Array of target structures, each one of which must
 *              have been filled by ctrlsel_filltarget().  The BUFFER
 *              member must point to existing address; BUFSIZE must
 *              be set to the number of bytes in BUFFER; NITEMS is
 *              automatically set by ctrlsel_filltarget().
 *
 * - NTARGETS:  Number of elements in the TARGETS array.
 *
 * - CONTEXT:   Pointer to an existing context to be filled.
 *
 * Return 0 on error; nonzero otherwise.
 */
int
ctrlsel_setowner(
	Display *display,
	Window window,
	Atom selection,
	Time time,
	int ismanager,
	struct CtrlSelTarget targets[],
	unsigned long ntargets,
	struct CtrlSelContext *context
);

/*
 * Process the selection request represented by CONTEXT, which must have
 * been previously filled by ctrlsel_request().  This function must be
 * called after XNextEvent(3) (or related routine) to filter the event.
 *
 * Arguments are as follows:
 * - CONTEXT:   Pointer to structure which contains the data for the
 *              selection request and which must have been previously
 *              filled by ctrlsel_request.
 *
 * - EVENT:     XEvent to be filtered.
 *
 * It returns:
 * - CTRLSEL_NONE:      The event is not related to the selection request.
 *                      The event can be further processed by the caller.
 *
 * - CTRLSEL_INTERNAL:  The event was filtered by ctrlsel_receive,
 *                      meaning that some internal state was changed.
 *                      The event must be ignored by the caller.
 *
 * - CTRLSEL_RECEIVED:  The event was filtered by ctrlsel_receive,
 *                      meaning that the selection content was received
 *                      and converted into all targets.  The event must
 *                      be ignored by the caller.
 *
 * - CTRLSEL_ERROR:     The event was filtered by ctrlsel_receive,
 *                      meaning that an error occurred.  The event must
 *                      be ignored by the caller and the context must
 *                      be discarded (not used in further calls to
 *                      ctrlsel_request).
 */
int ctrlsel_receive(struct CtrlSelContext *context, XEvent *event);

/*
 * Supply to any requestor the selection represented by CONTEXT, which
 * must have been previously filled by ctrlsel_request().  This function
 * must be called after XNextEvent(3) (or related routine) to filter the
 * event.
 *
 * Arguments are as follows:
 * - CONTEXT:   Pointer to structure which contains the data for the
 *              selection supply and which must have been previously
 *              filled by ctrlsel_setowner.
 *
 * - EVENT:     XEvent to be filtered.
 *
 * It returns:
 * - CTRLSEL_NONE:      The event is not related to the selection supply.
 *                      The event can be further processed by the caller.
 *
 * - CTRLSEL_INTERNAL:  The event was filtered by ctrlsel_send, meaning
 *                      that some internal state was changed.  The event
 *                      must be ignored by the caller.
 *
 * - CTRLSEL_LOST:      The event was filtered by ctrlsel_send, meaning
 *                      that the selection ownership has been lost.  The
 *                      event must be ignored by the caller and the
 *                      context must be discarded (not used in further
 *                      calls to ctrsel_send).
 *
 * - CTRLSEL_ERROR:     The event was filtered by ctrlsel_send, meaning
 *                      that an error occurred.  The event must be
 *                      ignored by the caller and the context must
 *                      be discarded (not used in further calls to
 *                      ctrlsel_send).
 */
int ctrlsel_send(struct CtrlSelContext *context, XEvent *event);

/*
 * Cancel any receiving in progress.
 *
 * Arguments are as follows:
 * - CONTEXT:   Pointer to structure which contains the data for the
 *              selection request and which must have been previously
 *              filled by ctrlsel_request.
 */
void ctrlsel_cancel(struct CtrlSelContext *context);

/*
 * Cancel any selection supply in progress.
 *
 * Arguments are as follows:
 * - CONTEXT:   Pointer to structure which contains the data for the
 *              selection supply and which must have been previously
 *              filled by ctrlsel_setowner.
 */
void ctrlsel_disown(struct CtrlSelContext *context);

int
ctrlsel_dndwatch(
	Display *display,
	Window window,
	unsigned int actions,
	struct CtrlSelTarget targets[],
	unsigned long ntargets,
	struct CtrlSelContext *context
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
	struct CtrlSelContext *context
);

int ctrlsel_dndsend(struct CtrlSelContext *context, XEvent *event);

void ctrlsel_dnddisown(struct CtrlSelContext *context);

#endif /* _CTRLSEL_H_ */
