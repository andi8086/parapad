# Parapad

Build `parapad.sln` in Visual Studio with the v142 C++ toolset and Windows SDK.
The application starts with an empty text buffer in command mode. Press `i` to
enter insert mode and Esc to return to command mode. The main window's status bar
shows `COMMAND`, `INSERT`, `REPLACE`, `VISUAL`, `VISUAL LINE`, `VISUAL BLOCK`, or `COMMAND LINE`
directly from the core's state.
The fixed black left gutter displays cyan one-based line numbers in a five-character field,
right-aligned and padded with spaces. Lines above 99999 display `#####`.
The editor retains its GDI bitmap and shifts existing rows during small vertical
scrolls. Only exposed rows or rows with changed bytes/selection are redrawn;
ordinary window exposure copies the cached pixels. Resizing, horizontal scrolling,
buffer replacement, and large jumps fall back to a full redraw.

In insert mode, typing inserts text, Enter splits the line, Backspace deletes the
previous character, and Tab advances to the next eight-column tab stop by default.
Arrow keys, Home, End, Delete,
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
characters; malformed bytes show replacement glyphs and control bytes other than
Tab show dots,
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
| `r` then a character | Replace the character under the caret, or each selected character in visual mode |
| `R` (Shift+R) in command mode | Enter continuous replace mode; Esc returns to command mode |
| `:` in command mode | Open the command input above the status bar; Enter executes and Esc cancels |
| `u` in command mode | Undo the last change; `3u` undoes three changes |
| Ctrl+R in command mode | Redo an undone change; `3` then Ctrl+R redoes three changes |
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
| Esc | Cancel selection, a pending operator/count, or insert/replace mode |

Prefix motions, delete/yank operators, and paste with a decimal count: `3j`,
`4w`, `3x`, `2dd`, `2yy`, `3p`, `3P`. Counts before and after operators multiply:
`2d3w` deletes six word motions. A leading `0` is a motion, while zero after a
nonzero digit extends a count, e.g. `10j`. Counts are overflow-checked, and motion
at document boundaries stops without looping through the remaining count.
`d` and `y` wait for a motion or a second matching key; unsupported continuations
cancel the pending operator. Counts do not repeat insert/replace-mode sessions.

`r` waits for the replacement character; Esc cancels it. A count such as `3rx`
replaces three characters with `x`, staying on the current line. If fewer than
three characters remain, nothing changes. The caret lands on the last replaced
character. Visual `r` supports character, line, and block selections, preserving
existing line separators. Replacement does not overwrite the yank register.

In `R` mode, typed characters overwrite complete UTF-8 characters and append at
the end of a line without consuming its newline. Enter inserts a line break;
Tab uses the core settings: expanded tabs overwrite characters with enough spaces
to reach the next virtual tab stop, and literal tabs replace one character. Backspace restores the
previous overwritten character (or removes text appended beyond the line end).
One Backspace restores an entire Tab operation. Arrow keys, Home, End, Delete,
mouse positioning, and direct edit/navigation messages clear restoration history;
Backspace with no history does nothing. Esc leaves the caret on the last typed
character when possible. New/load clears replace mode and its history.

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

## Settings

Press `:` in normal (command) mode to open and focus the command input above the
status bar. For example:

```text
:set maxcol=100
:set tabwidth=4
:set expandtabs=0
:set expandtabs=1
```

Enter applies the setting, closes the input, and returns focus to the editor.
Esc closes it without applying the pending command. The input is hidden and
disabled when inactive, and does not open for colons typed in insert/replace
mode or as the character following `r`. Commands are limited to 1023 input bytes.

Values are unsigned decimal integers; `expandtabs` accepts only numeric 0 or 1.
Whitespace around `=` is allowed. Unknown commands/settings, negative values,
invalid booleans, trailing text, integer overflow, and zero `tabwidth` are
rejected without changing settings. The input stays open for correction and the
status bar shows `invalid command`.

Each core owns an `EditorSettings` value, queried with `EDITOR_GET_SETTINGS` and
updated with `EDITOR_SET_SETTINGS`.

| Field | Default | Meaning |
| --- | --- | --- |
| `maxcol` | `80` | Draw a thin gray vertical guide after this many character cells; `0` hides it |
| `tabwidth` | `8` | Spacing between virtual tab stops for both expanded and literal tabs |
| `expandtabs` | `1` (true) | Insert spaces for Tab; when false, insert one real `\t` byte |

Expanded Tab inserts enough spaces to reach the next virtual tab stop. With
`tabwidth = 8`, column 0 inserts eight spaces and column 3 inserts five. Existing
tabs and UTF-8 characters are included in the virtual-column calculation.
Stored tabs are displayed as whitespace extending to the next tab stop, with
matching caret measurement, selection highlighting, and mouse hit-testing.
`tabwidth` must be positive and less than `SIZE_MAX`; invalid settings leave the
previous values intact. `expandtabs` is normalized to zero or one.

Settings survive New/load/buffer replacement and are not document edits or undo
steps. They do not change existing bytes or mark the document modified. Hosts
should refresh the view after changing settings. The retained bitmap is rebuilt
when `maxcol` or `tabwidth` changes. The guide moves with horizontal scrolling;
it does not wrap text or color overlong lines yet.

## Undo And Redo

`u` and Ctrl+R restore document bytes and caret positions, returning to command
mode and clearing selections and pending commands. An insert or replace session
is one undo step until Esc; block insertion includes the repetitions applied on
Esc. Arrow/Home/End navigation, mouse positioning, direct edit/navigation messages,
and successful saves end the current group. Each delete, paste, or one-shot
replacement command is a separate step. Read-only queries do not break groups.
Direct edit messages each create their own undo step.

Editing after undo discards the redo branch. Navigation, yank, and no-op edits
preserve it. Undo/redo does not change the yank register. Reaching either end of
history is a no-op. If an undo/redo command restores no change, the status line
shows `no change` until the next editor command. Read-only queries and redraws
preserve the message. New, successful load, and buffer replacement clear history;
saving retains it and records the clean revision, so undo/redo back to that
revision clears the modified flag. History uses complete byte-buffer snapshots,
preserving NULs and malformed UTF-8. It is per document instance, kept only in
memory, and limited by available memory rather than a fixed step count. This is
linear undo/redo, not Vim's branching undo tree or persistent undo files.

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
| `EDITOR_GET_MODE` | `EditorMode *` receiving the current command/insert/replace/visual/visual-line/visual-block/command-line mode |
| `EDITOR_GET_STATUS` | `EditorStatus *` receiving `EDITOR_STATUS_NONE`, `EDITOR_STATUS_NO_CHANGE`, or `EDITOR_STATUS_INVALID_COMMAND` without clearing it |
| `EDITOR_GET_SETTINGS` | `EditorSettings *` receiving `maxcol`, `tabwidth`, and `expandtabs` |
| `EDITOR_SET_SETTINGS` | `const EditorSettings *` supplying all three settings |
| `EDITOR_EXECUTE_COMMAND` | NUL-terminated command string, with an optional leading `:`; parse and apply `set name=value` in the core |
| `EDITOR_GET_LINE_CARET` | `EditorLineCaret *`; supply `line`, receive byte `column`, extra character-cell `advance`, and `active` for block carets |
| `EDITOR_SET_BUFFER` | `EditorBuffer *`; copy exactly `length` bytes from `data`, without conversion |
| `EDITOR_GET_BUFFER` | `EditorBuffer *` receiving borrowed raw `data` and byte `length` |
| `EDITOR_LOAD_FILE` | NUL-terminated ANSI path; load the file verbatim in binary mode |
| `EDITOR_SAVE_FILE` | NUL-terminated ANSI path; save exactly the current buffer's byte length |
| `EDITOR_GET_MODIFIED` | `int *` receiving whether edits have occurred since creation, replacement, load, or save |
| `EDITOR_UNDO`, `EDITOR_REDO` | NULL; restore one history step, returning to command mode |
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
command mode without inserting a character. ASCII `R` enters
`EDITOR_MODE_REPLACE`, while `r` waits for a one-shot replacement character.
In normal mode, `:` enters `EDITOR_MODE_COMMAND_LINE`; the frontend edits the
command text and submits it with `EDITOR_EXECUTE_COMMAND`. Successful execution
returns to command mode; invalid input returns 0, sets the invalid-command
status, and leaves command-line mode active. Esc cancels command-line mode.
Backspace, Tab, and Enter are passed
as their ASCII control values. Navigation and Delete use the `EditorKey` enum.
Ctrl+V is scalar 22. Block selection and deferred insertion are owned by the core;
Ctrl+R is scalar 18; ASCII `u` invokes undo outside insert/replace mode.
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