/*******************************************************************************
*                                                                              *
* search.c -- Nirvana Editor search and replace functions                      *
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

#include "search.h"
#include "DialogFind.h"
#include "DialogMultiReplace.h"
#include "DialogReplace.h"
#include "DocumentWidget.h"
#include "MainWindow.h"
#include "SignalBlocker.h"
#include "TextArea.h"
#include "TextBuffer.h"
#include "WrapStyle.h"
#include "file.h"
#include "highlight.h"
#include "nedit.h"
#include "preferences.h"
#include "regularExp.h"
#include "selection.h"
#include "userCmds.h"
#include <QApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QMimeData>
#include <QTimer>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/param.h>

int NHist = 0;

struct SearchSelectedCallData {
	SearchDirection direction;
	SearchType searchType;
	int searchWrap;
};


// History mechanism for search and replace strings 
QString SearchHistory[MAX_SEARCH_HISTORY];
QString ReplaceHistory[MAX_SEARCH_HISTORY];
SearchType SearchTypeHistory[MAX_SEARCH_HISTORY];
static int HistStart = 0;

static bool backwardRegexSearch(view::string_view string, view::string_view searchString, bool wrap, int beginPos, int *startPos, int *endPos, int *searchExtentBW, int *searchExtentFW, const char *delimiters, int defaultFlags);
static bool replaceUsingREEx(view::string_view searchStr, const char *replaceStr, view::string_view sourceStr, int beginPos, char *destStr, int maxDestLen, int prevChar, const char *delimiters, int defaultFlags);

static bool forwardRegexSearch(view::string_view string, view::string_view searchString, bool wrap, int beginPos, int *startPos, int *endPos, int *searchExtentBW, int *searchExtentFW, const char *delimiters, int defaultFlags);
static bool searchRegex(view::string_view string, view::string_view searchString, SearchDirection direction, bool wrap, int beginPos, int *startPos, int *endPos, int *searchExtentBW, int *searchExtentFW, const char *delimiters, int defaultFlags);
static int countWindows(void);
static bool searchLiteral(view::string_view string, view::string_view searchString, bool caseSense, SearchDirection direction, bool wrap, int beginPos, int *startPos, int *endPos, int *searchExtentBW, int *searchExtentFW);
static bool searchLiteralWord(view::string_view string, view::string_view searchString, bool caseSense, SearchDirection direction, bool wrap, int beginPos, int *startPos, int *endPos, const char *delimiters);
static bool searchMatchesSelectionEx(DocumentWidget *window, const QString &searchString, SearchType searchType, int *left, int *right, int *searchExtentBW, int *searchExtentFW);
static void iSearchRecordLastBeginPosEx(MainWindow *window, SearchDirection direction, int initPos);
static void iSearchTryBeepOnWrapEx(MainWindow *window, SearchDirection direction, int beginPos, int startPos);
static std::string upCaseStringEx(view::string_view inString);
static std::string downCaseStringEx(view::string_view inString);
static bool findMatchingCharEx(DocumentWidget *window, char toMatch, void *styleToMatch, int charPos, int startLimit, int endLimit, int *matchPos);


struct CharMatchTable {
	char c;
	char match;
	char direction;
};

#define N_MATCH_CHARS 13
#define N_FLASH_CHARS 6
static CharMatchTable MatchingChars[N_MATCH_CHARS] = {
    {'{', '}', SEARCH_FORWARD},
    {'}', '{', SEARCH_BACKWARD},
    {'(', ')', SEARCH_FORWARD},
    {')', '(', SEARCH_BACKWARD},
    {'[', ']', SEARCH_FORWARD},
    {']', '[', SEARCH_BACKWARD},
    {'<', '>', SEARCH_FORWARD},
    {'>', '<', SEARCH_BACKWARD},
    {'/', '/', SEARCH_FORWARD},
    {'"', '"', SEARCH_FORWARD},
    {'\'', '\'', SEARCH_FORWARD},
    {'`', '`', SEARCH_FORWARD},
    {'\\', '\\', SEARCH_FORWARD},
};

/*
** Definitions for the search method strings, used as arguments for
** macro search subroutines and search action routines
*/
static const char *searchTypeStrings[] = {"literal",     // SEARCH_LITERAL         
                                          "case",        // SEARCH_CASE_SENSE      
                                          "regex",       // SEARCH_REGEX           
                                          "word",        // SEARCH_LITERAL_WORD    
                                          "caseWord",    // SEARCH_CASE_SENSE_WORD 
                                          "regexNoCase", // SEARCH_REGEX_NOCASE    
                                          nullptr};

#ifdef REPLACE_SCOPE
/*
** Checks whether a selection spans multiple lines. Used to decide on the
** default scope for replace dialogs.
** This routine introduces a dependency on TextDisplay.h, which is not so nice,
** but I currently don't have a cleaner solution.
*/
static bool selectionSpansMultipleLines(Document *window) {
	int selStart;
	int selEnd;
	int rectStart;
	int rectEnd;
	int lineStartStart;
	int lineStartEnd;
	bool isRect;
	int lineWidth;

	if (!window->buffer_->BufGetSelectionPos(&selStart, &selEnd, &isRect, &rectStart, &rectEnd)) {
        return false;
	}

	/* This is kind of tricky. The perception of a line depends on the
	   line wrap mode being used. So in theory, we should take into
	   account the layout of the text on the screen. However, the
	   routine to calculate a line number for a given character position
	   (TextDPosToLineAndCol) only works for displayed lines, so we cannot
	   use it. Therefore, we use this simple heuristic:
	    - If a newline is found between the start and end of the selection,
	  we obviously have a multi-line selection.
	- If no newline is found, but the distance between the start and the
	      end of the selection is larger than the number of characters
	  displayed on a line, and we're in continuous wrap mode,
	  we also assume a multi-line selection.
	*/

	lineStartStart = window->buffer_->BufStartOfLine(selStart);
	lineStartEnd = window->buffer_->BufStartOfLine(selEnd);
	// If the line starts differ, we have a "\n" in between. 
	if (lineStartStart != lineStartEnd) {
		return true;
	}

	if (window->wrapMode_ != CONTINUOUS_WRAP) {
        return false; // Same line
	}

	// Estimate the number of characters on a line 
	TextDisplay *textD = textD_of(window->textArea_);
	if (textD->TextDGetFont()->max_bounds.width > 0) {
		lineWidth = textD->getRect().width / textD->TextDGetFont()->max_bounds.width;
	} else {
		lineWidth = 1;
	}

	if (lineWidth < 1) {
		lineWidth = 1; // Just in case 
	}

	/* Estimate the numbers of line breaks from the start of the line to
	   the start and ending positions of the selection and compare.*/
	if ((selStart - lineStartStart) / lineWidth != (selEnd - lineStartStart) / lineWidth) {
		return true; // Spans multiple lines
	}

    return false; // Small selection; probably doesn't span lines
}
#endif

void DoFindReplaceDlogEx(MainWindow *window, DocumentWidget *document, TextArea *area, SearchDirection direction, int keepDialogs, SearchType searchType) {

    Q_UNUSED(area);

    // Create the dialog if it doesn't already exist
    if (!window->dialogReplace_) {
        window->dialogReplace_ = new DialogReplace(window, document, window);
    }

    auto dialog = window->getDialogReplace();

    dialog->setTextField(document);

    // If the window is already up, just pop it to the top
    if(dialog->isVisible()) {
        dialog->raise();
        dialog->activateWindow();
        return;
    }

    // Blank the Replace with field
    dialog->ui.textReplace->setText(QString());

    // Set the initial search type
    dialog->initToggleButtons(searchType);

    // Set the initial direction based on the direction argument
    dialog->ui.checkBackward->setChecked(direction == SEARCH_FORWARD ? false : true);

    // Set the state of the Keep Dialog Up button
    dialog->ui.checkKeep->setChecked(keepDialogs);

#if defined(REPLACE_SCOPE) || 0
    /* Set the state of the scope radio buttons to "In Window".
       Notify to make sure that callbacks are called.
       NOTE: due to an apparent bug in OpenMotif, the radio buttons may
       get stuck after resetting the scope to "In Window". Therefore we must
       use RadioButtonChangeState(), which contains a workaround. */
    if (document->wasSelected_) {
        /* If a selection exists, the default scope depends on the preference
               of the user. */
        switch (GetPrefReplaceDefScope()) {
        case REPL_DEF_SCOPE_SELECTION:
            /* The user prefers selection scope, no matter what the
               size of the selection is. */
            dialog->ui.radioSelection->setChecked(true);
            break;
        case REPL_DEF_SCOPE_SMART:
            if (selectionSpansMultipleLines(window)) {
                /* If the selection spans multiple lines, the user most
                   likely wants to perform a replacement in the selection */
                dialog->ui.radioSelection->setChecked(true);
            } else {
                /* It's unlikely that the user wants a replacement in a
                   tiny selection only. */
                dialog->ui.radioWindow->setChecked(true);
            }
            break;
        default:
            // The user always wants window scope as default.
            dialog->ui.radioWindow->setChecked(true);
            break;
        }
    } else {
        // No selection -> always choose "In Window" as default.
        dialog->ui.radioWindow->setChecked(true);
    }
#endif

    dialog->UpdateReplaceActionButtons();

    // Start the search history mechanism at the current history item
    window->rHistIndex_ = 0;

    // TODO(eteran): center it on the cursor if settings say so
    dialog->show();
}

void DoFindDlogEx(MainWindow *window, DocumentWidget *document, SearchDirection direction, int keepDialogs, SearchType searchType) {

    if(!window->dialogFind_) {
        window->dialogFind_ = new DialogFind(window, document, window);
    }

    auto dialog = qobject_cast<DialogFind *>(window->dialogFind_);

    dialog->setTextField(document);

    if(dialog->isVisible()) {
        dialog->raise();
        dialog->activateWindow();
        return;
    }

    // Set the initial search type
    dialog->initToggleButtons(searchType);

    // Set the initial direction based on the direction argument
    dialog->ui.checkBackward->setChecked(direction == SEARCH_FORWARD ? false : true);

    // Set the state of the Keep Dialog Up button
    dialog->ui.checkKeep->setChecked(keepDialogs);

    // Set the state of the Find button
    dialog->fUpdateActionButtons();

    // start the search history mechanism at the current history item
    window->fHistIndex_ = 0;

    // TODO(eteran): center it on the cursor if settings say so
    dialog->show();
}

/*
** Count no. of windows
*/
static int countWindows() {
    return DocumentWidget::allDocuments().size();
}

/*
** Count no. of writable windows, but first update the status of all files.
*/
int countWritableWindows() {
	int nAfter;

	int nBefore = countWindows();
    int nWritable = 0;

    QList<DocumentWidget *> documents = DocumentWidget::allDocuments();
    auto first = documents.begin();
    auto last = documents.end();

    for(auto it = first; it != last; ++it) {
        DocumentWidget *w = *it;

		/* We must be very careful! The status check may trigger a pop-up
		   dialog when the file has changed on disk, and the user may destroy
		   arbitrary windows in response. */
        w->CheckForChangesToFileEx();
        nAfter = countWindows();

		if (nAfter != nBefore) {
			// The user has destroyed a file; start counting all over again 
			nBefore = nAfter;

            documents = DocumentWidget::allDocuments();
            first = documents.begin();
            last = documents.end();
            it = first;
			nWritable = 0;
			continue;
		}
		
		if (!w->lockReasons_.isAnyLocked()) {
			++nWritable;
		}
	}

	return nWritable;
}

/*
** Fetch and verify (particularly regular expression) search string,
** direction, and search type from the Find dialog.  If the search string
** is ok, save a copy in the search history, copy it to "searchString",
** which is assumed to be at least SEARCHMAX in length, return search type
** in "searchType", and return TRUE as the function value.  Otherwise,
** return FALSE.
*/
bool SearchAndSelectSameEx(MainWindow *window, DocumentWidget *document, TextArea *area, SearchDirection direction, bool searchWrap) {
    if (NHist < 1) {
        QApplication::beep();
        return false;
    }

    return SearchAndSelectEx(window, document, area, direction, SearchHistory[historyIndex(1)], SearchTypeHistory[historyIndex(1)], searchWrap);
}

/*
** Search for "searchString" in "window", and select the matching text in
** the window when found (or beep or put up a dialog if not found).  Also
** adds the search string to the global search history.
*/
bool SearchAndSelectEx(MainWindow *window, DocumentWidget *document, TextArea *area, SearchDirection direction, const QString &searchString, SearchType searchType, int searchWrap) {
    int startPos;
    int endPos;
    int beginPos;
    int cursorPos;
    int selStart;
    int selEnd;
    int movedFwd = 0;

    // Save a copy of searchString in the search history
    saveSearchHistory(searchString, QString(), searchType, false);

    /* set the position to start the search so we don't find the same
       string that was found on the last search	*/
    if (searchMatchesSelectionEx(document, searchString, searchType, &selStart, &selEnd, nullptr, nullptr)) {
        // selection matches search string, start before or after sel.
        if (direction == SEARCH_BACKWARD) {
            beginPos = selStart - 1;
        } else {
            beginPos = selStart + 1;
            movedFwd = 1;
        }
    } else {
        selStart = -1;
        selEnd = -1;
        // no selection, or no match, search relative cursor

        cursorPos = area->TextGetCursorPos();
        if (direction == SEARCH_BACKWARD) {
            // use the insert position - 1 for backward searches
            beginPos = cursorPos - 1;
        } else {
            // use the insert position for forward searches
            beginPos = cursorPos;
        }
    }

    /* when the i-search bar is active and search is repeated there
       (Return), the action "find" is called (not: "find_incremental").
       "find" calls this function SearchAndSelect.
       To keep track of the iSearchLastBeginPos correctly in the
       repeated i-search case it is necessary to call the following
       function here, otherwise there are no beeps on the repeated
       incremental search wraps.  */
    iSearchRecordLastBeginPosEx(window, direction, beginPos);

    // do the search.  SearchWindow does appropriate dialogs and beeps
    if (!SearchWindowEx(window, document, direction, searchString, searchType, searchWrap, beginPos, &startPos, &endPos, nullptr, nullptr))
        return false;

    /* if the search matched an empty string (possible with regular exps)
       beginning at the start of the search, go to the next occurrence,
       otherwise repeated finds will get "stuck" at zero-length matches */
    if (direction == SEARCH_FORWARD && beginPos == startPos && beginPos == endPos) {
        if (!movedFwd && !SearchWindowEx(window, document, direction, searchString, searchType, searchWrap, beginPos + 1, &startPos, &endPos, nullptr, nullptr))
            return false;
    }

    // if matched text is already selected, just beep
    if (selStart == startPos && selEnd == endPos) {
        QApplication::beep();
        return false;
    }

    // select the text found string
    document->buffer_->BufSelect(startPos, endPos);
    document->MakeSelectionVisible(area);

    area->TextSetCursorPos(endPos);

    return true;
}

void SearchForSelectedEx(MainWindow *window, DocumentWidget *document, TextArea *area, SearchDirection direction, SearchType searchType, int searchWrap) {

    // skip if we can't get the selection data or it's too long
    // should be of type text???
    const QMimeData *mimeData = QApplication::clipboard()->mimeData(QClipboard::Selection);
    if(!mimeData->hasText()) {
        if (GetPrefSearchDlogs()) {
            QMessageBox::warning(document, QLatin1String("Wrong Selection"), QLatin1String("Selection not appropriate for searching"));
        } else {
            QApplication::beep();
        }
        return;
    }

    // make the selection the current search string
    QString searchString = mimeData->text();

    if (searchString.size() > SEARCHMAX) {
        if (GetPrefSearchDlogs()) {
            QMessageBox::warning(document, QLatin1String("Selection too long"), QLatin1String("Selection too long"));
        } else {
            QApplication::beep();
        }
        return;
    }

    if (searchString.size() == 0) {
        QApplication::beep();
        return;
    }

    /* Use the passed method for searching, unless it is regex, since this
       kind of search is by definition a literal search */
    if (searchType == SEARCH_REGEX) {
        searchType = SEARCH_CASE_SENSE;
    } else if (searchType == SEARCH_REGEX_NOCASE) {
        searchType = SEARCH_LITERAL;
    }

    // search for it in the window
    SearchAndSelectEx(
                window,
                document,
                area,
                direction,
                searchString,
                searchType,
                searchWrap);
}

/*
** Reset window->iSearchLastBeginPos_ to the resulting initial
** search begin position for incremental searches.
*/
static void iSearchRecordLastBeginPosEx(MainWindow *window, SearchDirection direction, int initPos) {
    window->iSearchLastBeginPos_ = initPos;
    if (direction == SEARCH_BACKWARD)
        window->iSearchLastBeginPos_--;
}

/*
** Search for "searchString" in "window", and select the matching text in
** the window when found (or beep or put up a dialog if not found).  If
** "continued" is TRUE and a prior incremental search starting position is
** recorded, search from that original position, otherwise, search from the
** current cursor position.
*/
bool SearchAndSelectIncrementalEx(MainWindow *window, DocumentWidget *document, TextArea *area, SearchDirection direction, const QString &searchString, SearchType searchType, bool searchWrap, bool continued) {
    int beginPos;
    int startPos;
    int endPos;

    /* If there's a search in progress, start the search from the original
       starting position, otherwise search from the cursor position. */
    if (!continued || window->iSearchStartPos_ == -1) {
        window->iSearchStartPos_ = area->TextGetCursorPos();
        iSearchRecordLastBeginPosEx(window, direction, window->iSearchStartPos_);
    }

    beginPos = window->iSearchStartPos_;

    /* If the search string is empty, beep eventually if text wrapped
       back to the initial position, re-init iSearchLastBeginPos,
       clear the selection, set the cursor back to what would be the
       beginning of the search, and return. */
    if (searchString.isEmpty()) {
        int beepBeginPos = (direction == SEARCH_BACKWARD) ? beginPos - 1 : beginPos;
        iSearchTryBeepOnWrapEx(window, direction, beepBeginPos, beepBeginPos);
        iSearchRecordLastBeginPosEx(window, direction, window->iSearchStartPos_);
        document->buffer_->BufUnselect();

        area->TextSetCursorPos(beginPos);
        return true;
    }

    /* Save the string in the search history, unless we're cycling thru
       the search history itself, which can be detected by matching the
       search string with the search string of the current history index. */
    if (!(window->iSearchHistIndex_ > 1 && (searchString == SearchHistory[historyIndex(window->iSearchHistIndex_)]))) {
        saveSearchHistory(searchString, QString(), searchType, true);
        // Reset the incremental search history pointer to the beginning
        window->iSearchHistIndex_ = 1;
    }

    // begin at insert position - 1 for backward searches
    if (direction == SEARCH_BACKWARD)
        beginPos--;

    // do the search.  SearchWindow does appropriate dialogs and beeps
    if (!SearchWindowEx(window, document, direction, searchString, searchType, searchWrap, beginPos, &startPos, &endPos, nullptr, nullptr))
        return false;

    window->iSearchLastBeginPos_ = startPos;

    /* if the search matched an empty string (possible with regular exps)
       beginning at the start of the search, go to the next occurrence,
       otherwise repeated finds will get "stuck" at zero-length matches */
    if (direction == SEARCH_FORWARD && beginPos == startPos && beginPos == endPos)
        if (!SearchWindowEx(window, document, direction, searchString, searchType, searchWrap, beginPos + 1, &startPos, &endPos, nullptr, nullptr))
            return false;

    window->iSearchLastBeginPos_ = startPos;

    // select the text found string
    document->buffer_->BufSelect(startPos, endPos);
    document->MakeSelectionVisible(area);

    area->TextSetCursorPos(endPos);

    return true;
}

/*
** Check the character before the insertion cursor of textW and flash
** matching parenthesis, brackets, or braces, by temporarily highlighting
** the matching character (a timer procedure is scheduled for removing the
** highlights)
*/
void FlashMatchingEx(DocumentWidget *document, TextArea *area) {

    // if a marker is already drawn, erase it and cancel the timeout
    if (document->flashTimer_->isActive()) {
        eraseFlashEx(document);
        document->flashTimer_->stop();
    }

    // no flashing required
    if (document->showMatchingStyle_ == NO_FLASH) {
        return;
    }

    // don't flash matching characters if there's a selection
    if (document->buffer_->primary_.selected) {
        return;
    }

    // get the character to match and the position to start from
    int pos = area->TextGetCursorPos() - 1;
    if (pos < 0) {
        return;
    }

    char c = document->buffer_->BufGetCharacter(pos);

    void *style = GetHighlightInfoEx(document, pos);

    int matchIndex;
    int matchPos;

    // is the character one we want to flash?
    for (matchIndex = 0; matchIndex < N_FLASH_CHARS; matchIndex++) {
        if (MatchingChars[matchIndex].c == c)
            break;
    }

    if (matchIndex == N_FLASH_CHARS) {
        return;
    }

    /* constrain the search to visible text only when in single-pane mode
       AND using delimiter flashing (otherwise search the whole buffer) */
    int constrain = ((document->textPanes().size() == 0) && (document->showMatchingStyle_ == FLASH_DELIMIT));

    int startPos;
    int endPos;
    int searchPos;

    if (MatchingChars[matchIndex].direction == SEARCH_BACKWARD) {
        startPos = constrain ? area->TextFirstVisiblePos() : 0;
        endPos = pos;
        searchPos = endPos;
    } else {
        startPos = pos;
        endPos = constrain ? area->TextLastVisiblePos() : document->buffer_->BufGetLength();
        searchPos = startPos;
    }

    // do the search
    if (!findMatchingCharEx(document, c, style, searchPos, startPos, endPos, &matchPos)) {
        return;
    }


    if (document->showMatchingStyle_ == FLASH_DELIMIT) {
        // Highlight either the matching character ...
        document->buffer_->BufHighlight(matchPos, matchPos + 1);
    } else {
        // ... or the whole range.
        if (MatchingChars[matchIndex].direction == SEARCH_BACKWARD) {
            document->buffer_->BufHighlight(matchPos, pos + 1);
        } else {
            document->buffer_->BufHighlight(matchPos + 1, pos);
        }
    }

    document->flashTimer_->start();
    document->flashPos_ = matchPos;
}

static bool findMatchingCharEx(DocumentWidget *window, char toMatch, void *styleToMatch, int charPos, int startLimit, int endLimit, int *matchPos) {
    int nestDepth;
    int matchIndex;
    int direction;
    int beginPos;
    int pos;
    char matchChar;
    char c;
    void *style = nullptr;
    TextBuffer *buf = window->buffer_;
    int matchSyntaxBased = window->matchSyntaxBased_;

    // If we don't match syntax based, fake a matching style.
    if (!matchSyntaxBased)
        style = styleToMatch;

    // Look up the matching character and match direction
    for (matchIndex = 0; matchIndex < N_MATCH_CHARS; matchIndex++) {
        if (MatchingChars[matchIndex].c == toMatch)
            break;
    }
    if (matchIndex == N_MATCH_CHARS) {
        return false;
    }

    matchChar = MatchingChars[matchIndex].match;
    direction = MatchingChars[matchIndex].direction;

    // find it in the buffer
    beginPos = (direction == SEARCH_FORWARD) ? charPos + 1 : charPos - 1;
    nestDepth = 1;
    if (direction == SEARCH_FORWARD) {
        for (pos = beginPos; pos < endLimit; pos++) {
            c = buf->BufGetCharacter(pos);
            if (c == matchChar) {
                if (matchSyntaxBased)
                    style = GetHighlightInfoEx(window, pos);
                if (style == styleToMatch) {
                    nestDepth--;
                    if (nestDepth == 0) {
                        *matchPos = pos;
                        return true;
                    }
                }
            } else if (c == toMatch) {
                if (matchSyntaxBased)
                    style = GetHighlightInfoEx(window, pos);
                if (style == styleToMatch)
                    nestDepth++;
            }
        }
    } else { // SEARCH_BACKWARD
        for (pos = beginPos; pos >= startLimit; pos--) {
            c = buf->BufGetCharacter(pos);
            if (c == matchChar) {
                if (matchSyntaxBased)
                    style = GetHighlightInfoEx(window, pos);
                if (style == styleToMatch) {
                    nestDepth--;
                    if (nestDepth == 0) {
                        *matchPos = pos;
                        return true;
                    }
                }
            } else if (c == toMatch) {
                if (matchSyntaxBased)
                    style = GetHighlightInfoEx(window, pos);
                if (style == styleToMatch)
                    nestDepth++;
            }
        }
    }
    return false;
}

/*
** Erase the marker drawn on a matching parenthesis bracket or brace
** character.
*/
void eraseFlashEx(DocumentWidget *document) {
    document->buffer_->BufUnhighlight();
}

/*
** Search and replace using previously entered search strings (from dialog
** or selection).
*/
bool ReplaceSameEx(MainWindow *window, DocumentWidget *document, TextArea *area, SearchDirection direction, int searchWrap) {
    if (NHist < 1) {
        QApplication::beep();
        return false;
    }

    return SearchAndReplaceEx(window, document, area, direction, SearchHistory[historyIndex(1)], ReplaceHistory[historyIndex(1)], SearchTypeHistory[historyIndex(1)], searchWrap);
}

/*
** Search and replace using previously entered search strings (from dialog
** or selection).
*/
bool ReplaceFindSameEx(MainWindow *window, DocumentWidget *document, TextArea *area, SearchDirection direction, int searchWrap) {
    if (NHist < 1) {
        QApplication::beep();
        return false;
    }

    return ReplaceAndSearchEx(window, document, area, direction, SearchHistory[historyIndex(1)], ReplaceHistory[historyIndex(1)], SearchTypeHistory[historyIndex(1)], searchWrap);
}

/*
** Replace selection with "replaceString" and search for string "searchString" in window "window",
** using algorithm "searchType" and direction "direction"
*/
bool ReplaceAndSearchEx(MainWindow *window, DocumentWidget *document, TextArea *area, SearchDirection direction, const QString &searchString, const QString &replaceString, SearchType searchType, int searchWrap) {
    int startPos = 0;
    int endPos = 0;
    int replaceLen = 0;
    int searchExtentBW;
    int searchExtentFW;

    // Save a copy of search and replace strings in the search history
    saveSearchHistory(searchString, replaceString, searchType, false);

    bool replaced = false;

    // Replace the selected text only if it matches the search string
    if (searchMatchesSelectionEx(document, searchString, searchType, &startPos, &endPos, &searchExtentBW, &searchExtentFW)) {
        // replace the text
        if (isRegexType(searchType)) {
            char replaceResult[SEARCHMAX + 1];
            const std::string foundString = document->buffer_->BufGetRangeEx(searchExtentBW, searchExtentFW + 1);

            replaceUsingREEx(
                searchString.toLatin1().data(),
                replaceString.toLatin1().data(),
                foundString,
                startPos - searchExtentBW,
                replaceResult,
                SEARCHMAX,
                startPos == 0 ? '\0' : document->buffer_->BufGetCharacter(startPos - 1),
                GetWindowDelimitersEx(document).toLatin1().data(),
                defaultRegexFlags(searchType));

            document->buffer_->BufReplaceEx(startPos, endPos, replaceResult);
            replaceLen = strlen(replaceResult);
        } else {
            document->buffer_->BufReplaceEx(startPos, endPos, replaceString.toLatin1().data());
            replaceLen = replaceString.size();
        }

        // Position the cursor so the next search will work correctly based
        // on the direction of the search
        area->TextSetCursorPos(startPos + ((direction == SEARCH_FORWARD) ? replaceLen : 0));
        replaced = true;
    }

    // do the search; beeps/dialogs are taken care of
    SearchAndSelectEx(window, document, area, direction, searchString, searchType, searchWrap);
    return replaced;
}

/*
** Search for string "searchString" in window "window", using algorithm
** "searchType" and direction "direction", and replace it with "replaceString"
** Also adds the search and replace strings to the global search history.
*/
bool SearchAndReplaceEx(MainWindow *window, DocumentWidget *document, TextArea *area, SearchDirection direction, const QString &searchString, const QString &replaceString, SearchType searchType, bool searchWrap) {
    int startPos;
    int endPos;
    int replaceLen;
    int searchExtentBW;
    int searchExtentFW;
    int found;
    int beginPos;
    int cursorPos;

    // Save a copy of search and replace strings in the search history
    saveSearchHistory(searchString, replaceString, searchType, false);

    // If the text selected in the window matches the search string,
    // the user is probably using search then replace method, so
    // replace the selected text regardless of where the cursor is.
    // Otherwise, search for the string.
    if (!searchMatchesSelectionEx(document, searchString, searchType, &startPos, &endPos, &searchExtentBW, &searchExtentFW)) {
        // get the position to start the search

        cursorPos = area->TextGetCursorPos();
        if (direction == SEARCH_BACKWARD) {
            // use the insert position - 1 for backward searches
            beginPos = cursorPos - 1;
        } else {
            // use the insert position for forward searches
            beginPos = cursorPos;
        }
        // do the search
        found = SearchWindowEx(window, document, direction, searchString, searchType, searchWrap, beginPos, &startPos, &endPos, &searchExtentBW, &searchExtentFW);
        if (!found)
            return false;
    }

    // replace the text
    if (isRegexType(searchType)) {
        char replaceResult[SEARCHMAX];
        const std::string foundString = document->buffer_->BufGetRangeEx(searchExtentBW, searchExtentFW + 1);
        replaceUsingREEx(
            searchString.toLatin1().data(),
            replaceString.toLatin1().data(),
            foundString,
            startPos - searchExtentBW,
            replaceResult,
            SEARCHMAX,
            startPos == 0 ? '\0' : document->buffer_->BufGetCharacter(startPos - 1),
            GetWindowDelimitersEx(document).toLatin1().data(),
            defaultRegexFlags(searchType));

        document->buffer_->BufReplaceEx(startPos, endPos, replaceResult);
        replaceLen = strlen(replaceResult);
    } else {
        document->buffer_->BufReplaceEx(startPos, endPos, replaceString.toLatin1().data());
        replaceLen = replaceString.size();
    }

    /* after successfully completing a replace, selected text attracts
       attention away from the area of the replacement, particularly
       when the selection represents a previous search. so deselect */
    document->buffer_->BufUnselect();

    /* temporarily shut off autoShowInsertPos before setting the cursor
       position so MakeSelectionVisible gets a chance to place the replaced
       string at a pleasing position on the screen (otherwise, the cursor would
       be automatically scrolled on screen and MakeSelectionVisible would do
       nothing) */
    area->setAutoShowInsertPos(false);

    area->TextSetCursorPos(startPos + ((direction == SEARCH_FORWARD) ? replaceLen : 0));
    document->MakeSelectionVisible(area);
    area->setAutoShowInsertPos(true);

    return true;
}

/*
**  Uses the resource nedit.truncSubstitution to determine how to deal with
**  regex failures. This function only knows about the resource (via the usual
**  setting getter) and asks or warns the user depending on that.
**
**  One could argue that the dialoging should be determined by the setting
**  'searchDlogs'. However, the incomplete substitution is not just a question
**  of verbosity, but of data loss. The search is successful, only the
**  replacement fails due to an internal limitation of NEdit.
**
**  The parameters 'parent' and 'display' are only used to put dialogs and
**  beeps at the right place.
**
**  The result is either predetermined by the resource or the user's choice.
*/
static bool prefOrUserCancelsSubstEx(MainWindow *window, DocumentWidget *document) {

    Q_UNUSED(window);

    bool cancel = true;

    switch (GetPrefTruncSubstitution()) {
    case TRUNCSUBST_SILENT:
        //  silently fail the operation
        cancel = true;
        break;

    case TRUNCSUBST_FAIL:
        //  fail the operation and pop up a dialog informing the user
        QApplication::beep();

        QMessageBox::information(document, QLatin1String("Substitution Failed"), QLatin1String("The result length of the substitution exceeded an internal limit.\n"
                                                                                                           "The substitution is canceled."));

        cancel = true;
        break;

    case TRUNCSUBST_WARN:
        //  pop up dialog and ask for confirmation
        QApplication::beep();

        {
            QMessageBox messageBox(document);
            messageBox.setWindowTitle(QLatin1String("Substitution Failed"));
            messageBox.setIcon(QMessageBox::Warning);
            messageBox.setText(QLatin1String("The result length of the substitution exceeded an internal limit.\nExecuting the substitution will result in loss of data."));
            QPushButton *buttonLose   = messageBox.addButton(QLatin1String("Lose Data"), QMessageBox::AcceptRole);
            QPushButton *buttonCancel = messageBox.addButton(QMessageBox::Cancel);
            Q_UNUSED(buttonLose);

            messageBox.exec();
            cancel = (messageBox.clickedButton() == buttonCancel);
        }
        break;

    case TRUNCSUBST_IGNORE:
        //  silently conclude the operation; THIS WILL DESTROY DATA.
        cancel = false;
        break;
    }

    return cancel;
}

/*
** Replace all occurences of "searchString" in "window" with "replaceString"
** within the current primary selection in "window". Also adds the search and
** replace strings to the global search history.
*/
void ReplaceInSelectionEx(MainWindow *window, DocumentWidget *document, TextArea *area, const QString &searchString, const QString &replaceString, SearchType searchType) {
    int selStart;
    int selEnd;
    int beginPos;
    int startPos;
    int endPos;
    int realOffset;
    int replaceLen;
    bool found;
    bool isRect;
    int rectStart;
    int rectEnd;
    int lineStart;
    int cursorPos;
    int extentBW;
    int extentFW;
    std::string fileString;
    bool substSuccess = false;
    bool anyFound     = false;
    bool cancelSubst  = true;

    // save a copy of search and replace strings in the search history
    saveSearchHistory(searchString, replaceString, searchType, false);

    // find out where the selection is
    if (!document->buffer_->BufGetSelectionPos(&selStart, &selEnd, &isRect, &rectStart, &rectEnd)) {
        return;
    }

    // get the selected text
    if (isRect) {
        selStart = document->buffer_->BufStartOfLine(selStart);
        selEnd = document->buffer_->BufEndOfLine( selEnd);
        fileString = document->buffer_->BufGetRangeEx(selStart, selEnd);
    } else {
        fileString = document->buffer_->BufGetSelectionTextEx();
    }

    /* create a temporary buffer in which to do the replacements to hide the
       intermediate steps from the display routines, and so everything can
       be undone in a single operation */
    auto tempBuf = std::make_unique<TextBuffer>();
    tempBuf->BufSetAllEx(fileString);

    // search the string and do the replacements in the temporary buffer

    replaceLen = replaceString.size();
    found      = true;
    beginPos   = 0;
    cursorPos  = 0;
    realOffset = 0;

    while (found) {
        found = SearchString(fileString, searchString, SEARCH_FORWARD, searchType, false, beginPos, &startPos, &endPos, &extentBW, &extentFW, GetWindowDelimitersEx(document).toLatin1().data());
        if (!found)
            break;

        anyFound = true;
        /* if the selection is rectangular, verify that the found
           string is in the rectangle */
        if (isRect) {
            lineStart = document->buffer_->BufStartOfLine(selStart + startPos);
            if (document->buffer_->BufCountDispChars(lineStart, selStart + startPos) < rectStart || document->buffer_->BufCountDispChars(lineStart, selStart + endPos) > rectEnd) {
                if (fileString[endPos] == '\0')
                    break;
                /* If the match starts before the left boundary of the
                   selection, and extends past it, we should not continue
                   search after the end of the (false) match, because we
                   could miss a valid match starting between the left boundary
                   and the end of the false match. */
                if (document->buffer_->BufCountDispChars(lineStart, selStart + startPos) < rectStart && document->buffer_->BufCountDispChars(lineStart, selStart + endPos) > rectStart)
                    beginPos += 1;
                else
                    beginPos = (startPos == endPos) ? endPos + 1 : endPos;
                continue;
            }
        }

        /* Make sure the match did not start past the end (regular expressions
           can consider the artificial end of the range as the end of a line,
           and match a fictional whole line beginning there) */
        if (startPos == (selEnd - selStart)) {
            found = false;
            break;
        }

        // replace the string and compensate for length change
        if (isRegexType(searchType)) {
            char replaceResult[SEARCHMAX];
            const std::string foundString = tempBuf->BufGetRangeEx(extentBW + realOffset, extentFW + realOffset + 1);
            substSuccess = replaceUsingREEx(
                            searchString.toLatin1().data(),
                            replaceString.toLatin1().data(),
                            foundString,
                            startPos - extentBW,
                            replaceResult,
                            SEARCHMAX,
                            (startPos + realOffset) == 0 ? '\0' : tempBuf->BufGetCharacter(startPos + realOffset - 1),
                            GetWindowDelimitersEx(document).toLatin1().data(),
                            defaultRegexFlags(searchType));

            if (!substSuccess) {
                /*  The substitution failed. Primary reason for this would be
                    a result string exceeding SEARCHMAX. */

                cancelSubst = prefOrUserCancelsSubstEx(window, document);

                if (cancelSubst) {
                    //  No point in trying other substitutions.
                    break;
                }
            }

            tempBuf->BufReplaceEx(startPos + realOffset, endPos + realOffset, replaceResult);
            replaceLen = strlen(replaceResult);
        } else {
            // at this point plain substitutions (should) always work
            tempBuf->BufReplaceEx(startPos + realOffset, endPos + realOffset, replaceString.toLatin1().data());
            substSuccess = true;
        }

        realOffset += replaceLen - (endPos - startPos);
        // start again after match unless match was empty, then endPos+1
        beginPos = (startPos == endPos) ? endPos + 1 : endPos;
        cursorPos = endPos;
        if (fileString[endPos] == '\0')
            break;
    }

    if (anyFound) {
        if (substSuccess || !cancelSubst) {
            /*  Either the substitution was successful (the common case) or the
                user does not care and wants to have a faulty replacement.  */

            // replace the selected range in the real buffer
            document->buffer_->BufReplaceEx(selStart, selEnd, tempBuf->BufAsStringEx());

            // set the insert point at the end of the last replacement
            area->TextSetCursorPos(selStart + cursorPos + realOffset);

            /* leave non-rectangular selections selected (rect. ones after replacement
               are less useful since left/right positions are randomly adjusted) */
            if (!isRect) {
                document->buffer_->BufSelect(selStart, selEnd + realOffset);
            }
        }
    } else {
        //  Nothing found, tell the user about it
        if (GetPrefSearchDlogs()) {

            // Avoid bug in Motif 1.1 by putting away search dialog before Dialogs
            if (auto dialog = qobject_cast<DialogFind *>(window->dialogFind_)) {
                if(!dialog->keepDialog()) {
                    dialog->hide();
                }
            }

            auto dialog = window->getDialogReplace();
            if (dialog && !dialog->keepDialog()) {
                dialog->hide();
            }

            QMessageBox::information(document, QLatin1String("String not found"), QLatin1String("String was not found"));
        } else {
            QApplication::beep();
        }
    }
}


/*
** Replace all occurences of "searchString" in "window" with "replaceString".
** Also adds the search and replace strings to the global search history.
*/
bool ReplaceAllEx(MainWindow *window, DocumentWidget *document, TextArea *area, const QString &searchString, const QString &replaceString, SearchType searchType) {
    char *newFileString;
    int copyStart, copyEnd, replacementLen;

    // reject empty string
    if (searchString.isEmpty()) {
        return false;
    }

    // save a copy of search and replace strings in the search history
    saveSearchHistory(searchString, replaceString, searchType, false);

    // view the entire text buffer from the text area widget as a string
    view::string_view fileString = document->buffer_->BufAsStringEx();

    newFileString = ReplaceAllInString(fileString, searchString, replaceString.toLatin1().data(), searchType, &copyStart, &copyEnd, &replacementLen, GetWindowDelimitersEx(document).toLatin1().data());

    if(!newFileString) {
        if (document->multiFileBusy_) {
            document->replaceFailed_ = true; /* only needed during multi-file
                                             replacements */
        } else if (GetPrefSearchDlogs()) {

            if (auto dialog = qobject_cast<DialogFind *>(window->dialogFind_)) {
                if(!dialog->keepDialog()) {
                    dialog->hide();
                }
            }

            auto dialog = window->getDialogReplace();
            if (dialog && !dialog->keepDialog()) {
                dialog->hide();
            }

            QMessageBox::information(document, QLatin1String("String not found"), QLatin1String("String was not found"));
        } else
            QApplication::beep();
        return false;
    }

    // replace the contents of the text widget with the substituted text
    document->buffer_->BufReplaceEx(copyStart, copyEnd, newFileString);

    // Move the cursor to the end of the last replacement
    area->TextSetCursorPos(copyStart + replacementLen);

    delete [] newFileString;
    return true;
}

/*
** Replace all occurences of "searchString" in "inString" with "replaceString"
** and return an allocated string covering the range between the start of the
** first replacement (returned in "copyStart", and the end of the last
** replacement (returned in "copyEnd")
*/
char *ReplaceAllInString(view::string_view inString, const QString &searchString, const char *replaceString, SearchType searchType, int *copyStart, int *copyEnd, int *replacementLength, const char *delimiters) {
    int startPos;
    int endPos;
    int lastEndPos;
    int copyLen;
    char *outString;
    char *fillPtr;
    int searchExtentBW;
    int searchExtentFW;

	// reject empty string 
    if (searchString.isNull()) {
		return nullptr;
    }

	/* rehearse the search first to determine the size of the buffer needed
	   to hold the substituted text.  No substitution done here yet */
    int replaceLen = strlen(replaceString);
    bool found     = true;
    int nFound     = 0;
    int removeLen  = 0;
    int addLen     = 0;
    int beginPos   = 0;

	*copyStart = -1;

	while (found) {
        found = SearchString(inString, searchString, SEARCH_FORWARD, searchType, false, beginPos, &startPos, &endPos, &searchExtentBW, &searchExtentFW, delimiters);
		if (found) {
			if (*copyStart < 0)
				*copyStart = startPos;
			*copyEnd = endPos;
			// start next after match unless match was empty, then endPos+1 
			beginPos = (startPos == endPos) ? endPos + 1 : endPos;
			nFound++;
			removeLen += endPos - startPos;
			if (isRegexType(searchType)) {
				char replaceResult[SEARCHMAX];
				replaceUsingREEx(
                    searchString.toLatin1().data(),
					replaceString,
					&inString[searchExtentBW],
					startPos - searchExtentBW,
					replaceResult,
					SEARCHMAX,
					startPos == 0 ? '\0' : inString[startPos - 1],
					delimiters,
					defaultRegexFlags(searchType));

				addLen += strlen(replaceResult);
			} else
				addLen += replaceLen;
			if (inString[endPos] == '\0')
				break;
		}
	}
	if (nFound == 0)
		return nullptr;

	/* Allocate a new buffer to hold all of the new text between the first
	   and last substitutions */
	copyLen = *copyEnd - *copyStart;
    outString = new char[copyLen - removeLen + addLen + 1];

	/* Scan through the text buffer again, substituting the replace string
	   and copying the part between replaced text to the new buffer  */
    found = true;
	beginPos = 0;
	lastEndPos = 0;
	fillPtr = outString;
	while (found) {
        found = SearchString(inString, searchString, SEARCH_FORWARD, searchType, false, beginPos, &startPos, &endPos, &searchExtentBW, &searchExtentFW, delimiters);
		if (found) {
			if (beginPos != 0) {
				memcpy(fillPtr, &inString[lastEndPos], startPos - lastEndPos);
				fillPtr += startPos - lastEndPos;
			}
			if (isRegexType(searchType)) {
				char replaceResult[SEARCHMAX];
				replaceUsingREEx(
                    searchString.toLatin1().data(),
					replaceString,
					&inString[searchExtentBW],
					startPos - searchExtentBW,
					replaceResult,
					SEARCHMAX,
					startPos == 0 ? '\0' : inString[startPos - 1],
					delimiters,
					defaultRegexFlags(searchType));

				replaceLen = strlen(replaceResult);
				memcpy(fillPtr, replaceResult, replaceLen);
			} else {
				memcpy(fillPtr, replaceString, replaceLen);
			}
			fillPtr += replaceLen;
			lastEndPos = endPos;
			// start next after match unless match was empty, then endPos+1 
			beginPos = (startPos == endPos) ? endPos + 1 : endPos;
			if (inString[endPos] == '\0')
				break;
		}
	}
	*fillPtr = '\0';
	*replacementLength = fillPtr - outString;
	return outString;
}

/*
** If this is an incremental search and BeepOnSearchWrap is on:
** Emit a beep if the search wrapped over BOF/EOF compared to
** the last startPos of the current incremental search.
*/
static void iSearchTryBeepOnWrapEx(MainWindow *window, SearchDirection direction, int beginPos, int startPos) {
    if (GetPrefBeepOnSearchWrap()) {
        if (direction == SEARCH_FORWARD) {
            if ((startPos >= beginPos && window->iSearchLastBeginPos_ < beginPos) || (startPos < beginPos && window->iSearchLastBeginPos_ >= beginPos)) {
                QApplication::beep();
            }
        } else {
            if ((startPos <= beginPos && window->iSearchLastBeginPos_ > beginPos) || (startPos > beginPos && window->iSearchLastBeginPos_ <= beginPos)) {
                QApplication::beep();
            }
        }
    }
}

/*
** Search the text in "window", attempting to match "searchString"
*/
bool SearchWindowEx(MainWindow *window, DocumentWidget *document, SearchDirection direction, const QString &searchString, SearchType searchType, int searchWrap, int beginPos, int *startPos, int *endPos, int *extentBW, int *extentFW) {
    bool found;
    int fileEnd = document->buffer_->BufGetLength() - 1;
    bool outsideBounds;

    // reject empty string
    if (searchString.isEmpty()) {
        return false;
    }

    // get the entire text buffer from the text area widget
    view::string_view fileString = document->buffer_->BufAsStringEx();

    /* If we're already outside the boundaries, we must consider wrapping
       immediately (Note: fileEnd+1 is a valid starting position. Consider
       searching for $ at the end of a file ending with \n.) */
    if ((direction == SEARCH_FORWARD && beginPos > fileEnd + 1) || (direction == SEARCH_BACKWARD && beginPos < 0)) {
        outsideBounds = true;
    } else {
        outsideBounds = false;
    }

    /* search the string copied from the text area widget, and present
       dialogs, or just beep.  iSearchStartPos is not a perfect indicator that
       an incremental search is in progress.  A parameter would be better. */
    if (window->iSearchStartPos_ == -1) { // normal search
        found = !outsideBounds && SearchString(fileString, searchString, direction, searchType, false, beginPos, startPos, endPos, extentBW, extentFW, GetWindowDelimitersEx(document).toLatin1().data());

        // Avoid Motif 1.1 bug by putting away search dialog before Dialogs
        if (auto dialog = qobject_cast<DialogFind *>(window->dialogFind_)) {
            if(!dialog->keepDialog()) {
                dialog->hide();
            }
        }

        auto dialog = window->getDialogReplace();
        if (dialog && !dialog->keepDialog()) {
            dialog->hide();
        }

        if (!found) {
            if (searchWrap) {
                if (direction == SEARCH_FORWARD && beginPos != 0) {
                    if (GetPrefBeepOnSearchWrap()) {
                        QApplication::beep();
                    } else if (GetPrefSearchDlogs()) {

                        QMessageBox messageBox(nullptr /*window->shell_*/);
                        messageBox.setWindowTitle(QLatin1String("Wrap Search"));
                        messageBox.setIcon(QMessageBox::Question);
                        messageBox.setText(QLatin1String("Continue search from\nbeginning of file?"));
                        QPushButton *buttonContinue = messageBox.addButton(QLatin1String("Continue"), QMessageBox::AcceptRole);
                        QPushButton *buttonCancel   = messageBox.addButton(QMessageBox::Cancel);
                        Q_UNUSED(buttonContinue);

                        messageBox.exec();
                        if(messageBox.clickedButton() == buttonCancel) {
                            return false;
                        }
                    }
                    found = SearchString(fileString, searchString, direction, searchType, false, 0, startPos, endPos, extentBW, extentFW, GetWindowDelimitersEx(document).toLatin1().data());
                } else if (direction == SEARCH_BACKWARD && beginPos != fileEnd) {
                    if (GetPrefBeepOnSearchWrap()) {
                        QApplication::beep();
                    } else if (GetPrefSearchDlogs()) {

                        QMessageBox messageBox(nullptr /*window->shell_*/);
                        messageBox.setWindowTitle(QLatin1String("Wrap Search"));
                        messageBox.setIcon(QMessageBox::Question);
                        messageBox.setText(QLatin1String("Continue search\nfrom end of file?"));
                        QPushButton *buttonContinue = messageBox.addButton(QLatin1String("Continue"), QMessageBox::AcceptRole);
                        QPushButton *buttonCancel   = messageBox.addButton(QMessageBox::Cancel);
                        Q_UNUSED(buttonContinue);

                        messageBox.exec();
                        if(messageBox.clickedButton() == buttonCancel) {
                            return false;
                        }
                    }
                    found = SearchString(fileString, searchString, direction, searchType, false, fileEnd + 1, startPos, endPos, extentBW, extentFW, GetWindowDelimitersEx(document).toLatin1().data());
                }
            }
            if (!found) {
                if (GetPrefSearchDlogs()) {
                    QMessageBox::information(nullptr /*parent*/, QLatin1String("String not found"), QLatin1String("String was not found"));
                } else {
                    QApplication::beep();
                }
            }
        }
    } else { // incremental search
        if (outsideBounds && searchWrap) {
            if (direction == SEARCH_FORWARD)
                beginPos = 0;
            else
                beginPos = fileEnd + 1;
            outsideBounds = false;
        }
        found = !outsideBounds && SearchString(fileString, searchString, direction, searchType, searchWrap, beginPos, startPos, endPos, extentBW, extentFW, GetWindowDelimitersEx(document).toLatin1().data());
        if (found) {
            iSearchTryBeepOnWrapEx(window, direction, beginPos, *startPos);
        } else
            QApplication::beep();
    }

    return found;
}

/*
** Search the null terminated string "string" for "searchString", beginning at
** "beginPos".  Returns the boundaries of the match in "startPos" and "endPos".
** searchExtentBW and searchExtentFW return the backwardmost and forwardmost
** positions used to make the match, which are usually startPos and endPos,
** but may extend further if positive lookahead or lookbehind was used in
** a regular expression match.  "delimiters" may be used to provide an
** alternative set of word delimiters for regular expression "<" and ">"
** characters, or simply passed as null for the default delimiter set.
*/
bool SearchString(view::string_view string, const QString &searchString, SearchDirection direction, SearchType searchType, bool wrap, int beginPos, int *startPos, int *endPos, int *searchExtentBW, int *searchExtentFW, const char *delimiters) {
	switch (searchType) {
	case SEARCH_CASE_SENSE_WORD:
        return searchLiteralWord(string, searchString.toLatin1().data(), true, direction, wrap, beginPos, startPos, endPos, delimiters);
	case SEARCH_LITERAL_WORD:
        return searchLiteralWord(string, searchString.toLatin1().data(), false, direction, wrap, beginPos, startPos, endPos, delimiters);
	case SEARCH_CASE_SENSE:
        return searchLiteral(string, searchString.toLatin1().data(), true, direction, wrap, beginPos, startPos, endPos, searchExtentBW, searchExtentFW);
	case SEARCH_LITERAL:
        return searchLiteral(string, searchString.toLatin1().data(), false, direction, wrap, beginPos, startPos, endPos, searchExtentBW, searchExtentFW);
	case SEARCH_REGEX:
        return searchRegex(string, searchString.toLatin1().data(), direction, wrap, beginPos, startPos, endPos, searchExtentBW, searchExtentFW, delimiters, REDFLT_STANDARD);
	case SEARCH_REGEX_NOCASE:
        return searchRegex(string, searchString.toLatin1().data(), direction, wrap, beginPos, startPos, endPos, searchExtentBW, searchExtentFW, delimiters, REDFLT_CASE_INSENSITIVE);
	default:
		Q_ASSERT(0);
	}
    return false; // never reached, just makes compilers happy
}

/*
** Parses a search type description string. If the string contains a valid
** search type description, returns TRUE and writes the corresponding
** SearchType in searchType. Returns FALSE and leaves searchType untouched
** otherwise. (Originally written by Markus Schwarzenberg; slightly adapted).
*/
int StringToSearchType(const std::string &string, SearchType *searchType) {

    for (int i = 0; searchTypeStrings[i]; i++) {
        if (string == searchTypeStrings[i]) {
            *searchType = static_cast<SearchType>(i);
            return true;
		}
	}

    return false;
}

/*
**  Searches for whole words (Markus Schwarzenberg).
**
**  If the first/last character of `searchString' is a "normal
**  word character" (not contained in `delimiters', not a whitespace)
**  then limit search to strings, who's next left/next right character
**  is contained in `delimiters' or is a whitespace or text begin or end.
**
**  If the first/last character of `searchString' itself is contained
**  in delimiters or is a white space, then the neighbour character of the
**  first/last character will not be checked, just a simple match
**  will suffice in that case.
**
*/
static bool searchLiteralWord(view::string_view string, view::string_view searchString, bool caseSense, SearchDirection direction, bool wrap, int beginPos, int *startPos, int *endPos, const char *delimiters) {


	// TODO(eteran): rework this code in terms of iterators, it will be more clean
	//               also, I'd love to have a nice clean way of replacing this macro
	//               with a lambda or similar

	std::string lcString;
	std::string ucString;
    bool cignore_L = false;
    bool cignore_R = false;

	auto DOSEARCHWORD2 = [&](const char *filePtr) {
		if (*filePtr == ucString[0] || *filePtr == lcString[0]) {
			// matched first character 
			auto ucPtr = ucString.begin();
			auto lcPtr = lcString.begin();
			const char *tempPtr = filePtr;
			while (*tempPtr == *ucPtr || *tempPtr == *lcPtr) {
				tempPtr++;
				ucPtr++;
				lcPtr++;
				if (ucPtr == ucString.end()                                                            // matched whole string 
			    	&& (cignore_R || isspace((uint8_t)*tempPtr) || strchr(delimiters, *tempPtr)) // next char right delimits word ? 
			    	&& (cignore_L || filePtr == &string[0] ||                                          // border case 
			        	isspace((uint8_t)filePtr[-1]) || strchr(delimiters, filePtr[-1])))       /* next char left delimits word ? */ {
					*startPos = filePtr - &string[0];
					*endPos = tempPtr - &string[0];
					return true;
				}
			}
		}
		
        return false;
	};


	// If there is no language mode, we use the default list of delimiters 
	QByteArray delimiterString = GetPrefDelimiters().toLatin1();
	if(!delimiters) {
		delimiters = delimiterString.data();
	}

	if (isspace((uint8_t)searchString[0]) || strchr(delimiters, searchString[0])) {
		cignore_L = true;
	}

	if (isspace((uint8_t)searchString[searchString.size() - 1]) || strchr(delimiters, searchString[searchString.size() - 1])) {
		cignore_R = true;
	}

	if (caseSense) {
		ucString = searchString.to_string();
		lcString = searchString.to_string();
	} else {
		ucString = upCaseStringEx(searchString);
		lcString = downCaseStringEx(searchString);
	}

	if (direction == SEARCH_FORWARD) {
		// search from beginPos to end of string 
		for (auto filePtr = string.begin() + beginPos; filePtr != string.end(); filePtr++) {
			if(DOSEARCHWORD2(filePtr)) {
				return true;
			}
		}
		if (!wrap)
            return false;

		// search from start of file to beginPos 
		for (auto filePtr = string.begin(); filePtr <= string.begin() + beginPos; filePtr++) {
			if(DOSEARCHWORD2(filePtr)) {
				return true;
			}
		}
        return false;
	} else {
		// SEARCH_BACKWARD 
		// search from beginPos to start of file. A negative begin pos 
		// says begin searching from the far end of the file 
		if (beginPos >= 0) {
			for (auto filePtr = string.begin() + beginPos; filePtr >= string.begin(); filePtr--) {
				if(DOSEARCHWORD2(filePtr)) {
					return true;
				}
			}
		}
		if (!wrap)
            return false;
		// search from end of file to beginPos 
		/*... this strlen call is extreme inefficiency, but it's not obvious */
		// how to get the text string length from the text widget (under 1.1)
		for (auto filePtr = string.begin() + string.size(); filePtr >= string.begin() + beginPos; filePtr--) {
			if(DOSEARCHWORD2(filePtr)) {
				return true;
			}
		}
        return false;
	}
}

static bool searchLiteral(view::string_view string, view::string_view searchString, bool caseSense, SearchDirection direction, bool wrap, int beginPos, int *startPos, int *endPos, int *searchExtentBW, int *searchExtentFW) {


	std::string lcString;
	std::string ucString;

	auto DOSEARCH2 = [&](const char *filePtr) {
		if (*filePtr == ucString[0] || *filePtr == lcString[0]) {
			// matched first character 
			auto ucPtr   = ucString.begin();
			auto lcPtr   = lcString.begin();
			const char *tempPtr = filePtr;

			while (*tempPtr == *ucPtr || *tempPtr == *lcPtr) {
				tempPtr++;
				ucPtr++;
				lcPtr++;
				if (ucPtr == ucString.end()) {
					// matched whole string 
					*startPos = filePtr - &string[0];
					*endPos   = tempPtr - &string[0];
					if(searchExtentBW) {
						*searchExtentBW = *startPos;
					}

					if(searchExtentFW) {
						*searchExtentFW = *endPos;
					}
					return true;
				}
			}
		}

        return false;
	};

	if (caseSense) {
		lcString = searchString.to_string();
		ucString = searchString.to_string();
	} else {
		ucString = upCaseStringEx(searchString);
		lcString = downCaseStringEx(searchString);
	}

	if (direction == SEARCH_FORWARD) {

		auto first = string.begin();
		auto mid   = first + beginPos;
		auto last  = string.end();

		// search from beginPos to end of string 
		for (auto filePtr = mid; filePtr != last; ++filePtr) {
			if(DOSEARCH2(filePtr)) {
				return true;
			}
		}

		if (!wrap) {
            return false;
		}

		// search from start of file to beginPos 
		// TODO(eteran): this used to include "mid", but that seems redundant given that we already looked there
		//               in the first loop
		for (auto filePtr = first; filePtr != mid; ++filePtr) {
			if(DOSEARCH2(filePtr)) {
				return true;
			}
		}

        return false;
	} else {
		// SEARCH_BACKWARD 
		// search from beginPos to start of file.  A negative begin pos	
		// says begin searching from the far end of the file 

		auto first = string.begin();
		auto mid   = first + beginPos;
		auto last  = string.end();

		if (beginPos >= 0) {
			for (auto filePtr = mid; filePtr >= first; --filePtr) {
				if(DOSEARCH2(filePtr)) {
					return true;
				}
			}
		}

		if (!wrap) {
            return false;
		}

		// search from end of file to beginPos 
		// how to get the text string length from the text widget (under 1.1)
		for (auto filePtr = last; filePtr >= mid; --filePtr) {
			if(DOSEARCH2(filePtr)) {
				return true;
			}
		}

        return false;
	}
}

static bool searchRegex(view::string_view string, view::string_view searchString, SearchDirection direction, bool wrap, int beginPos, int *startPos, int *endPos, int *searchExtentBW, int *searchExtentFW, const char *delimiters, int defaultFlags) {
	if (direction == SEARCH_FORWARD)
		return forwardRegexSearch(string, searchString, wrap, beginPos, startPos, endPos, searchExtentBW, searchExtentFW, delimiters, defaultFlags);
	else
		return backwardRegexSearch(string, searchString, wrap, beginPos, startPos, endPos, searchExtentBW, searchExtentFW, delimiters, defaultFlags);
}

static bool forwardRegexSearch(view::string_view string, view::string_view searchString, bool wrap, int beginPos, int *startPos, int *endPos, int *searchExtentBW, int *searchExtentFW, const char *delimiters, int defaultFlags) {

	try {
		regexp compiledRE(searchString, defaultFlags);

		// search from beginPos to end of string 
		if (compiledRE.execute(string, beginPos, delimiters, false)) {

			*startPos = compiledRE.startp[0] - &string[0];
			*endPos   = compiledRE.endp[0]   - &string[0];

			if(searchExtentFW) {
				*searchExtentFW = compiledRE.extentpFW - &string[0];
			}

			if(searchExtentBW) {
				*searchExtentBW = compiledRE.extentpBW - &string[0];
			}

			return true;
		}

		// if wrap turned off, we're done 
		if (!wrap) {
            return false;
		}

		// search from the beginning of the string to beginPos 
		if (compiledRE.execute(string, 0, beginPos, delimiters, false)) {

			*startPos = compiledRE.startp[0] - &string[0];
			*endPos = compiledRE.endp[0]     - &string[0];

			if(searchExtentFW) {
				*searchExtentFW = compiledRE.extentpFW - &string[0];
			}

			if(searchExtentBW) {
				*searchExtentBW = compiledRE.extentpBW - &string[0];
			}
			return true;
		}

        return false;
	} catch(const regex_error &e) {
		/* Note that this does not process errors from compiling the expression.
		 * It assumes that the expression was checked earlier.
		 */
        return false;
	}
}

static bool backwardRegexSearch(view::string_view string, view::string_view searchString, bool wrap, int beginPos, int *startPos, int *endPos, int *searchExtentBW, int *searchExtentFW, const char *delimiters, int defaultFlags) {

	try {
		regexp compiledRE(searchString, defaultFlags);

		// search from beginPos to start of file.  A negative begin pos	
		// says begin searching from the far end of the file.		
		if (beginPos >= 0) {

			// TODO(eteran): why do we use '\0' as the previous char, and not string[beginPos - 1] (assuming that beginPos > 0)?
			if (compiledRE.execute(string, 0, beginPos, '\0', '\0', delimiters, true)) {

				*startPos = compiledRE.startp[0] - &string[0];
				*endPos   = compiledRE.endp[0]   - &string[0];

				if(searchExtentFW) {
					*searchExtentFW = compiledRE.extentpFW - &string[0];
				}

				if(searchExtentBW) {
					*searchExtentBW = compiledRE.extentpBW - &string[0];
				}

				return true;
			}
		}

		// if wrap turned off, we're done 
		if (!wrap) {
            return false;
		}

		// search from the end of the string to beginPos 
		if (beginPos < 0) {
			beginPos = 0;
		}

		if (compiledRE.execute(string, beginPos, delimiters, true)) {

			*startPos = compiledRE.startp[0] - &string[0];
			*endPos   = compiledRE.endp[0]   - &string[0];

			if(searchExtentFW) {
				*searchExtentFW = compiledRE.extentpFW - &string[0];
			}

			if(searchExtentBW) {
				*searchExtentBW = compiledRE.extentpBW - &string[0];
			}

			return true;
		}

        return false;
	} catch(const regex_error &e) {
		// NOTE(eteran): ignoring error!
        return false;
	}
}

static std::string upCaseStringEx(view::string_view inString) {


	std::string str;
	str.reserve(inString.size());
	std::transform(inString.begin(), inString.end(), std::back_inserter(str), [](char ch) {
		return toupper((uint8_t)ch);
	});
	return str;
}

static std::string downCaseStringEx(view::string_view inString) {
	std::string str;
	str.reserve(inString.size());
	std::transform(inString.begin(), inString.end(), std::back_inserter(str), [](char ch) {
		return tolower((uint8_t)ch);
	});
	return str;
}

/*
** Return TRUE if "searchString" exactly matches the text in the window's
** current primary selection using search algorithm "searchType".  If true,
** also return the position of the selection in "left" and "right".
*/
static bool searchMatchesSelectionEx(DocumentWidget *window, const QString &searchString, SearchType searchType, int *left, int *right, int *searchExtentBW, int *searchExtentFW) {
    int selLen, selStart, selEnd, startPos, endPos, extentBW, extentFW, beginPos;
    int regexLookContext = isRegexType(searchType) ? 1000 : 0;
    std::string string;
    int rectStart, rectEnd, lineStart = 0;
    bool isRect;

    // find length of selection, give up on no selection or too long
    if (!window->buffer_->BufGetEmptySelectionPos(&selStart, &selEnd, &isRect, &rectStart, &rectEnd)) {
        return false;
    }

    if (selEnd - selStart > SEARCHMAX) {
        return false;
    }

    // if the selection is rectangular, don't match if it spans lines
    if (isRect) {
        lineStart = window->buffer_->BufStartOfLine(selStart);
        if (lineStart != window->buffer_->BufStartOfLine(selEnd)) {
            return false;
        }
    }

    /* get the selected text plus some additional context for regular
       expression lookahead */
    if (isRect) {
        int stringStart = lineStart + rectStart - regexLookContext;
        if (stringStart < 0) {
            stringStart = 0;
        }

        string = window->buffer_->BufGetRangeEx(stringStart, lineStart + rectEnd + regexLookContext);
        selLen = rectEnd - rectStart;
        beginPos = lineStart + rectStart - stringStart;
    } else {
        int stringStart = selStart - regexLookContext;
        if (stringStart < 0) {
            stringStart = 0;
        }

        string = window->buffer_->BufGetRangeEx(stringStart, selEnd + regexLookContext);
        selLen = selEnd - selStart;
        beginPos = selStart - stringStart;
    }
    if (string.empty()) {
        return false;
    }

    // search for the string in the selection (we are only interested
    // in an exact match, but the procedure SearchString does important
    // stuff like applying the correct matching algorithm)
    bool found = SearchString(string, searchString, SEARCH_FORWARD, searchType, false, beginPos, &startPos, &endPos, &extentBW, &extentFW, GetWindowDelimitersEx(window).toLatin1().data());

    // decide if it is an exact match
    if (!found) {
        return false;
    }

    if (startPos != beginPos || endPos - beginPos != selLen) {
        return false;
    }

    // return the start and end of the selection
    if (isRect) {
        window->buffer_->GetSimpleSelection(left, right);
    } else {
        *left  = selStart;
        *right = selEnd;
    }

    if(searchExtentBW) {
        *searchExtentBW = *left - (startPos - extentBW);
    }

    if(searchExtentFW) {
        *searchExtentFW = *right + extentFW - endPos;
    }

    return true;
}

/*
** Substitutes a replace string for a string that was matched using a
** regular expression.  This was added later and is rather ineficient
** because instead of using the compiled regular expression that was used
** to make the match in the first place, it re-compiles the expression
** and redoes the search on the already-matched string.  This allows the
** code to continue using strings to represent the search and replace
** items.
*/
static bool replaceUsingREEx(view::string_view searchStr, const char *replaceStr, view::string_view sourceStr, int beginPos, char *destStr, int maxDestLen, int prevChar, const char *delimiters, int defaultFlags) {
	try {
		regexp compiledRE(searchStr, defaultFlags);
		compiledRE.execute(sourceStr, beginPos, sourceStr.size(), prevChar, '\0', delimiters, false);
		return compiledRE.SubstituteRE(replaceStr, destStr, maxDestLen);
	} catch(const regex_error &e) {
		// NOTE(eteran): ignoring error!
        return false;
	}
}

/*
** Store the search and replace strings, and search type for later recall.
** If replaceString is nullptr, duplicate the last replaceString used.
** Contiguous incremental searches share the same history entry (each new
** search modifies the current search string, until a non-incremental search
** is made.  To mark the end of an incremental search, call saveSearchHistory
** again with an empty search string and isIncremental==False.
*/
void saveSearchHistory(const QString &searchString, QString replaceString, SearchType searchType, bool isIncremental) {

    static bool currentItemIsIncremental = false;

	/* Cancel accumulation of contiguous incremental searches (even if the
	   information is not worthy of saving) if search is not incremental */
    if (!isIncremental) {
        currentItemIsIncremental = false;
    }

	// Don't save empty search strings 
    if (searchString.isEmpty()) {
		return;
    }

	// If replaceString is nullptr, duplicate the last one (if any) 
    if(replaceString.isNull()) {
        replaceString = (NHist >= 1) ? ReplaceHistory[historyIndex(1)] : QLatin1String("");
    }

	/* Compare the current search and replace strings against the saved ones.
	   If they are identical, don't bother saving */
    if (NHist >= 1 && searchType == SearchTypeHistory[historyIndex(1)] && SearchHistory[historyIndex(1)] == searchString && ReplaceHistory[historyIndex(1)] == replaceString) {
		return;
	}

	/* If the current history item came from an incremental search, and the
	   new one is also incremental, just update the entry */
	if (currentItemIsIncremental && isIncremental) {

        SearchHistory[historyIndex(1)]     = searchString;
		SearchTypeHistory[historyIndex(1)] = searchType;
		return;
	}
	currentItemIsIncremental = isIncremental;

	if (NHist == 0) {
        for(MainWindow *window : MainWindow::allWindows()) {
            window->ui.action_Find_Again->setEnabled(true);
            window->ui.action_Replace_Find_Again->setEnabled(true);
            window->ui.action_Replace_Again->setEnabled(true);
		}
	}

	/* If there are more than MAX_SEARCH_HISTORY strings saved, recycle
	   some space, free the entry that's about to be overwritten */
    if (NHist != MAX_SEARCH_HISTORY) {
		NHist++;
    }

    SearchHistory[HistStart]     = searchString;
    ReplaceHistory[HistStart]    = replaceString;
	SearchTypeHistory[HistStart] = searchType;

    HistStart++;

    if (HistStart >= MAX_SEARCH_HISTORY) {
		HistStart = 0;
    }
}

/*
** return an index into the circular buffer arrays of history information
** for search strings, given the number of saveSearchHistory cycles back from
** the current time.
*/
int historyIndex(int nCycles) {

    if (nCycles > NHist || nCycles <= 0) {
		return -1;
    }

    int index = HistStart - nCycles;
    if (index < 0) {
		index = MAX_SEARCH_HISTORY + index;
    }

	return index;
}


/*
** Checks whether a search mode in one of the regular expression modes.
*/
int isRegexType(SearchType searchType) {
	return searchType == SEARCH_REGEX || searchType == SEARCH_REGEX_NOCASE;
}

/*
** Returns the default flags for regular expression matching, given a
** regular expression search mode.
*/
int defaultRegexFlags(SearchType searchType) {
	switch (searchType) {
	case SEARCH_REGEX:
		return REDFLT_STANDARD;
	case SEARCH_REGEX_NOCASE:
		return REDFLT_CASE_INSENSITIVE;
	default:
		// We should never get here, but just in case ... 
		return REDFLT_STANDARD;
	}
}

