# Parapad

Build `parapad.sln` in Visual Studio with the v142 C++ toolset and Windows SDK.
The application starts with an empty text buffer in command mode. Press `i` to
enter insert mode and Esc to return to command mode. The main window's status bar
shows `COMMAND`, `INSERT`, `VISUAL`, `VISUAL LINE`, or `VISUAL BLOCK` directly from the core's state.
The fixed black left gutter displays cyan one-based line numbers in a five-character field,
right-aligned and padded with spaces. Lines above 99999 display `#####`.
The editor retains its GDI bitmap and shifts existing rows during small vertical
scrolls. Only exposed rows or rows with changed bytes/selection are redrawn;
ordinary window exposure copies the cached pixels. Resizing, horizontal scrolling,
buffer replacement, and large jumps fall back to a full redraw.

In insert mode, typing inserts text, Enter splits the line, Backspace deletes the
previous character, and Tab inserts four spaces. Arrow keys, Home, End, Delete,
and mouse caret positioning work in all modes. The viewport follows the caret.
This is a Vim-style command subset, not a full Vim engine.

## Files

Use File > Open (`Ctrl+O`), Save (`Ctrl+S`), or Save As. File > New (`Ctrl+N`)
starts an empty document. The title shows the current path. New, Open, and Exit
prompt to save modified documents; cancelling or failing a save keeps the current
document open. Failed loads leave the current buffer, path, and viewport intact.

File I/O uses binary `rb`/`wb` streams and explicit byte lengths. Loading and saving
do not decode, transcode, normalize newlines, remove a BOM, or stop at NUL or
Ctrl+Z. Unedited files round-trip byte for byte, including arbitrary non-UTF-8 data.
Paths use ANSI dialogs and the system code page, consistent with the Win95 GUI.

The current text view uses LF as its line separator. Valid UTF-8 is displayed as
characters; malformed bytes show replacement glyphs and control bytes show dots,
without modifying their stored bytes. Navigation treats each malformed byte as a
single unit. CR bytes in CRLF files are retained and displayed as control bytes;
text typed through the keyboard still inserts LF. This is a raw-buffer foundation
for a future hex view, not an implemented hex editor.

## Commands

| Keys | Action |
| --- | --- |
| `h/j/k/l`, arrow keys | Move left/down/up/right |
| `w`, `b` | Move to the next/previous word; ASCII punctuation forms separate words |
| `0`, `$`, Home, End | Move to the start/end of the line |
| `v` | Toggle inclusive character selection |
| `V` (Shift+V) | Toggle whole-line selection |
| Ctrl+V | Toggle rectangular visual-block selection; motions extend the rectangle |
| `I` (Shift+I) in visual-block mode | Insert at the left edge on the top selected line; Esc repeats the insertion on the other selected lines |
| `y` in visual mode | Copy the selection to the core's register and return to command mode |
| `yy` | Copy the current line |
| `p` | Paste after the caret, or below the current line for a linewise register |
| `P` (Shift+P) | Paste before the caret, or above the current line for a linewise register |
| `p`, `P` in character/line visual mode | Replace the selection with the register contents |
| `x`, Delete | Delete selected text, or characters on the current line, saving them to the register |
| `d` plus a motion | Delete the motion's range, e.g. `dw`, `d$`, `d0`, `dj`, `dG` |
| `dd` | Delete the current line, saving it to the register |
| `y` plus a motion | Copy the motion's range, e.g. `yw`, `y$`, `yj`, `yG` |
| `gg`, `G` | Go to the first/last line |
| `12G`, `12gg` | Go to line 12 (one-based; clamped to existing lines) |
| Esc | Cancel selection, a pending operator/count, or insert mode |

Prefix motions, delete/yank operators, and paste with a decimal count: `3j`,
`4w`, `3x`, `2dd`, `2yy`, `3p`, `3P`. Counts before and after operators multiply:
`2d3w` deletes six word motions. A leading `0` is a motion, while zero after a
nonzero digit extends a count, e.g. `10j`. Counts are overflow-checked, and motion
at document boundaries stops without looping through the remaining count.
`d` and `y` wait for a motion or a second matching key; unsupported continuations
cancel the pending operator. Counts do not repeat insert-mode sessions.

Selections include both endpoint characters, preserve UTF-8 boundaries, and may
span lines in either direction. `v` and `V` switch between selection styles while
keeping the anchor; pressing the active selection key again cancels selection.
Yank/delete return to command mode. Linewise registers include a terminating LF,
even for the document's final unterminated line. Visual paste preserves the
register; subsequent paste can reuse it. Copy/paste uses a per-core internal UTF-8
register, not the Windows system clipboard, so no code-page conversion loses text.

Ctrl+V selects an inclusive rectangle in character columns rather than byte
offsets, retaining the target column when moving vertically through short lines.
The GUI highlights each row's selected bytes and draws stacked secondary carets.
`x`, Delete, or `d` deletes the rectangle without removing line separators.
`I` starts a block insert: type on the top selected line, then press Esc to repeat
the final inserted text on the remaining selected lines. Backspace can remove
characters typed during that session but cannot erase before its starting point.
Lines shorter than the insertion column are skipped; no padding is added. The
starting column is clamped to the top selected line. Moving the caret, clicking,
pressing Delete, or inserting a newline ends repetition and continues ordinary
editing on the first line. An empty insert followed by Esc changes nothing.
Block yank/delete stores newline-separated row slices in the ordinary register;
blockwise paste is not implemented, and `p`/`P` while selecting a block is a no-op.

## Editor Core

`editor_core.h` exposes an opaque `EditorCore` with no Windows dependencies.
Create an instance with `editor_core_create(initial_text)` and release it with
`editor_core_destroy`. Creation copies the supplied text and places the caret
at the beginning; NULL creates an empty document.

`editor_core_send(core, message, data)` returns 1 on success (including boundary
no-ops) and 0 for invalid arguments, an unknown message, an invalid line, or an
allocation failure. Failed edits leave the document unchanged.

| Message | Data |
| --- | --- |
| `EDITOR_HANDLE_KEY` | `unsigned long *` containing a Unicode scalar or an `EditorKey` special-key value |
| `EDITOR_GET_MODE` | `EditorMode *` receiving the current command/insert/visual/visual-line/visual-block mode |
| `EDITOR_GET_LINE_CARET` | `EditorLineCaret *`; supply `line`, receive byte `column`, extra character-cell `advance`, and `active` for block carets |
| `EDITOR_SET_BUFFER` | `EditorBuffer *`; copy exactly `length` bytes from `data`, without conversion |
| `EDITOR_GET_BUFFER` | `EditorBuffer *` receiving borrowed raw `data` and byte `length` |
| `EDITOR_LOAD_FILE` | NUL-terminated ANSI path; load the file verbatim in binary mode |
| `EDITOR_SAVE_FILE` | NUL-terminated ANSI path; save exactly the current buffer's byte length |
| `EDITOR_GET_MODIFIED` | `int *` receiving whether edits have occurred since creation, replacement, load, or save |
| `EDITOR_GET_SELECTION` | `EditorSelection *`; supply `line`, receive byte `start`/exclusive `end`, `active`, and `includes_newline` |
| `EDITOR_GET_REGISTER` | `EditorRegister *` receiving borrowed UTF-8 `text`, byte `length`, and `linewise` |
| `EDITOR_ADD_CHAR` | NUL-terminated UTF-8 `char *` containing exactly one Unicode scalar |
| `EDITOR_ADD_CODEPOINT` | `unsigned long *` containing a nonzero Unicode scalar, encoded by the core |
| `EDITOR_INSERT_TEXT` | NUL-terminated UTF-8 `char *` to insert at the caret |
| `EDITOR_BACKSPACE` | NULL; delete the character before the caret |
| `EDITOR_DELETE_CHAR` | NULL; delete the character at the caret |
| `EDITOR_GET_LINE` | `EditorLine *`; supply `index`, receive `text` and `length` |
| `EDITOR_GET_CHARACTER` | `EditorCharacter *`; supply `line` and byte `column`, receive scalar `value` and byte `length` |
| `EDITOR_DELETE_LINE` | `size_t *` containing the line index |
| `EDITOR_GET_LINE_COUNT` | `size_t *` receiving the count |
| `EDITOR_GET_CARET` | `EditorCaret *` receiving the position |
| `EDITOR_SET_CARET` | `EditorCaret *` supplying the position; column is clamped |
| `EDITOR_MOVE_LEFT`, `EDITOR_MOVE_RIGHT` | NULL; move across characters and line boundaries |
| `EDITOR_MOVE_UP`, `EDITOR_MOVE_DOWN` | NULL; move one line, clamping the column |
| `EDITOR_MOVE_HOME`, `EDITOR_MOVE_END` | NULL; move to the current line boundary |

`EDITOR_HANDLE_KEY` runs the core's per-instance state machine. ASCII `i` changes
command mode to insert mode without inserting itself; scalar 27 (Esc) returns to
command mode without inserting a character. Backspace, Tab, and Enter are passed
as their ASCII control values. Navigation and Delete use the `EditorKey` enum.
Ctrl+V is scalar 22. Block selection and deferred insertion are owned by the core;
`EDITOR_GET_LINE_CARET` supplies per-line caret positions for rendering. During
deferred insertion, secondary rows remain unchanged until Esc, and `advance`
positions their preview carets after the pending inserted characters. The primary
caret remains available through `EDITOR_GET_CARET`. New/load clears block state.
Mode transitions are synchronous and can be queried with `EDITOR_GET_MODE`.
The GUI sends key messages without interpreting commands or deciding transitions.
Direct buffer-edit and navigation messages remain mode-independent for loading
text, programmatic edits, and other non-keyboard clients.

The document is a length-based byte buffer, independent of the system locale.
`EDITOR_SET_BUFFER` and `EDITOR_LOAD_FILE` accept arbitrary bytes, including NUL
and invalid UTF-8. Replacing or loading the buffer resets the caret, command state,
selection, and modified flag, while preserving the yank register. The buffer API
can therefore be reused by a future hex editor without a text conversion step.
`EDITOR_GET_BUFFER` returns a borrowed pointer valid until mutation or destruction;
always use its length rather than `strlen`. An empty buffer may be supplied as
`{ NULL, 0 }`. Replacement copies the input and also supports a slice of the old
buffer. File-load failures leave the original document unchanged. Save reports
write/close failures and clears the modified flag only after successful completion.

Text insertion APIs still accept validated UTF-8 `char` strings. They reject
malformed sequences, surrogate values, and values above U+10FFFF. Those string
APIs cannot insert embedded NUL; use the raw buffer API for binary content.
Line and column indices are zero-based byte offsets. Navigation and deletion
preserve valid Unicode scalars and advance one byte over malformed data. Setting
a column inside a valid UTF-8 sequence clamps to its start. Character queries
return U+FFFD with length 1 for an invalid byte. Combining sequences are not treated
as single grapheme clusters.
CRLF and CR are normalized to LF only during text insertion, never raw loading.
Empty documents have one
empty line, and a trailing LF creates a final empty line. Deleting a line removes
its following newline, or the preceding newline when deleting the final line.
Backspace and Delete join adjacent lines when removing a newline.

`EDITOR_GET_SELECTION` exposes the selected span on a requested line without
requiring the GUI to implement command logic. An empty selected line or a selected
newline can have equal start/end offsets; `includes_newline` lets a renderer show
that selection. Direct buffer mutations cancel visual mode to avoid stale anchors.

`EDITOR_GET_LINE` returns a borrowed slice, not a NUL-terminated line. Use its
length, do not modify it, and query it again after any text mutation. The pointer
is invalid after a mutation or destruction. Dispatch is synchronous and instances
must not be accessed concurrently without external synchronization.
Register text is NUL-terminated and remains valid until the next successful
yank/delete or core destruction; clients must not modify it.

## ANSI Frontend

The GUI uses ANSI window and GDI entry points, not the Win95 Unicode stubs.
The core handles all UTF-8 encoding and decoding. The GUI queries Unicode scalar
values for layout and converts them to the active ANSI code page for `TextOutA`.
ANSI `WM_CHAR` input, including DBCS lead/trail bytes, is converted using
`MultiByteToWideChar(CP_ACP)` and sent to the core as scalars. The temporary
`WCHAR` units used by the Win95 code-page conversion functions are not editor
buffers. No `CP_UTF8` support is required from the operating system.

Characters unavailable in the active code page or font may appear as replacement
glyphs; their UTF-8 bytes remain intact. Full Unicode rendering on Windows 95
requires a separate renderer and suitable fonts. The current v142 toolchain/CRT
does not produce Windows 95-compatible binaries; an appropriate legacy toolchain
and actual Windows 95 testing are still required for deployment there. Windows 95
is 32-bit only.

## Core Tests

From a Visual Studio Developer Command Prompt in the repository directory:

```bat
if not exist Debug\core-tests mkdir Debug\core-tests
cl /nologo /W4 /WX /std:c11 /FoDebug\core-tests\ /FeDebug\core-tests\editor_core_tests.exe editor_core.c editor_core_tests.c
Debug\core-tests\editor_core_tests.exe
```

The standalone tests do not link the GUI or Windows libraries. Assertions must
remain enabled (do not define `NDEBUG`). Test objects use a separate directory so
the command prompt's target architecture cannot overwrite application objects.