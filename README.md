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
adds an empty buffer; Open adds a loaded file without replacing any open buffer
or prompting to discard its edits. The title shows the active buffer's full path.

The list at the far left displays open filenames, or `Untitled` for unnamed
buffers. Click a filename to switch buffers; a surrounding box marks the active
one, and `*` marks unsaved changes. Long filenames are ellipsized within the list.
Each buffer retains its own text, caret, scroll position, mode, selection,
settings, undo/redo history, search pattern, repeat command, and yank register.
Switching buffers cancels any open search/colon input without submitting it.

The status bar shows the current one-based row and total lines (`LN 35/2102`),
then the display column, followed by the caret's byte-position percentage of
the whole file, `(byte offset / total bytes)`, and the LF/CRLF format.
Columns include tab expansion and count each Unicode scalar as one cell.
The percentage uses serialized file bytes, including CRLF pairs, rather than
line count or scroll position. Byte offsets are zero-based. An empty buffer
shows `LN 1/1, Col 1`, `100% (0 / 0)`, and
`LF`; the end-of-file position in a nonempty buffer shows `100%`.

File > Close (`Ctrl+W`) closes only the active buffer, asking whether to save
unsaved changes. Cancel or a failed save keeps it open. Closing the final buffer
creates a fresh untitled buffer. Exit checks every modified buffer, including
inactive ones; cancelling keeps the application open with that buffer selected.
Failed loads leave existing buffers and the selected buffer's state intact.

File I/O uses binary `rb`/`wb` streams and explicit byte lengths. Loading and saving
do not decode, transcode, remove a BOM, or stop at NUL or Ctrl+Z. LF and CRLF files
retain their line-ending format; other bytes, including arbitrary non-UTF-8 data,
are preserved. Mixed LF/CRLF input selects CRLF and is saved consistently as CRLF.
Paths use ANSI dialogs and the system code page, consistent with the Win95 GUI.

### Directory Buffers

`:tabe .` opens a directory listing in a new buffer. `:tabe <directory>` also
accepts directory paths, including quoted paths containing spaces. Move with
Up/Down or `j`/`k`, then press Enter or double-click a row. Directories have a
trailing `/`; `../`, Backspace, or `-` navigates to the parent. Parent comes
first, followed by sorted directories and then sorted files. The active row is
highlighted and the status bar shows `DIRECTORY`.

Each directory buffer has its own current path and navigation state. Navigation
never calls `SetCurrentDirectory` or `chdir`, so it cannot change another buffer
or the process working directory. Relative `:tabe` paths in a directory buffer
resolve against that buffer's path. Selecting a file loads it into the same
buffer, replaces the listing, updates the title/sidebar and lexer, and leaves
the tab count unchanged. Failed enumeration or file loading preserves the
listing. Listings are read-only and cannot be saved or edited.

Windows enumeration uses Win95-supported `FindFirstFileA`/`FindNextFileA` and
the full `cFileName`, never the 8.3 `cAlternateFileName`. Long filenames and
spaces are retained in native ANSI paths; only displayed names are converted
to UTF-8. Paths must fit the Win95 `MAX_PATH` limit (260 bytes including the
terminator); oversized paths fail instead of being silently truncated.
This does not require Unicode filesystem APIs or modern long-path extensions.

The current text view uses LF as its line separator. Valid UTF-8 is displayed as
characters; malformed bytes show replacement glyphs and control bytes other than
Tab show dots,
without modifying their stored bytes. Navigation treats each malformed byte as a
single unit. CRLF pairs are displayed as one line break, without a dot for CR.
Standalone CR bytes remain control bytes. Editing uses normalized LF boundaries
internally and transparently exports the buffer's selected LF or CRLF format.

## Syntax Highlighting

Files whose names end in `.c`, `.h`, or `.hpp` (case-insensitive) automatically
use the C/C++ lexer. `.asm`, `.inc`, and `.s` select the Intel assembly lexer.
Only `.mk` (case-sensitive) or an exact extensionless basename `Makefile` selects
the Makefile lexer. Directory names are ignored, including dots in directories;
`Makefile.txt`, `GNUmakefile`, and unrelated extensionless files do not match.
`.bat` and `.cmd` (case-insensitive) select the DOS batch lexer.
Save As switches lexers or disables highlighting when the extension changes;
unnamed buffers and other extensions keep the existing plain-text colors.
Keywords are cyan, comments green, strings and character literals yellow,
numbers violet, and preprocessor/assembler directives orange. Ordinary source text is
light gray on black. Selection colors take precedence.

`lexer.h` defines a language-neutral descriptor: extension matching, line lexing,
state comparison, and line cleanup callbacks. Each core registers all built-in
lexers at creation. `EDITOR_SELECT_LEXER` asks the registered lexers whether they
support a filename's extension or basename, and `EDITOR_GET_LEXER` returns the
selected descriptor, or NULL for plain text. The optional `supports_filename`
callback receives only the basename, not its directory. Each lexer owns its
matching rules; the window and renderer contain no language list.

To add a built-in language, implement a `Lexer` descriptor, add its source to the
project and its descriptor to `available_lexers` in the core. Hosts can also use
`EDITOR_REGISTER_LEXER` to register additional descriptors, then select again.
The first matching registered lexer wins. Descriptors must outlive the core and
any cached lines. `lex_line` receives NULL for the initial state and returns owned
tokens and opaque ending state; `free_line` releases both and must accept a
zero-initialized line. Failed lexing must release its temporary allocations.

The standalone `lexer_c_cpp.c` module returns byte-based token spans and ending
line states, without Windows or editor-core dependencies. It handles multiline
comments, escaped and continued literals, continued directives and line
comments, include filenames, and C++ raw strings. It is a lexical highlighter,
not a preprocessor or a complete C/C++ parser; inactive conditional branches
are not evaluated. Raw document bytes are never changed.

`lexer_intel_asm.c` recognizes Intel-syntax mnemonics case-insensitively from
8086 through Pentium MMX: integer, string, bit, control-flow, protected-mode,
privileged/system, the complete pre-P6 x87 set (including waiting/nonwaiting
forms), and every original MMX mnemonic. All Jcc/SETcc condition aliases,
size-specific string/stack/return forms, repeat/lock/size prefixes, and legacy
mnemonics such as SALC, ICEBP, LOADALL, IBTS/XBTS, and FFREEP are included.
Pentium MMX's RDPMC is included; Pentium Pro-only CMOV/FCMOV/FCOMI and later
SSE, extended MMX, 3DNow!, and x86-64 additions are not part of this lexer.
The instruction families are cross-referenced against the
[NASM instruction list](https://www.nasm.us/doc/nasmaf.html).

Assembly highlighting also recognizes 8/16/32-bit, segment, control/debug/test,
x87, and MMX registers; NASM/MASM-style directives and operand qualifiers;
decimal, binary, octal, hexadecimal, and decimal floating literals; quoted
strings (including doubled quotes and backtick escapes); semicolon comments;
and MASM delimiter-based multiline `COMMENT` blocks. Labels and other symbols
retain the ordinary text color. This is highlighting, not operand validation,
macro expansion, or CPU-feature checking. `.s` is interpreted as Intel syntax,
not AT&T syntax. Like C/C++, this lexer has no Windows dependency.

`lexer_makefile.c` highlights rule targets, assignment names/operators,
Make directives, nested `$(...)`/`${...}` and automatic variable expansions,
comments, and quoted recipe strings. It tracks backslash continuations,
continued recipes/comments, and `define`/`endef` nesting across lines.
Recipe recognition uses the standard leading tab. This is a lexical
highlighter, not a Make evaluator or shell parser; custom `.RECIPEPREFIX`
settings and variable-generated syntax are not evaluated.

`lexer_batch.c` highlights DOS/Windows command keywords, control words,
labels, `REM`/`::` comments, redirection and command separators, quoted text,
numbers, `%NAME%`, positional parameters, FOR variables, `%~` parameter
modifiers, and delayed `!NAME!` expansions. Caret continuations preserve
command context. Commands and extensions are case-insensitive. This lexer
does not execute commands or expand variables.

Each buffer owns its selected lexer and token/state cache. The renderer uses only
the generic callbacks and token kinds; switching lexers clears the previous
lexer's cached lines and invalidates their paint versions. Core change ranges start incremental
re-lexing at the first affected line; processing continues through the changed
range until the ending state converges with the cached state. Line insertion
or deletion, raw replacement, and undo/redo conservatively invalidate the
remaining affected lines. Token versions also invalidate retained bitmap rows,
so changing a multiline comment repaints affected lines even if their bytes
did not change.

In all buffers, displayed text beyond `maxcol` is white on red. The first
`maxcol` display columns retain their normal colors. Literal tabs use their
virtual tab stops; only the overflowing part of a tab's width is marked.
`maxcol=0` disables both this marking and the gray guide. Selected text retains
the system selection colors.

## Commands

| Keys | Action |
| --- | --- |
| `h/j/k/l`, arrow keys | Move left/down/up/right |
| `a` in command mode | Enter insert mode after the current character, staying on the same line |
| `A` (Shift+A) in command mode | Enter insert mode at the end of the current line |
| `w`, `b` | Move to the next/previous word; ASCII punctuation forms separate words |
| `e` | Move to the end of the current/next word; supports counts and inclusive `de`/`ye` operators |
| `/` in command mode | Open forward literal-text search; Enter searches and Esc cancels |
| `n` | Repeat the latest search, wrapping at the document end; `3n` repeats three matches |
| `N` (Shift+N) | Repeat the latest search backward, wrapping at the document beginning; supports counts |
| `*` | Search forward for the whole word under the caret; supports counts and wrapping |
| `0`, `$`, Home, End | Move to the start/end of the line |
| `^` | Move to the first character other than a space or Tab on the current line; also works with operators |
| `v` | Toggle inclusive character selection |
| `V` (Shift+V) | Toggle whole-line selection |
| Ctrl+V | Toggle rectangular visual-block selection; motions extend the rectangle |
| `I` (Shift+I) in visual-block mode | Insert at the left edge on the top selected line; Esc repeats the insertion on the other selected lines |
| `r` then a character | Replace the character under the caret, or each selected character in visual mode |
| `R` (Shift+R) in command mode | Enter continuous replace mode; Esc returns to command mode |
| `:` in command mode | Open the command input above the status bar; Enter executes and Esc cancels |
| `u` in command mode | Undo the last change; `3u` undoes three changes |
| Ctrl+R in command mode | Redo an undone change; `3` then Ctrl+R redoes three changes |
| `.` in command mode | Repeat the last keyboard editing command at the caret; a count overrides its saved count |
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

`.` repeats keyboard changes such as `x`, `dw`, `dd`, `r`, paste, and completed
insert/replace sessions. Motions, searches, yanks, undo/redo, and empty insert
sessions leave the last change available. Each replay is one undo step. A count
such as `3.` replaces the saved command count, including counts on operator
motions; for insert/replace sessions it repeats the session's input three times.
Later `.` presses retain that override until a new change is recorded. Visual
changes replay their selection motions relative to the new caret. Arrow or mouse
navigation separates insert/replace recordings, matching undo boundaries.
Direct edit API calls and colon commands are not recorded. With no previous
keyboard change, `.` leaves the buffer unchanged and displays `no change`.

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

## Search

Press `/`, type a case-sensitive literal pattern, and press Enter. Search starts
after the caret and wraps to the document beginning. `n` repeats the latest
pattern; counts such as `3n` advance multiple matches. `N` (Shift+N) searches
backward, wrapping to the final match; `3N` moves back three matches. An empty `/` submission
reuses the previous pattern. Search patterns are UTF-8, and the ANSI frontend
converts native input before dispatch. Matches start on character boundaries.
`*` selects the word under the caret (or the next word on the current line when
on whitespace) and searches for its next whole-word occurrence. It uses the same
word classes as word motions, including separate punctuation runs. Counts such
as `2*` skip multiple occurrences. Subsequent `n`/`N` retain whole-word matching;
entering a new nonempty `/` pattern returns to literal substring matching.
Regular expressions, `?` search input, and search highlighting are not implemented.

A failed search leaves the caret unchanged and displays `pattern not found`;
the search input remains open for correction or Esc. `n`/`N` without a pattern shows
`no previous search`. Searching does not modify bytes, the yank register, or undo
history. Each open buffer retains its own search pattern when switching buffers.

## Settings

Press `:` in normal (command) mode to open and focus the command input above the
status bar. For example:

```text
:set maxcol=100
:set tabwidth=4
:set expandtabs=0
:set expandtabs=1
:set lf
:set crlf
:retab
```

Enter executes the command, closes the input, and returns focus to the editor.
Esc closes it without applying the pending command. The input is hidden and
disabled when inactive, and does not open for colons typed in insert/replace
mode or as the character following `r`. Commands are limited to 1023 input bytes.

Numeric setting values are unsigned decimal integers; `expandtabs` accepts only 0 or 1.
Whitespace around `=` is allowed. Unknown commands/settings, negative values,
invalid booleans, trailing text, integer overflow, and zero `tabwidth` are
rejected without changing settings. The input stays open for correction and the
status bar shows `invalid command`.

`:set lf` and `:set crlf` convert every line ending in the current buffer to the
requested format. The rightmost status-bar field displays `LF` or `CRLF` for the
active buffer. Loading detects CRLF if any CRLF pair is present; otherwise it
selects LF, including for empty files. New buffers default to LF. Insert, replace,
paste, line joins, and save consistently use that buffer's format. Changing the
format marks the buffer modified and is one undo/redo step; selecting the current
format is a no-op. Conversions preserve the caret, selection, BOM, NULs, malformed
UTF-8, and standalone CR bytes, and never add or remove a final line break.
Directory listings reject these conversions.

`:retab` replaces every real tab in the document with spaces reaching the next
virtual tab stop, using the current `tabwidth`, but only when `expandtabs=1`.
With `expandtabs=0`, or when no real tabs remain, it does nothing. Existing spaces
and all other bytes are preserved, including NULs, malformed UTF-8, and line
endings. The caret is remapped past expanded tabs, and the complete conversion
is one undo/redo step. The command takes no arguments.

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

Settings are per buffer; new buffers start with the defaults above. Within a core,
settings survive raw load/buffer replacement and are not document edits or undo
steps. They do not change existing bytes or mark the document modified. Hosts
should refresh the view after changing settings. The retained bitmap is rebuilt
when `maxcol` or `tabwidth` changes. The guide moves with horizontal scrolling;
it does not wrap text. Text beyond the guide is displayed white on red.

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
preserve the message. New/Open buffers have independent histories; switching
buffers preserves them. A raw core load or buffer replacement clears its history;
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
| `EDITOR_GET_STATUS` | `EditorStatus *` receiving the current no-change, invalid-command, pattern-not-found, or no-previous-search status, or `EDITOR_STATUS_NONE`, without clearing it |
| `EDITOR_GET_COMMAND_PREFIX` | `char *` receiving `:` for settings/commands or `/` for search input |
| `EDITOR_GET_SETTINGS` | `EditorSettings *` receiving `maxcol`, `tabwidth`, and `expandtabs` |
| `EDITOR_SET_SETTINGS` | `const EditorSettings *` supplying all three settings |
| `EDITOR_EXECUTE_COMMAND` | NUL-terminated UTF-8 input; execute `set name=value`, `set lf`, `set crlf`, or `retab` with an optional `:`, or search with `/pattern` |
| `EDITOR_GET_LINE_CARET` | `EditorLineCaret *`; supply `line`, receive byte `column`, extra character-cell `advance`, and `active` for block carets |
| `EDITOR_SET_BUFFER` | `EditorBuffer *`; copy `length` arbitrary bytes from `data`, detect line endings, and normalize CRLF pairs for editing |
| `EDITOR_GET_BUFFER` | `EditorBuffer *` receiving borrowed file-format `data` and byte `length`, serialized in the selected LF/CRLF mode |
| `EDITOR_GET_TEXT_BUFFER` | `EditorBuffer *` receiving borrowed normalized editing text and byte length; uses the same offsets as caret/line queries |
| `EDITOR_GET_LINE_ENDING` | `EditorLineEnding *` receiving `EDITOR_LINE_ENDING_LF` or `EDITOR_LINE_ENDING_CRLF` |
| `EDITOR_GET_POSITION` | `EditorPosition *` receiving zero-based `line` and display `column`, total `line_count`, plus caret byte `offset` and total byte `length` in the selected serialized file format; read-only |
| `EDITOR_LOAD_FILE` | NUL-terminated ANSI path; read in binary mode and detect/normalize line endings for editing |
| `EDITOR_SAVE_FILE` | NUL-terminated ANSI path; save arbitrary bytes with line breaks in the selected format |
| `EDITOR_GET_MODIFIED` | `int *` receiving whether edits have occurred since creation, replacement, load, or save |
| `EDITOR_TAKE_CHANGE` | `EditorChange *` receiving the change revision and accumulated inclusive changed-line range; consumes the range without affecting undo groups |
| `EDITOR_REGISTER_LEXER` | `const Lexer *`; register an additional lexer descriptor on this core; registering the same pointer again is a no-op |
| `EDITOR_SELECT_LEXER` | NUL-terminated filename; select the first registered lexer matching its basename or extension, or plain text when none matches |
| `EDITOR_GET_LEXER` | `const Lexer **` receiving the selected descriptor, or NULL for plain text |
| `EDITOR_REGISTER_DIRECTORY_FUNCTIONS` | `const EditorDirectoryFunctions *`; register host directory listing and local path-resolution callbacks |
| `EDITOR_OPEN_DIRECTORY` | NUL-terminated host-native path; replace an unmodified buffer with a read-only listing, resolving relative to its current directory when browsing |
| `EDITOR_ACTIVATE_DIRECTORY_ENTRY` | NULL; enter the directory or load the file at the caret, replacing the listing in place |
| `EDITOR_GET_DIRECTORY` | `EditorDirectory *`; receive the current browser path and whether the buffer is a directory listing |
| `EDITOR_TAKE_DIRECTORY_CHANGE` | `EditorDirectory *`; receive a pending browser path change, or NULL path when none; consumes the notification |
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
command mode without inserting a character. ASCII `a` and `A` enter insert mode
after a character or at line end. ASCII `R` enters
`EDITOR_MODE_REPLACE`, while `r` waits for a one-shot replacement character.
In normal mode, `:` or `/` enters `EDITOR_MODE_COMMAND_LINE`; the core supplies
the prompt through `EDITOR_GET_COMMAND_PREFIX`. The frontend edits the input
and submits it with `EDITOR_EXECUTE_COMMAND`. Successful execution
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

Directory callbacks are copied into each core and survive document replacement.
Their context must outlive the core or be replaced before it expires. `list`
receives a resolved directory path and an `EditorDirectoryEmit` callback; emit
one entry per row with a UTF-8 display name, host-native full path, and directory
flag. Include a parent entry named `..` when appropriate. Emitted strings are
copied synchronously; stop and report failure if emit returns zero. `change`
receives the current browser path (NULL when not browsing) and requested path;
it must validate/resolve a directory without changing global working-directory
state and return a `malloc`-allocated path through `resolved_path`. The core
owns/frees that path, including any allocation returned on failure. Both
callbacks return 1 for success or 0 for failure and must not reenter the core.
Directory query/notification paths are borrowed until the next successful
document replacement/navigation or core destruction. A file-selection
notification has `active == 0`; hosts should update their filename without
adding another buffer. Failed operations do not discard the old listing.
The GUI sends key messages without interpreting commands or deciding transitions.
Direct buffer-edit and navigation messages remain mode-independent for loading
text, programmatic edits, and other non-keyboard clients.

The document is a length-based byte buffer, independent of the system locale.
`EDITOR_SET_BUFFER` and `EDITOR_LOAD_FILE` accept arbitrary bytes, including NUL
and invalid UTF-8. Replacing or loading the buffer resets the caret, command state,
selection, and modified flag, while preserving the yank register. The buffer API
normalizes CRLF pairs internally while preserving all other bytes. Uniform LF
or CRLF input round-trips exactly unless edited or explicitly converted.
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
CRLF and CR are normalized to LF during text insertion. Loading and raw buffer
replacement normalize only CRLF pairs and retain standalone CR bytes. Line/caret
offsets refer to normalized editing text, not serialized file-byte offsets;
`EDITOR_GET_TEXT_BUFFER` exposes that representation for lexers and other views.
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
The core stores UTF-8 text independently of the system code page. The GUI queries Unicode scalar
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
cl /nologo /W4 /WX /std:c11 /FoDebug\core-tests\ /FeDebug\core-tests\editor_core_tests.exe editor_core.c lexer_c_cpp.c lexer_intel_asm.c lexer_makefile.c lexer_batch.c editor_core_tests.c
Debug\core-tests\editor_core_tests.exe
```

## Native Window Tests

From a Visual Studio Developer Command Prompt, run:

```bat
if not exist Debug\core-tests mkdir Debug\core-tests
cl /nologo /W3 /WX /std:c11 /FoDebug\core-tests\ /FeDebug\core-tests\editor_window_tests.exe editor_window_tests.c editor_core.c lexer_c_cpp.c lexer_intel_asm.c lexer_makefile.c lexer_batch.c user32.lib gdi32.lib comctl32.lib comdlg32.lib /link /SUBSYSTEM:CONSOLE
Debug\core-tests\editor_window_tests.exe
```

These tests cover buffer switching, independent state and undo, raw file saves,
the active filename's border, compact/large window layout, close confirmation,
failed opens, and exit prompts for inactive modified buffers. Test-owned dialogs
are answered automatically.

The standalone tests do not link the GUI or Windows libraries. Assertions must
remain enabled (do not define `NDEBUG`). Test objects use a separate directory so
the command prompt's target architecture cannot overwrite application objects.