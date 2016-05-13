/*******************************************************************************
*                                                                              *
* window.c -- Nirvana Editor window creation/deletion                          *
*                                                                              *
* Copyright (C) 1999 Mark Edel                                                 *
*                                                                              *
* This is free software; you can redistribute it and/or modify it under the    *
* terms of the GNU General Public License as published by the Free Software    *
* Foundation; either version 2 of the License, or (at your option) any later   *
* version. In addition, you may distribute version of this program linked to   *
* Motif or Open Motif. See README for details.                                 *
*                                                                              *
* This software is distributed in the hope that it will be useful, but WITHOUT *
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or        *
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License        *
* for more details.                                                            *
*                                                                              *
* You should have received a copy of the GNU General Public License along with *
* software; if not, write to the Free Software Foundation, Inc., 59 Temple     *
* Place, Suite 330, Boston, MA  02111-1307 USA                                 *
*                                                                              *
* Nirvana Text Editor                                                          *
* May 10, 1991                                                                 *
*                                                                              *
* Written by Mark Edel                                                         *
*                                                                              *
*******************************************************************************/

#include <QApplication>
#include <QStringList>
#include <QString>

#include "window.h"
#include "TextBuffer.h"
#include "textSel.h"
#include "text.h"
#include "Document.h"
#include "textP.h"
#include "UndoInfo.h"
#include "nedit.h"
#include "menu.h"
#include "file.h"
#include "search.h"
#include "preferences.h"
#include "selection.h"
#include "server.h"
#include "shell.h"
#include "macro.h"
#include "highlight.h"
#include "smartIndent.h"
#include "userCmds.h"
#include "res/n.bm"
#include "interpret.h"
#include "RangesetTable.h"
#include "Rangeset.h"
#include "MotifHelper.h"
#include "../util/clearcase.h"
#include "../util/misc.h"
#include "../util/fileUtils.h"
#include "../util/utils.h"
#include "../util/fileUtils.h"
#include "../Xlt/BubbleButtonP.h"
#include "../Microline/XmL/Folder.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <ctime>
#include <sys/stat.h>
#include <sys/time.h>
#include <vector>

#include <X11/Shell.h>
#include <X11/Xatom.h>
#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/PanedW.h>
#include <Xm/PanedWP.h>
#include <Xm/RowColumnP.h>
#include <Xm/Separator.h>
#include <Xm/Text.h>
#include <Xm/ToggleB.h>
#include <Xm/PushB.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Label.h>
#include <Xm/SelectioB.h>
#include <Xm/List.h>
#include <Xm/Protocols.h>
#include <Xm/ScrolledW.h>
#include <Xm/ScrollBar.h>
#include <Xm/PrimitiveP.h>
#include <Xm/Frame.h>
#include <Xm/CascadeB.h>

#ifdef EDITRES
#include <X11/Xmu/Editres.h>
// extern void _XEditResCheckMessages(); 
#endif

extern void _XmDismissTearOff(Widget, XtPointer, XtPointer);

static void saveYourselfCB(Widget w, XtPointer clientData, XtPointer callData);

/*
** find which document a tab belongs to
*/
Document *TabToWindow(Widget tab) {

	return Document::find_if([tab](Document *win) {
		return win->tab_ == tab;
	});
}


/*
** Check if there is already a window open for a given file
*/
Document *FindWindowWithFile(const char *name, const char *path) {

	/* I don't think this algorithm will work on vms so I am
	   disabling it for now */
	if (!GetPrefHonorSymlinks()) {
		struct stat attribute;
		
		QString fullname = QString(QLatin1String("%1%2")).arg(QLatin1String(path), QLatin1String(name));
		

		if (stat(fullname.toLatin1().data(), &attribute) == 0) {
		
			Document *window = Document::find_if([attribute](Document *window) {
				return attribute.st_dev == window->device_ && attribute.st_ino == window->inode_;
			});
			
			if(window) {
				return window;
			}
		} /*  else:  Not an error condition, just a new file. Continue to check
		      whether the filename is already in use for an unsaved document.  */
	}
		
	Document *window = Document::find_if([name, path](Document *window) {
		return (window->filename_ == QLatin1String(name)) && (window->path_ == QLatin1String(path));
	});

	return window;
}

/*
** Count the windows
*/
int NWindows(void) {
	return Document::WindowCount();
}




/*
** Find the start and end of a single line selection.  Hides rectangular
** selection issues for older routines which use selections that won't
** span lines.
*/
bool TextBuffer::GetSimpleSelection(int *left, int *right) {
	int selStart, selEnd, rectStart, rectEnd;
	bool isRect;

	/* get the character to match and its position from the selection, or
	   the character before the insert point if nothing is selected.
	   Give up if too many characters are selected */
	if (!this->BufGetSelectionPos(&selStart, &selEnd, &isRect, &rectStart, &rectEnd)) {
		return false;
	}
	
	if (isRect) {
		int lineStart = this->BufStartOfLine(selStart);
		selStart  = this->BufCountForwardDispChars(lineStart, rectStart);
		selEnd    = this->BufCountForwardDispChars(lineStart, rectEnd);
	}
	
	*left  = selStart;
	*right = selEnd;
	return true;
}

#ifndef NO_SESSION_RESTART
static void saveYourselfCB(Widget w, XtPointer clientData, XtPointer callData) {

	auto appShell = static_cast<Widget>(clientData);
	
	(void)w;
	(void)callData;

	char geometry[MAX_GEOM_STRING_LEN];
	int wasIconic = False;
	int nItems;
	WidgetList children;

	/* Allocate memory for an argument list and for a reversed list of
	   windows.  The window list is reversed for IRIX 4DWM and any other
	   window/session manager combination which uses window creation
	   order for re-associating stored geometry information with
	   new windows created by a restored application */
	int nWindows = Document::WindowCount();;
	
	auto revWindowList = new Document *[nWindows];
	int i = nWindows - 1;
	
	
	for(Document *win: WindowList) {
		revWindowList[i] = win;
		--i;
	}

	// Create command line arguments for restoring each window in the list 
	std::vector<char *> argv;
	argv.push_back(XtNewStringEx(qApp->arguments()[0]));
	
	if (IsServer) {
		argv.push_back(XtNewStringEx("-server"));
		
		if (GetPrefServerName()[0] != '\0') {
			argv.push_back(XtNewStringEx("-svrname"));
			argv.push_back(XtNewStringEx(GetPrefServerName()));
		}
	}

	// editor windows are popup-shell children of top-level appShell 
	XtVaGetValues(appShell, XmNchildren, &children, XmNnumChildren, &nItems, nullptr);

	for (int n = nItems - 1; n >= 0; n--) {
		WidgetList tabs;
		int tabCount;
		Document *topWin;
		if (strcmp(XtName(children[n]), "textShell") || ((topWin = Document::WidgetToWindow(children[n])) == nullptr)) {
			continue; // skip non-editor windows 
		}

		// create a group for each window 
		topWin->getGeometryString(geometry);
		argv.push_back(XtNewStringEx("-group"));
		argv.push_back(XtNewStringEx("-geometry"));
		argv.push_back(XtNewStringEx(geometry));
		
		if (topWin->IsIconic()) {
			argv.push_back(XtNewStringEx("-iconic"));
			wasIconic = True;
		} else if (wasIconic) {
			argv.push_back(XtNewStringEx("-noiconic"));
			wasIconic = False;
		}

		// add filename of each tab in window... 
		XtVaGetValues(topWin->tabBar_, XmNtabWidgetList, &tabs, XmNtabCount, &tabCount, nullptr);

		for (int i = 0; i < tabCount; i++) {
			Document *win = TabToWindow(tabs[i]);
			if (win->filenameSet_) {
				// add filename 
				argv.push_back(XtNewStringEx(win->FullPath()));
			}
		}
	}

	delete [] revWindowList;

	// Set the window's WM_COMMAND property to the created command line 
	XSetCommand(TheDisplay, XtWindow(appShell), &argv[0], argv.size());
	
	for (char *p : argv) {
		XtFree(p);
	}
}

void AttachSessionMgrHandler(Widget appShell) {
	static Atom wmpAtom, syAtom = 0;

	/* Add wm protocol callback for making nedit restartable by session
	   managers.  Doesn't yet handle multiple-desktops or iconifying right. */
	if (syAtom == 0) {
		wmpAtom = XmInternAtom(TheDisplay, (String) "WM_PROTOCOLS", FALSE);
		syAtom = XmInternAtom(TheDisplay, (String) "WM_SAVE_YOURSELF", FALSE);
	}
	
	XmAddProtocolCallback(appShell, wmpAtom, syAtom, saveYourselfCB, (XtPointer)appShell);
}
#endif // NO_SESSION_RESTART 


static bool currentlyBusy = false;
static long busyStartTime = 0;
static bool modeMessageSet = false;

/*
 * Auxiliary function for measuring elapsed time during busy waits.
 */
static long getRelTimeInTenthsOfSeconds() {
	struct timeval current;
	gettimeofday(&current, nullptr);
	return (current.tv_sec * 10 + current.tv_usec / 100000) & 0xFFFFFFFL;
}

void AllWindowsBusy(const char *message) {

	if (!currentlyBusy) {
		busyStartTime = getRelTimeInTenthsOfSeconds();
		modeMessageSet = False;

		for(Document *w: WindowList) {
			/* We don't the display message here yet, but defer it for
			   a while. If the wait is short, we don't want
			   to have it flash on and off the screen.  However,
			   we can't use a time since in generally we are in
			   a tight loop and only processing exposure events, so it's
			   up to the caller to make sure that this routine is called
			   at regular intervals.
			*/
			BeginWait(w->shell_);
		}
		
	} else if (!modeMessageSet && message && getRelTimeInTenthsOfSeconds() - busyStartTime > 10) {
		// Show the mode message when we've been busy for more than a second 
		
		for(Document *w: WindowList) {
			w->SetModeMessage(message);
		}
		modeMessageSet = True;
	}
	BusyWait(WindowList->shell_);

	currentlyBusy = True;
}

void AllWindowsUnbusy(void) {

	for(Document *w: WindowList) {
		w->ClearModeMessage();
		EndWait(w->shell_);
	}

	currentlyBusy = False;
	modeMessageSet = False;
	busyStartTime = 0;
}

void AddSmallIcon(Widget shell) {
	static Pixmap iconPixmap = 0, maskPixmap = 0;

	if (iconPixmap == 0) {
		iconPixmap = XCreateBitmapFromData(TheDisplay, RootWindowOfScreen(XtScreen(shell)), (char *)n_bits, n_width, n_height);
		maskPixmap = XCreateBitmapFromData(TheDisplay, RootWindowOfScreen(XtScreen(shell)), (char *)n_mask, n_width, n_height);
	}
	XtVaSetValues(shell, XmNiconPixmap, iconPixmap, XmNiconMask, maskPixmap, nullptr);
}



