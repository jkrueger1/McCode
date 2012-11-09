/*
 * tkUnixSelect.c --
 *
 *      This file contains X specific routines for manipulating
 *      selections.
 *
 * Copyright (c) 1995-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tkUnixSelect.c,v 1.11 2002/10/01 08:48:08 dkf Exp $
 */

#include "tkInt.h"
#include "tkSelect.h"

typedef struct ConvertInfo {
    int offset;                 /* The starting byte offset into the selection
				 * for the next chunk; -1 means all data has
				 * been transferred for this conversion. -2
				 * means only the final zero-length transfer
				 * still has to be done.  Otherwise it is the
				 * offset of the next chunk of data to
				 * transfer. */
    Tcl_EncodingState state;    /* The encoding state needed across chunks. */
    char buffer[TCL_UTF_MAX];   /* A buffer to hold part of a UTF character
				 * that is split across chunks.*/
} ConvertInfo;

/*
 * When handling INCR-style selection retrievals, the selection owner
 * uses the following data structure to communicate between the
 * ConvertSelection procedure and TkSelPropProc.
 */

typedef struct IncrInfo {
    TkWindow *winPtr;           /* Window that owns selection. */
    Atom selection;             /* Selection that is being retrieved. */
    Atom *multAtoms;            /* Information about conversions to
				 * perform:  one or more pairs of
				 * (target, property).  This either
				 * points to a retrieved  property (for
				 * MULTIPLE retrievals) or to a static
				 * array. */
    unsigned long numConversions;
				/* Number of entries in converts (same as
				 * # of pairs in multAtoms). */
    ConvertInfo *converts;      /* One entry for each pair in multAtoms.
				 * This array is malloc-ed. */
    char **tempBufs;            /* One pointer for each pair in multAtoms;
				 * each pointer is either NULL, or it points
				 * to a small bit of character data that was
				 * left over from the previous chunk. */
    Tcl_EncodingState *state;   /* One state info per pair in multAtoms:
				 * State info for encoding conversions
				 * that span multiple buffers. */
    int *flags;                 /* One state flag per pair in multAtoms:
				 * Encoding flags, set to TCL_ENCODING_START
				 * at the beginning of an INCR transfer. */
    int numIncrs;               /* Number of entries in converts that
				 * aren't -1 (i.e. # of INCR-mode transfers
				 * not yet completed). */
    Tcl_TimerToken timeout;     /* Token for timer procedure. */
    int idleTime;               /* Number of seconds since we heard
				 * anything from the selection
				 * requestor. */
    Window reqWindow;           /* Requestor's window id. */
    Time time;                  /* Timestamp corresponding to
				 * selection at beginning of request;
				 * used to abort transfer if selection
				 * changes. */
    struct IncrInfo *nextPtr;   /* Next in list of all INCR-style
				 * retrievals currently pending. */
} IncrInfo;


typedef struct ThreadSpecificData {
    IncrInfo *pendingIncrs;     /* List of all incr structures
				 * currently active. */
} ThreadSpecificData;
static Tcl_ThreadDataKey dataKey;

/*
 * Largest property that we'll accept when sending or receiving the
 * selection:
 */

#define MAX_PROP_WORDS 100000

static TkSelRetrievalInfo *pendingRetrievals = NULL;
				/* List of all retrievals currently
				 * being waited for. */

/*
 * Forward declarations for procedures defined in this file:
 */

static void             ConvertSelection _ANSI_ARGS_((TkWindow *winPtr,
			    XSelectionRequestEvent *eventPtr));
static void             IncrTimeoutProc _ANSI_ARGS_((ClientData clientData));
static int              SelectionSize _ANSI_ARGS_((TkSelHandler *selPtr, Atom type, Tk_Window tkwin));
static int              SelGetProc _ANSI_ARGS_((ClientData clientData,
			    Tcl_Interp *interp, char *portion));
static void             SelRcvIncrProc _ANSI_ARGS_((ClientData clientData,
			    XEvent *eventPtr));
static void             SelTimeoutProc _ANSI_ARGS_((ClientData clientData));

static void             FreeHandler _ANSI_ARGS_((ClientData clientData));
static int              HandleCompat _ANSI_ARGS_((ClientData clientData,
			    int offset, long *buffer, int maxBytes,
			    Atom type, Tk_Window tkwin));

/*
 *----------------------------------------------------------------------
 *
 * TkSelGetSelection --
 *
 *      Retrieve the specified selection from another process.
 *
 * Results:
 *      The return value is a standard Tcl return value.
 *      If an error occurs (such as no selection exists)
 *      then an error message is left in the interp's result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
TkSelGetSelection(interp, tkwin, selection, target, proc, clientData)
    Tcl_Interp *interp;         /* Interpreter to use for reporting
				 * errors. */
    Tk_Window tkwin;            /* Window on whose behalf to retrieve
				 * the selection (determines display
				 * from which to retrieve). */
    Atom selection;             /* Selection to retrieve. */
    Atom target;                /* Desired form in which selection
				 * is to be returned. */
    Tk_GetXSelProc *proc;       /* Procedure to call to process the
				 * selection, once it has been retrieved. */
    ClientData clientData;      /* Arbitrary value to pass to proc. */
{
    TkSelRetrievalInfo retr;
    TkWindow *winPtr = (TkWindow *) tkwin;
    TkDisplay *dispPtr = winPtr->dispPtr;

    /*
     * The selection is owned by some other process.  To
     * retrieve it, first record information about the retrieval
     * in progress.  Use an internal window as the requestor.
     */

    retr.interp = interp;
    if (dispPtr->clipWindow == NULL) {
	int result;

	result = TkClipInit(interp, dispPtr);
	if (result != TCL_OK) {
	    return result;
	}
    }
    retr.winPtr = (TkWindow *) dispPtr->clipWindow;
    retr.selection = selection;
    retr.property = selection;
    retr.target = target;
    retr.proc = proc;
    retr.clientData = clientData;
    retr.result = -1;
    retr.idleTime = 0;
    retr.encFlags = TCL_ENCODING_START;
    retr.nextPtr = pendingRetrievals;
    Tcl_DStringInit(&retr.buf);
    pendingRetrievals = &retr;

    /*
     * Tcl/Tk says:
     * Initiate the request for the selection.  Note:  can't use
     * TkCurrentTime for the time.  If we do, and this application hasn't
     * received any X events in a long time, the current time will be way
     * in the past and could even predate the time when the selection was
     * made;  if this happens, the request will be rejected.
     *
     * NI-S Believes that X ICCCM rules say we must use an event time
     * and such a rejection is valid - try it and see.
     */

    XConvertSelection(winPtr->display, retr.selection, retr.target,
	    retr.property, retr.winPtr->window, TkCurrentTime(dispPtr,1));

    /*
     * Enter a loop processing X events until the selection
     * has been retrieved and processed.  If no response is
     * received within a few seconds, then timeout.
     */

    retr.timeout = Tcl_CreateTimerHandler(1000, SelTimeoutProc,
	    (ClientData) &retr);
    while (retr.result == -1) {
	Tcl_DoOneEvent(0);
    }
    Tcl_DeleteTimerHandler(retr.timeout);

    /*
     * Unregister the information about the selection retrieval
     * in progress.
     */

    if (pendingRetrievals == &retr) {
	pendingRetrievals = retr.nextPtr;
    } else {
	TkSelRetrievalInfo *retrPtr;

	for (retrPtr = pendingRetrievals; retrPtr != NULL;
		retrPtr = retrPtr->nextPtr) {
	    if (retrPtr->nextPtr == &retr) {
		retrPtr->nextPtr = retr.nextPtr;
		break;
	    }
	}
    }
    Tcl_DStringFree(&retr.buf);
    return retr.result;
}

/*
 *----------------------------------------------------------------------
 *
 * TkSelPropProc --
 *
 *      This procedure is invoked when property-change events
 *      occur on windows not known to the toolkit.  Its function
 *      is to implement the sending side of the INCR selection
 *      retrieval protocol when the selection requestor deletes
 *      the property containing a part of the selection.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      If the property that is receiving the selection was just
 *      deleted, then a new piece of the selection is fetched and
 *      placed in the property, until eventually there's no more
 *      selection to fetch.
 *
 *----------------------------------------------------------------------
 */
void
TkSelPropProc(eventPtr)
    register XEvent *eventPtr;          /* X PropertyChange event. */
{
    register IncrInfo *incrPtr;
    register TkSelHandler *selPtr;
    unsigned i;
    int length, numItems;
    Atom target, formatType = None;
    long buffer[TK_SEL_WORDS_AT_ONCE];
    TkDisplay *dispPtr = TkGetDisplay(eventPtr->xany.display);
    Tk_ErrorHandler errorHandler;
    ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    /*
     * See if this event announces the deletion of a property being
     * used for an INCR transfer.  If so, then add the next chunk of
     * data to the property.
     */

    if (eventPtr->xproperty.state != PropertyDelete) {
	return;
    }
    for (incrPtr = tsdPtr->pendingIncrs; incrPtr != NULL;
	    incrPtr = incrPtr->nextPtr) {
	if (incrPtr->reqWindow != eventPtr->xproperty.window) {
	    continue;
	}

	/*
	 * For each conversion that has been requested, handle any
	 * chunks that haven't been transmitted yet.
	 */

	for (i = 0; i < incrPtr->numConversions; i++) {
	    if ((eventPtr->xproperty.atom != incrPtr->multAtoms[2*i + 1])
		    || (incrPtr->converts[i].offset == -1)) {
		continue;
	    }
	    target = incrPtr->multAtoms[2*i];
	    incrPtr->idleTime = 0;

	    /*
	     * Look for a matching selection handler.
	     */

	    for (selPtr = incrPtr->winPtr->selHandlerList; ;
		    selPtr = selPtr->nextPtr) {
		if (selPtr == NULL) {
		    /*
		     * No handlers match, so mark the conversion as done.
		     */

		    incrPtr->multAtoms[2*i + 1] = None;
		    incrPtr->converts[i].offset = -1;
		    incrPtr->numIncrs --;
		    return;
		}
		if ((selPtr->target == target)
			&& (selPtr->selection == incrPtr->selection)) {
		    break;
		}
	    }

	    /*
	     * We found a handler, so get the next chunk from it.
	     */

	    LangSelectHook("INCRRequest",(Tk_Window) incrPtr->winPtr,
                           selPtr->selection, selPtr->target, selPtr->format);

	    formatType = selPtr->format;
	    if (incrPtr->converts[i].offset == -2) {
		/*
		 * We already got the last chunk, so send a null chunk
		 * to indicate that we are finished.
		 */

		numItems = 0;
		length = 0;
	    } else {
		TkSelInProgress ip;
		ip.selPtr = selPtr;
		ip.nextPtr = TkSelGetInProgress();
		TkSelSetInProgress(&ip);

		/*
		 * Copy any bytes left over from a partial character at the end
		 * of the previous chunk into the beginning of the buffer.
		 * Pass the rest of the buffer space into the selection
		 * handler.
		 */

		length = strlen(incrPtr->converts[i].buffer);
		strcpy((char *)buffer, incrPtr->converts[i].buffer);

		numItems = (*selPtr->proc)(selPtr->clientData,
			incrPtr->converts[i].offset,
			(long *)(((char *) buffer) + length),
			TK_SEL_BYTES_AT_ONCE - length, formatType, (Tk_Window) incrPtr->winPtr);
		TkSelSetInProgress(ip.nextPtr);
		if (ip.selPtr == NULL) {
		    /*
		     * The selection handler deleted itself.
		     */

		    return;
		}
		if (numItems < 0) {
		    numItems = 0;
		}
		numItems += length;
		if (numItems > TK_SEL_BYTES_AT_ONCE) {
		    panic("selection handler returned too many bytes");
		}
	    }
	    ((char *) buffer)[numItems] = 0;

	    errorHandler = Tk_CreateErrorHandler(eventPtr->xproperty.display,
		    -1, -1, -1, (int (*)()) NULL, (ClientData) NULL);
	    /*
	     * Encode the data using the proper format for each type.
	     */

	    if ((formatType == XA_STRING)
		    || (dispPtr && formatType==dispPtr->utf8Atom)
		    || (dispPtr && formatType==dispPtr->compoundTextAtom)) {
		char *space = NULL;
		int   spaceAlloc = 0;
		int   spaceUsed  = 0;
		int encodingCvtFlags;
		int srcLen, dstLen, result, srcRead, dstWrote, soFar;
		char *src, *dst;
		Tcl_Encoding encoding;

		/*
		 * Set up the encoding state based on the format and whether
		 * this is the first and/or last chunk.
		 */

		encodingCvtFlags = 0;
		if (incrPtr->converts[i].offset == 0) {
		    encodingCvtFlags |= TCL_ENCODING_START;
		}
		if (numItems < TK_SEL_BYTES_AT_ONCE) {
		    encodingCvtFlags |= TCL_ENCODING_END;
		}
		if (formatType == XA_STRING) {
		    encoding = Tcl_GetEncoding(NULL, "iso8859-1");
		} else if (dispPtr && formatType==dispPtr->utf8Atom) {
		    encoding = Tcl_GetEncoding(NULL, "utf-8");
		} else {
		    encoding = Tcl_GetEncoding(NULL, "iso2022");
		}

		/*
		 * Now convert the data.
		 */

		src = (char *)buffer;
		srcLen = numItems;

		dstLen = 2*numItems;
		if (dstLen < 16) {
		    dstLen = 16;
		}
		space = ckalloc(dstLen+1);
		if (space) {
		    spaceAlloc = dstLen;
		}
		dst    = space;
		dstLen = spaceAlloc;

		/*
		 * Now convert the data, growing the destination buffer
		 * as needed.
		 */

		while (1) {
		    result = Tcl_UtfToExternal(NULL, encoding,
			    src, srcLen, encodingCvtFlags,
			    &incrPtr->converts[i].state,
			    dst, dstLen, &srcRead, &dstWrote, NULL);
		    soFar = dst + dstWrote - space;
		    encodingCvtFlags &= ~TCL_ENCODING_START;
		    src += srcRead;
		    srcLen -= srcRead;
		    spaceUsed = soFar;
		    if (result != TCL_CONVERT_NOSPACE) {
			break;
		    }
		    /* use dst/dstLen as scratch for realloc */
		    dstLen = 2*spaceUsed;
		    if (!dstLen) {
			dstLen = numItems;
		    }
		    if ((dst = ckrealloc(space,dstLen+1))) {
			space = dst;
			spaceAlloc = dstLen;
		    }
		    else {
			/* Could not get that much */
		        panic("Could not get %d bytes for conversion",dstLen+1);
			break;
		    }
		    /* reset dst/dstLen for avail region */
		    dst = space + soFar;
		    dstLen = spaceAlloc - (soFar);
		}
		spaceUsed = soFar;
		space[spaceUsed] = '\0';

		if (encoding) {
		    Tcl_FreeEncoding(encoding);
		}

		/*
		 * Set the property to the encoded string value.
		 */

		XChangeProperty(eventPtr->xproperty.display,
			eventPtr->xproperty.window, eventPtr->xproperty.atom,
			formatType, 8, PropModeReplace,
			(unsigned char *) space, spaceUsed);

		/*
		 * Preserve any left-over bytes.
		 */

		if (srcLen > TCL_UTF_MAX) {
		    panic("selection conversion left too many bytes unconverted");
		}
		memcpy(incrPtr->converts[i].buffer, src, (size_t) srcLen+1);
		ckfree(space);
	    } else {
#ifdef _LANG
		/*
		 * Set the property to something other than a string
		 */

		long *propPtr = (long *) ckalloc(TK_SEL_BYTES_AT_ONCE);
		numItems = TkSelCvtToX(propPtr, (char *) buffer,
			formatType, (Tk_Window) incrPtr->winPtr,
			TK_SEL_BYTES_AT_ONCE);

		XChangeProperty(eventPtr->xproperty.display,
			eventPtr->xproperty.window,
			eventPtr->xproperty.atom, formatType, 32,
			PropModeReplace,
			(unsigned char *) propPtr, numItems);

		ckfree((char *)propPtr);
#else
		/*
		 * Set the property to the encoded string value.
		 */

		char *propPtr = (char *) SelCvtToX((char *) buffer,
			formatType, (Tk_Window) incrPtr->winPtr,
			&numItems);

		XChangeProperty(eventPtr->xproperty.display,
			eventPtr->xproperty.window,
			eventPtr->xproperty.atom, formatType, 8,
			PropModeReplace,
			(unsigned char *) Tcl_DStringValue(&ds), numItems);

		ckfree(propPtr);
#endif
	    }
	    Tk_DeleteErrorHandler(errorHandler);

	    /*
	     * Compute the next offset value.  If this was the last chunk,
	     * then set the offset to -2.  If this was an empty chunk,
	     * then set the offset to -1 to indicate we are done.
	     */

	    if (numItems < TK_SEL_BYTES_AT_ONCE) {
		if (numItems <= 0) {
		    incrPtr->converts[i].offset = -1;
		    incrPtr->numIncrs--;
		} else {
		    incrPtr->converts[i].offset = -2;
		}
	    } else {
		/*
		 * Advance over the selection data that was consumed
		 * this time.
		 */

		incrPtr->converts[i].offset += numItems - length;
	    }
	    return;
	}
    }
}

/*
 *--------------------------------------------------------------
 *
 * TkSelEventProc --
 *
 *      This procedure is invoked whenever a selection-related
 *      event occurs.  It does the lion's share of the work
 *      in implementing the selection protocol.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Lots:  depends on the type of event.
 *
 *--------------------------------------------------------------
 */

void
TkSelEventProc(tkwin, eventPtr)
    Tk_Window tkwin;            /* Window for which event was
				 * targeted. */
    register XEvent *eventPtr;  /* X event:  either SelectionClear,
				 * SelectionRequest, or
				 * SelectionNotify. */
{
    register TkWindow *winPtr = (TkWindow *) tkwin;
    TkDisplay *dispPtr = winPtr->dispPtr;
    Tcl_Interp *interp;

    /*
     * Case #1: SelectionClear events.
     */

    if (eventPtr->type == SelectionClear) {
	TkSelClearSelection(tkwin, eventPtr);
    }

    /*
     * Case #2: SelectionNotify events.  Call the relevant procedure
     * to handle the incoming selection.
     */

    if (eventPtr->type == SelectionNotify) {
	register TkSelRetrievalInfo *retrPtr;
	long *propInfo;
	Atom type;
	int format, result;
	unsigned long numItems, bytesAfter;
	Tcl_DString ds;
	Tcl_Encoding encoding;

	for (retrPtr = pendingRetrievals; ; retrPtr = retrPtr->nextPtr) {
	    if (retrPtr == NULL) {
		return;
	    }
	    if ((retrPtr->winPtr == winPtr)
		    && (retrPtr->selection == eventPtr->xselection.selection)
		    && (retrPtr->target == eventPtr->xselection.target)
		    && (retrPtr->result == -1)) {
		if (retrPtr->property == eventPtr->xselection.property) {
		    break;
		}
		if (eventPtr->xselection.property == None) {
		    Tcl_SetResult(retrPtr->interp, (char *) NULL, TCL_STATIC);
		    Tcl_AppendResult(retrPtr->interp,
			    Tk_GetAtomName(tkwin, retrPtr->selection),
			    " selection doesn't exist or form \"",
			    Tk_GetAtomName(tkwin, retrPtr->target),
			    "\" not defined", (char *) NULL);
		    retrPtr->result = TCL_ERROR;
		    return;
		}
	    }
	}

	propInfo = NULL;
	result = XGetWindowProperty(eventPtr->xselection.display,
		eventPtr->xselection.requestor, retrPtr->property,
		0, MAX_PROP_WORDS, False, (Atom) AnyPropertyType,
		&type, &format, &numItems, &bytesAfter,
		(unsigned char **) &propInfo);
	if ((result != Success) || (type == None)) {
	    return;
	}
	if (bytesAfter != 0) {
	    Tcl_SetResult(retrPtr->interp, "selection property too large",
		TCL_STATIC);
	    retrPtr->result = TCL_ERROR;
	    XFree((char *) propInfo);
	    return;
	}

        LangSelectHook("Notify",(Tk_Window) winPtr, retrPtr->selection, retrPtr->target, type);

	if (type == dispPtr->utf8Atom) {
	    /*
	     * The X selection data is in UTF-8 format already.
	     * We can't guarantee that propInfo is NULL-terminated,
	     * so we might have to copy the string.
	     */
	    char *propData = (char *) propInfo;

	    if (format != 8) {
		char buf[64 + TCL_INTEGER_SPACE];

		sprintf(buf,
			"bad format for UTF-8 string selection: wanted \"8\", got \"%d\"",
			format);
		Tcl_SetResult(retrPtr->interp, buf, TCL_VOLATILE);
		retrPtr->result = TCL_ERROR;
		return;
	    }

	    if (numItems >= sizeof(long)*MAX_PROP_WORDS || propData[numItems] != '\0') {
		propData = ckalloc((size_t) numItems + 1);
		strncpy(propData, (char *) propInfo, numItems);
		propData[numItems] = '\0';
	    }
	    retrPtr->result = (*retrPtr->proc)(retrPtr->clientData, retrPtr->interp, (long *) propData,
			       numItems, format, type, (Tk_Window) winPtr);
	    if (propData != (char *) propInfo) {
		ckfree((char *) propData);
	    }
	} else if (type == dispPtr->incrAtom) {

	    /*
	     * It's a !?#@!?!! INCR-style reception.  Arrange to receive
	     * the selection in pieces, using the ICCCM protocol, then
	     * hang around until either the selection is all here or a
	     * timeout occurs.
	     */

	    retrPtr->idleTime = 0;
	    Tk_CreateEventHandler(tkwin, PropertyChangeMask, SelRcvIncrProc,
		    (ClientData) retrPtr);
	    XDeleteProperty(Tk_Display(tkwin), Tk_WindowId(tkwin),
		    retrPtr->property);
	    while (retrPtr->result == -1) {
		Tcl_DoOneEvent(0);
	    }
	    Tk_DeleteEventHandler(tkwin, PropertyChangeMask, SelRcvIncrProc,
		    (ClientData) retrPtr);
	} else if ((type == XA_STRING) || (type == dispPtr->textAtom)
		|| (type == dispPtr->compoundTextAtom)) {
	    if (format != 8) {
		char buf[64 + TCL_INTEGER_SPACE];

		sprintf(buf,
			"bad format for string selection: wanted \"8\", got \"%d\"",
			format);
		Tcl_SetResult(retrPtr->interp, buf, TCL_VOLATILE);
		retrPtr->result = TCL_ERROR;
		return;
	    }
	    interp = retrPtr->interp;
	    Tcl_Preserve((ClientData) interp);

	    /*
	     * Convert the X selection data into UTF before passing it
	     * to the selection callback.  Note that the COMPOUND_TEXT
	     * uses a modified iso2022 encoding, not the current system
	     * encoding.  For now we'll just blindly apply the iso2022
	     * encoding.  This is probably wrong, but it's a placeholder
	     * until we figure out what we're really supposed to do.  For
	     * STRING, we need to use Latin-1 instead.  Again, it's not
	     * really the full iso8859-1 space, but this is close enough.
	     */

	    if (type == dispPtr->compoundTextAtom) {
		encoding = Tcl_GetEncoding(NULL, "iso2022");
	    } else {
		encoding = Tcl_GetEncoding(NULL, "iso8859-1");
	    }
	    Tcl_ExternalToUtfDString(encoding, (char *) propInfo, (int)numItems, &ds);
	    if (encoding) {
		Tcl_FreeEncoding(encoding);
	    }
	    /*
	     * Assuming we know about UTF8_STRING then use that as the type
	     * when calling the handler
	     */
	    if (dispPtr->utf8Atom) {
		type = dispPtr->utf8Atom;
	    }
	    retrPtr->result = (*retrPtr->proc)(retrPtr->clientData,
		    interp, (long *) Tcl_DStringValue(&ds), Tcl_DStringLength(&ds),
		    format, type, (Tk_Window) winPtr);
	    Tcl_DStringFree(&ds);
	    Tcl_Release((ClientData) interp);
	} else {
#ifndef _LANG
	    char *string;
	    if (format != 32) {
		char buf[64 + TCL_INTEGER_SPACE];

		sprintf(buf,
			"bad format for selection: wanted \"32\", got \"%d\"",
			format);
		Tcl_SetResult(retrPtr->interp, buf, TCL_VOLATILE);
		retrPtr->result = TCL_ERROR;
		XFree((char *) propInfo);
		return;
	    }
	    string = TkSelCvtFromX((long *) propInfo, (int) numItems, type,
		    (Tk_Window) winPtr);
#endif
	    interp = retrPtr->interp;
	    Tcl_Preserve((ClientData) interp);
	    retrPtr->result = (*retrPtr->proc)(retrPtr->clientData,
		    interp, propInfo, (int) numItems, format, type, (Tk_Window) winPtr);
	    Tcl_Release((ClientData) interp);
	}
	XFree((char *) propInfo);
	return;
    }

    /*
     * Case #3: SelectionRequest events.  Call ConvertSelection to
     * do the dirty work.
     */

    if (eventPtr->type == SelectionRequest) {
	ConvertSelection(winPtr, &eventPtr->xselectionrequest);
	return;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SelTimeoutProc --
 *
 *      This procedure is invoked once every second while waiting for
 *      the selection to be returned.  After a while it gives up and
 *      aborts the selection retrieval.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      A new timer callback is created to call us again in another
 *      second, unless time has expired, in which case an error is
 *      recorded for the retrieval.
 *
 *----------------------------------------------------------------------
 */

static void
SelTimeoutProc(clientData)
    ClientData clientData;              /* Information about retrieval
					 * in progress. */
{
    register TkSelRetrievalInfo *retrPtr = (TkSelRetrievalInfo *) clientData;

    /*
     * Make sure that the retrieval is still in progress.  Then
     * see how long it's been since any sort of response was received
     * from the other side.
     */

    if (retrPtr->result != -1) {
	return;
    }
    retrPtr->idleTime++;
    if (retrPtr->idleTime >= 5) {

	/*
	 * Use a careful procedure to store the error message, because
	 * the result could already be partially filled in with a partial
	 * selection return.
	 */

	Tcl_SetResult(retrPtr->interp, "selection owner didn't respond",
		TCL_STATIC);
	retrPtr->result = TCL_ERROR;
    } else {
	retrPtr->timeout = Tcl_CreateTimerHandler(1000, SelTimeoutProc,
	    (ClientData) retrPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ConvertSelection --
 *
 *      This procedure is invoked to handle SelectionRequest events.
 *      It responds to the requests, obeying the ICCCM protocols.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Properties are created for the selection requestor, and a
 *      SelectionNotify event is generated for the selection
 *      requestor.  In the event of long selections, this procedure
 *      implements INCR-mode transfers, using the ICCCM protocol.
 *
 *----------------------------------------------------------------------
 */

static void
ConvertSelection(winPtr, eventPtr)
    TkWindow *winPtr;                   /* Window that received the
					 * conversion request;  may not be
					 * selection's current owner, be we
					 * set it to the current owner. */
    register XSelectionRequestEvent *eventPtr;
					/* Event describing request. */
{
    XSelectionEvent reply;              /* Used to notify requestor that
					 * selection info is ready. */
    int multiple;                       /* Non-zero means a MULTIPLE request
					 * is being handled. */
    IncrInfo incr;                      /* State of selection conversion. */
    Atom singleInfo[2];                 /* incr.multAtoms points here except
					 * for multiple conversions. */
    unsigned i;
    Tk_ErrorHandler errorHandler;
    TkSelectionInfo *infoPtr;
    TkSelInProgress ip;
    ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    errorHandler = Tk_CreateErrorHandler(eventPtr->display, -1, -1,-1,
	    (int (*)()) NULL, (ClientData) NULL);

    /*
     * Initialize the reply event.
     */

    reply.type = SelectionNotify;
    reply.serial = 0;
    reply.send_event = True;
    reply.display = eventPtr->display;
    reply.requestor = eventPtr->requestor;
    reply.selection = eventPtr->selection;
    reply.target = eventPtr->target;
    reply.property = eventPtr->property;
    if (reply.property == None) {
	reply.property = reply.target;
    }
    reply.time = eventPtr->time;

    for (infoPtr = winPtr->dispPtr->selectionInfoPtr; infoPtr != NULL;
	    infoPtr = infoPtr->nextPtr) {
	if (infoPtr->selection == eventPtr->selection)
	    break;
    }
    if (infoPtr == NULL) {
	goto refuse;
    }
    winPtr = (TkWindow *) infoPtr->owner;

    /*
     * Figure out which kind(s) of conversion to perform.  If handling
     * a MULTIPLE conversion, then read the property describing which
     * conversions to perform.
     */

    incr.winPtr = winPtr;
    incr.selection = eventPtr->selection;
    if (eventPtr->target != winPtr->dispPtr->multipleAtom) {
	multiple = 0;
	singleInfo[0] = reply.target;
	singleInfo[1] = reply.property;
	incr.multAtoms = singleInfo;
	incr.numConversions = 1;
    } else {
	Atom type;
	int format, result;
	unsigned long bytesAfter;

	multiple = 1;
	incr.multAtoms = NULL;
	if (eventPtr->property == None) {
	    goto refuse;
	}
	result = XGetWindowProperty(eventPtr->display,
		eventPtr->requestor, eventPtr->property,
		0, MAX_PROP_WORDS, False, XA_ATOM,
		&type, &format, &incr.numConversions, &bytesAfter,
		(unsigned char **) &incr.multAtoms);
	if (result == Success && incr.numConversions == 0 && format == 32 &&
	    type != XA_ATOM && type != None) {
	    result = XGetWindowProperty(eventPtr->display,
		eventPtr->requestor, eventPtr->property,
		0, MAX_PROP_WORDS, False, type,
		&type, &format, &incr.numConversions, &bytesAfter,
		(unsigned char **) &incr.multAtoms);
	}
	if ((result != Success) || (bytesAfter != 0) || (format != 32)
		|| (type == None)) {
	    if (incr.multAtoms != NULL) {
		XFree((char *) incr.multAtoms);
	    }
	    goto refuse;
	}
	incr.numConversions /= 2;               /* Two atoms per conversion. */
    }

    /*
     * Loop through all of the requested conversions, and either return
     * the entire converted selection, if it can be returned in a single
     * bunch, or return INCR information only (the actual selection will
     * be returned below).
     */

    incr.converts = (ConvertInfo *) ckalloc((unsigned)
	    (incr.numConversions*sizeof(ConvertInfo)));
    incr.numIncrs = 0;
    for (i = 0; i < incr.numConversions; i++) {
	Atom target, property, type = XA_STRING;
	long buffer[TK_SEL_WORDS_AT_ONCE];
	register TkSelHandler *selPtr;
	int numItems, format = 8;
	char *propPtr = (char *) buffer;

	target = incr.multAtoms[2*i];
	property = incr.multAtoms[2*i + 1];
	incr.converts[i].offset = -1;
	incr.converts[i].buffer[0] = '\0';

        /* If we get asked for compound text look for string/utf8_string ?
           otherwise cannot see how it would ever get handled
           despite the encoding call below.
           Also presumably TEXT could do the same and convert
           to locale encoding?
         */

	for (selPtr = winPtr->selHandlerList; selPtr != NULL;
		selPtr = selPtr->nextPtr) {
	    if ((selPtr->target == target)
		    && (selPtr->selection == eventPtr->selection)) {
		break;
	    }
	}

	if (selPtr == NULL) {
	    /*
	     * Nobody seems to know about this kind of request.  If
	     * it's of a sort that we can handle without any help, do
	     * it.  Otherwise mark the request as an errror.
	     */

	    numItems = TkSelDefaultSelection(infoPtr, target, buffer,
		    TK_SEL_BYTES_AT_ONCE, &type, &format);
	    if (numItems < 0) {
		incr.multAtoms[2*i + 1] = None;
		LangSelectHook("Request",(Tk_Window) winPtr, infoPtr->selection, target, None);
		continue;
	    }
	} else {
	    ip.selPtr = selPtr;
	    ip.nextPtr = TkSelGetInProgress();
	    TkSelSetInProgress(&ip);
	    type = selPtr->format;
	    if ((type == XA_STRING)
		    || (type==winPtr->dispPtr->utf8Atom)
		    || (type==winPtr->dispPtr->textAtom)
		    || (type==winPtr->dispPtr->compoundTextAtom)) {
		format = 8;
	    } else {
		format = 32;
	    }
	    numItems = (*selPtr->proc)(selPtr->clientData, 0,
		    buffer, TK_SEL_BYTES_AT_ONCE, type, (Tk_Window) winPtr);
	    TkSelSetInProgress(ip.nextPtr);
	    if ((ip.selPtr == NULL) || (numItems < 0)) {
		incr.multAtoms[2*i + 1] = None;
		continue;
	    }
	    if (numItems > TK_SEL_BYTES_AT_ONCE) {
		panic("selection handler returned too many bytes");
	    }
	    ((char *) buffer)[numItems] = '\0';
	}

	/*
	 * Got the selection;  store it back on the requestor's property.
	 */

	if (numItems == TK_SEL_BYTES_AT_ONCE) {
	    /*
	     * Selection is too big to send at once;  start an
	     * INCR-mode transfer.
	     */

	    incr.numIncrs++;
	    type = winPtr->dispPtr->incrAtom;
	    buffer[0] = SelectionSize(selPtr, type, (Tk_Window) winPtr);
	    /* What units is the value we are returning here ?
               SelctionSize counts numItems things which are
               expected to be bytes for Tcl world.
               Size is also size before encoding ...
             */
	    if (buffer[0] == 0) {
		incr.multAtoms[2*i + 1] = None;
		continue;
	    }
	    numItems = 1;
	    propPtr = (char *) buffer;
	    format = 32;
	    incr.converts[i].offset = 0;
	    XChangeProperty(reply.display, reply.requestor,
		    property, type, format, PropModeReplace,
		    (unsigned char *) propPtr, numItems);
	} else if (type == winPtr->dispPtr->utf8Atom) {
	    /*
	     * This matches selection requests of type UTF8_STRING,
	     * which allows us to pass our utf-8 information untouched.
	     */

	    XChangeProperty(reply.display, reply.requestor,
		    property, type, 8, PropModeReplace,
		    (unsigned char *) buffer, numItems);
	} else if ((type == XA_STRING)
		|| (type == winPtr->dispPtr->compoundTextAtom)) {
	    Tcl_DString ds;
	    Tcl_Encoding encoding;

	    /*
	     * STRING is Latin-1, COMPOUND_TEXT is an iso2022 variant.
	     * We need to convert the selection text into these external
	     * forms before modifying the property.
	     */
	

	    if (type == XA_STRING) {
		encoding = Tcl_GetEncoding(NULL, "iso8859-1");
	    } else {
		encoding = Tcl_GetEncoding(NULL, "iso2022");
	    }
	    /* What does this do if it cannot encode ?
               we "should" fail in that case
             */
	    Tcl_UtfToExternalDString(encoding, (char*)buffer, numItems, &ds);
	    XChangeProperty(reply.display, reply.requestor,
		    property, type, 8, PropModeReplace,
		    (unsigned char *) Tcl_DStringValue(&ds),
		    Tcl_DStringLength(&ds));
	    if (encoding) {
		Tcl_FreeEncoding(encoding);
	    }
	    Tcl_DStringFree(&ds);
	} else {
	    /* Put value in format/type provided */
	    XChangeProperty(reply.display, reply.requestor,
		    property, type, format, PropModeReplace,
		    (unsigned char *) buffer, numItems);
	}
    }

    /*
     * Send an event back to the requestor to indicate that the
     * first stage of conversion is complete (everything is done
     * except for long conversions that have to be done in INCR
     * mode).
     */

    if (incr.numIncrs > 0) {
	XSelectInput(reply.display, reply.requestor, PropertyChangeMask);
	incr.timeout = Tcl_CreateTimerHandler(1000, IncrTimeoutProc,
	    (ClientData) &incr);
	incr.idleTime = 0;
	incr.reqWindow = reply.requestor;
	incr.time = infoPtr->time;
	incr.nextPtr = tsdPtr->pendingIncrs;
	tsdPtr->pendingIncrs = &incr;
    }
    if (multiple) {
	XChangeProperty(reply.display, reply.requestor, reply.property,
		XA_ATOM, 32, PropModeReplace,
		(unsigned char *) incr.multAtoms,
		(int) incr.numConversions*2);
    } else {

	/*
	 * Not a MULTIPLE request.  The first property in "multAtoms"
	 * got set to None if there was an error in conversion.
	 */

	reply.property = incr.multAtoms[1];
    }
    XSendEvent(reply.display, reply.requestor, False, 0, (XEvent *) &reply);
    Tk_DeleteErrorHandler(errorHandler);

    /*
     * Handle any remaining INCR-mode transfers.  This all happens
     * in callbacks to TkSelPropProc, so just wait until the number
     * of uncompleted INCR transfers drops to zero.
     */

    if (incr.numIncrs > 0) {
	IncrInfo *incrPtr2;

	while (incr.numIncrs > 0) {
	    Tcl_DoOneEvent(0);
	}
	Tcl_DeleteTimerHandler(incr.timeout);
	/* winPtr may have been destroyed now */
	errorHandler = Tk_CreateErrorHandler(reply.display,
		-1, -1,-1, (int (*)()) NULL, (ClientData) NULL);
	XSelectInput(reply.display, reply.requestor, 0L);
	Tk_DeleteErrorHandler(errorHandler);
	if (tsdPtr->pendingIncrs == &incr) {
	    tsdPtr->pendingIncrs = incr.nextPtr;
	} else {
	    for (incrPtr2 = tsdPtr->pendingIncrs; incrPtr2 != NULL;
		    incrPtr2 = incrPtr2->nextPtr) {
		if (incrPtr2->nextPtr == &incr) {
		    incrPtr2->nextPtr = incr.nextPtr;
		    break;
		}
	    }
	}
    }

    /*
     * All done.  Cleanup and return.
     */

    ckfree((char *) incr.converts);
    if (multiple) {
	XFree((char *) incr.multAtoms);
    }
    return;

    /*
     * An error occurred.  Send back a refusal message.
     */

    refuse:
    reply.property = None;
    XSendEvent(reply.display, reply.requestor, False, 0, (XEvent *) &reply);
    Tk_DeleteErrorHandler(errorHandler);
    return;
}

/*
 *----------------------------------------------------------------------
 *
 * SelRcvIncrProc --
 *
 *      This procedure handles the INCR protocol on the receiving
 *      side.  It is invoked in response to property changes on
 *      the requestor's window (which hopefully are because a new
 *      chunk of the selection arrived).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      If a new piece of selection has arrived, a procedure is
 *      invoked to deal with that piece.  When the whole selection
 *      is here, a flag is left for the higher-level procedure that
 *      initiated the selection retrieval.
 *
 *----------------------------------------------------------------------
 */

static void
SelRcvIncrProc(clientData, eventPtr)
    ClientData clientData;              /* Information about retrieval. */
    register XEvent *eventPtr;          /* X PropertyChange event. */
{
    register TkSelRetrievalInfo *retrPtr = (TkSelRetrievalInfo *) clientData;
    char *propInfo;
    Atom type;
    int format, result;
    unsigned long numItems, bytesAfter;
    Tcl_Interp *interp;

    if ((eventPtr->xproperty.atom != retrPtr->property)
	    || (eventPtr->xproperty.state != PropertyNewValue)
	    || (retrPtr->result != -1)) {
	return;
    }
    propInfo = NULL;
    result = XGetWindowProperty(eventPtr->xproperty.display,
	    eventPtr->xproperty.window, retrPtr->property, 0, MAX_PROP_WORDS,
	    True, (Atom) AnyPropertyType, &type, &format, &numItems,
	    &bytesAfter, (unsigned char **) &propInfo);
    if ((result != Success) || (type == None)) {
	return;
    }
    if (bytesAfter != 0) {
	Tcl_SetResult(retrPtr->interp, "selection property too large",
		TCL_STATIC);
	retrPtr->result = TCL_ERROR;
	goto done;
    }

    LangSelectHook("INCRNotify",(Tk_Window) retrPtr->winPtr,
                    retrPtr->selection, retrPtr->target, type);

    if ((type == XA_STRING)
	    || (type == retrPtr->winPtr->dispPtr->textAtom)
	    || (type == retrPtr->winPtr->dispPtr->utf8Atom)
	    || (type == retrPtr->winPtr->dispPtr->compoundTextAtom)) {
	char *dst, *src;
	int srcLen, dstLen, srcRead, dstWrote, soFar;
	Tcl_Encoding encoding;
	Tcl_DString *dstPtr, temp;

	if (format != 8) {
	    char buf[64 + TCL_INTEGER_SPACE];

	    sprintf(buf,
		    "bad format for string selection: wanted \"8\", got \"%d\"",
		    format);
	    Tcl_SetResult(retrPtr->interp, buf, TCL_VOLATILE);
	    retrPtr->result = TCL_ERROR;
	    goto done;
	}
	interp = retrPtr->interp;
	Tcl_Preserve((ClientData) interp);

	if (type == retrPtr->winPtr->dispPtr->compoundTextAtom) {
	    encoding = Tcl_GetEncoding(NULL, "iso2022");
	} else if (type == retrPtr->winPtr->dispPtr->utf8Atom) {
	    /* We use encoding even though we have data as UTF-8 as code
               below handles partial characters at buffer boundaries
             */
	    encoding = Tcl_GetEncoding(NULL, "utf-8");
	} else {
	    encoding = Tcl_GetEncoding(NULL, "iso8859-1");
	}

	/*
	 * Check to see if there is any data left over from the previous
	 * chunk.  If there is, copy the old data and the new data into
	 * a new buffer.
	 */

	Tcl_DStringInit(&temp);
	if (Tcl_DStringLength(&retrPtr->buf) > 0) {
	    Tcl_DStringAppend(&temp, Tcl_DStringValue(&retrPtr->buf),
		    Tcl_DStringLength(&retrPtr->buf));
	    if (numItems > 0) {
		Tcl_DStringAppend(&temp, propInfo, (int)numItems);
	    }
	    src = Tcl_DStringValue(&temp);
	    srcLen = Tcl_DStringLength(&temp);
	} else if (numItems == 0) {
	    /*
	     * There is no new data, so we're done.
	     */

	    retrPtr->result = TCL_OK;
	    Tcl_Release((ClientData) interp);
	    goto done;
	} else {
	    src = propInfo;
	    srcLen = numItems;
	}

	/*
	 * Set up the destination buffer so we can use as much space as
	 * is available.
	 */

	dstPtr = &retrPtr->buf;
	Tcl_DStringSetLength(dstPtr,2*numItems);
	Tcl_DStringSetLength(dstPtr,0);
	dst = Tcl_DStringValue(dstPtr);
	dstLen = 2*numItems;

	/*
	 * Now convert the data, growing the destination buffer as needed.
	 */

	while (1) {
	    result = Tcl_ExternalToUtf(NULL, encoding, src, srcLen,
		    retrPtr->encFlags, &retrPtr->encState,
		    dst, dstLen, &srcRead, &dstWrote, NULL);
	    soFar = dst + dstWrote - Tcl_DStringValue(dstPtr);
	    retrPtr->encFlags &= ~TCL_ENCODING_START;
	    src += srcRead;
	    srcLen -= srcRead;
	    if (result != TCL_CONVERT_NOSPACE) {
		Tcl_DStringSetLength(dstPtr, soFar);
		break;
	    }
	    if (Tcl_DStringLength(dstPtr) == 0) {
		Tcl_DStringSetLength(dstPtr, dstLen);
	    }
	    Tcl_DStringSetLength(dstPtr, 2 * Tcl_DStringLength(dstPtr) + 1);
	    dst = Tcl_DStringValue(dstPtr) + soFar;
	    dstLen = Tcl_DStringLength(dstPtr) - soFar - 1;
	}
	Tcl_DStringSetLength(dstPtr, soFar);

	if (retrPtr->winPtr->dispPtr->utf8Atom) {
	    type = retrPtr->winPtr->dispPtr->utf8Atom;
	}

	result = (*retrPtr->proc)(retrPtr->clientData, interp,
		(long *) Tcl_DStringValue(dstPtr), Tcl_DStringLength(dstPtr),
		format, type, (Tk_Window) retrPtr->winPtr);
	Tcl_Release((ClientData) interp);

	/*
	 * Copy any unused data into the destination buffer so we can
	 * pick it up next time around.
	 */

	Tcl_DStringSetLength(dstPtr, 0);
	Tcl_DStringAppend(dstPtr, src, srcLen);

	Tcl_DStringFree(&temp);
	if (encoding) {
	    Tcl_FreeEncoding(encoding);
	}
	if (result != TCL_OK) {
	    retrPtr->result = result;
	}
    } else if (numItems == 0) {
	retrPtr->result = TCL_OK;
    } else {
#ifndef _LANG
	char *string;

	if (format != 32) {
	    char buf[64 + TCL_INTEGER_SPACE];

	    sprintf(buf,
		    "bad format for selection: wanted \"32\", got \"%d\"",
		    format);
	    Tcl_SetResult(retrPtr->interp, buf, TCL_VOLATILE);
	    retrPtr->result = TCL_ERROR;
	    goto done;
	}
	string = TkSelCvtFromX((long *) propInfo, (int) numItems, type,
		(Tk_Window) retrPtr->winPtr);
#endif
	interp = retrPtr->interp;
	Tcl_Preserve((ClientData) interp);
	result = (*retrPtr->proc)(retrPtr->clientData, interp,
		(long *) propInfo, (int) numItems, format, type, (Tk_Window) retrPtr->winPtr);
	Tcl_Release((ClientData) interp);
	if (result != TCL_OK) {
	    retrPtr->result = result;
	}
    }

    done:
    XFree((char *) propInfo);
    retrPtr->idleTime = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * SelectionSize --
 *
 *      This procedure is called when the selection is too large to
 *      send in a single buffer;  it computes the total length of
 *      the selection in bytes.
 *
 * Results:
 *      The return value is the number of bytes in the selection
 *      given by selPtr.
 *
 * Side effects:
 *      The selection is retrieved from its current owner (this is
 *      the only way to compute its size).
 *
 *----------------------------------------------------------------------
 */

static int
SelectionSize(selPtr, type, tkwin)
    TkSelHandler *selPtr;       /* Information about how to retrieve
				 * the selection whose size is wanted. */
    Atom type;
    Tk_Window tkwin;
{
    long buffer[TK_SEL_WORDS_AT_ONCE];
    int size, chunkSize;
    TkSelInProgress ip;

    size = TK_SEL_BYTES_AT_ONCE;
    ip.selPtr = selPtr;
    ip.nextPtr = TkSelGetInProgress();
    TkSelSetInProgress(&ip);
    do {
	chunkSize = (*selPtr->proc)(selPtr->clientData, size,
			buffer, TK_SEL_BYTES_AT_ONCE, type, tkwin);
	if (ip.selPtr == NULL) {
	    size = 0;
	    break;
	}
	size += chunkSize;
    } while (chunkSize == TK_SEL_BYTES_AT_ONCE);
    TkSelSetInProgress(ip.nextPtr);
    return size;
}

/*
 *----------------------------------------------------------------------
 *
 * IncrTimeoutProc --
 *
 *      This procedure is invoked once a second while sending the
 *      selection to a requestor in INCR mode.  After a while it
 *      gives up and aborts the selection operation.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      A new timeout gets registered so that this procedure gets
 *      called again in another second, unless too many seconds
 *      have elapsed, in which case incrPtr is marked as "all done".
 *
 *----------------------------------------------------------------------
 */

static void
IncrTimeoutProc(clientData)
    ClientData clientData;              /* Information about INCR-mode
					 * selection retrieval for which
					 * we are selection owner. */
{
    register IncrInfo *incrPtr = (IncrInfo *) clientData;

    incrPtr->idleTime++;
    if (incrPtr->idleTime >= 5) {
	incrPtr->numIncrs = 0;
    } else {
	incrPtr->timeout = Tcl_CreateTimerHandler(1000, IncrTimeoutProc,
		(ClientData) incrPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SelCvtToX --
 *
 *      Given a selection represented as a string (the normal Tcl form),
 *      convert it to the ICCCM-mandated format for X, depending on
 *      the type argument.  This procedure and SelCvtFromX are inverses.
 *
 * Results:
 *      The return value is a malloc'ed buffer holding a value
 *      equivalent to "string", but formatted as for "type".  It is
 *      the caller's responsibility to free the string when done with
 *      it.  The word at *numLongsPtr is filled in with the number of
 *      32-bit words returned in the result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
TkSelCvtToX(propPtr, string, type, tkwin, maxBytes)
    long *propPtr;
    char *string;               /* String representation of selection. */
    Atom type;                  /* Atom specifying the X format that is
				 * desired for the selection.  Should not
				 * be XA_STRING (if so, don't bother calling
				 * this procedure at all). */
    Tk_Window tkwin;            /* Window that governs atom conversion. */
    int maxBytes;               /* Number of 32-bit words contained in the
				 * result. */
{
    register char *p;
    char *field;
    int numFields;
    int bytes;
    long *longPtr;
#define MAX_ATOM_NAME_LENGTH 100
    char atomName[MAX_ATOM_NAME_LENGTH+1];

    /*
     * The string is assumed to consist of fields separated by spaces.
     * The property gets generated by converting each field to an
     * integer number, in one of two ways:
     * 1. If type is XA_ATOM, convert each field to its corresponding
     *    atom.
     * 2. If type is anything else, convert each field from an ASCII number
     *    to a 32-bit binary number.
     */

    numFields = 1;
    for (p = string; *p != 0; p++) {
	if (isspace(UCHAR(*p))) {
	    numFields++;
	}
    }

    /*
     * Convert the fields one-by-one.
     */

    for (longPtr = propPtr, bytes = 0, p = string; bytes < maxBytes
	    ; bytes += sizeof(long), longPtr++) {
	while (isspace(UCHAR(*p))) {
	    p++;
	}
	if (*p == 0) {
	    break;
	}
	field = p;
	while ((*p != 0) && !isspace(UCHAR(*p))) {
	    p++;
	}
	if (type == XA_ATOM) {
	    int length;

	    length = p - field;
	    if (length > MAX_ATOM_NAME_LENGTH) {
		length = MAX_ATOM_NAME_LENGTH;
	    }
	    strncpy(atomName, field, (unsigned) length);
	    atomName[length] = 0;
	    *longPtr = (long) Tk_InternAtom(tkwin, atomName);
	} else {
	    char *dummy;

	    *longPtr = strtol(field, &dummy, 0);
	}
    }
    return bytes / sizeof(long);
}

/*
 *----------------------------------------------------------------------
 *
 * SelCvtFromX --
 *
 *      Given an X property value, formatted as a collection of 32-bit
 *      values according to "type" and the ICCCM conventions, convert
 *      the value to a string suitable for manipulation by Tcl.  This
 *      procedure is the inverse of SelCvtToX.
 *
 * Results:
 *      The return value is the string equivalent of "property".  It is
 *      malloc-ed and should be freed by the caller when no longer
 *      needed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
TkSelCvtFromX(propPtr, numValues, type, tkwin)
    register long *propPtr;     /* Property value from X. */
    int numValues;              /* Number of 32-bit values in property. */
    Atom type;                  /* Type of property  Should not be
				 * XA_STRING (if so, don't bother calling
				 * this procedure at all). */
    Tk_Window tkwin;            /* Window to use for atom conversion. */
{
    char *result;
    int resultSpace, curSize, fieldSize;
    CONST char *atomName;

    /*
     * Convert each long in the property to a string value, which is
     * either the name of an atom (if type is XA_ATOM) or a hexadecimal
     * string.  Make an initial guess about the size of the result, but
     * be prepared to enlarge the result if necessary.
     */

    resultSpace = 12*numValues+1;
    curSize = 0;
    atomName = "";      /* Not needed, but eliminates compiler warning. */
    result = (char *) ckalloc((unsigned) resultSpace);
    *result  = '\0';
    for ( ; numValues > 0; propPtr++, numValues--) {
	if (type == XA_ATOM) {
	    atomName = Tk_GetAtomName(tkwin, (Atom) *propPtr);
	    fieldSize = strlen(atomName) + 1;
	} else {
	    fieldSize = 12;
	}
	if (curSize+fieldSize >= resultSpace) {
	    char *newResult;

	    resultSpace *= 2;
	    if (curSize+fieldSize >= resultSpace) {
		resultSpace = curSize + fieldSize + 1;
	    }
	    newResult = (char *) ckalloc((unsigned) resultSpace);
	    strncpy(newResult, result, (unsigned) curSize);
	    ckfree(result);
	    result = newResult;
	}
	if (curSize != 0) {
	    result[curSize] = ' ';
	    curSize++;
	}
	if (type == XA_ATOM) {
	    strcpy(result+curSize, atomName);
	} else {
	    sprintf(result+curSize, "0x%x", (unsigned int) *propPtr);
	}
	curSize += strlen(result+curSize);
    }
    return result;
}


