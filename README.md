# Parapad

Build `parapad.sln` in Visual Studio with the v142 C++ toolset and Windows SDK.
The application starts with an empty text buffer in command mode. Press `i` to
enter insert mode and Esc to return to command mode. The main window's status bar
shows `COMMAND` or `INSERT` directly from the core's state.

In insert mode, typing inserts text, Enter splits the line, Backspace deletes the
previous character, and Tab inserts four spaces. In command mode, `h/j/k/l` move
left/down/up/right, `x` deletes the current character, `0` moves to the start of
the line, and `$` moves to its end. Other character keys are ignored. Arrow keys,
Home, End, Delete, and mouse caret positioning work in either mode. The viewport
follows the caret. This is a small Vim-style command subset, not a full Vim engine.

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
| `EDITOR_GET_MODE` | `EditorMode *` receiving `EDITOR_MODE_COMMAND` or `EDITOR_MODE_INSERT` |
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
Mode transitions are synchronous and can be queried with `EDITOR_GET_MODE`.
The GUI sends key messages without interpreting commands or deciding transitions.
Direct buffer-edit and navigation messages remain mode-independent for loading
text, programmatic edits, and other non-keyboard clients.

All stored text and string payloads are UTF-8 `char` strings, independent of the
system locale. Line and column indices are zero-based. Columns and lengths count
bytes; navigation and deletion operate on complete Unicode scalars, never partial
UTF-8 sequences. Setting a column inside a sequence clamps to its start.
Malformed UTF-8, surrogate values, and values above U+10FFFF are rejected without
changing the document. Embedded NUL is not supported. Combining sequences are not
treated as single grapheme clusters.
CRLF and CR are normalized to LF during insertion. Empty documents have one
empty line, and a trailing LF creates a final empty line. Deleting a line removes
its following newline, or the preceding newline when deleting the final line.
Backspace and Delete join adjacent lines when removing a newline.

`EDITOR_GET_LINE` returns a borrowed slice, not a NUL-terminated line. Use its
length, do not modify it, and query it again after any text mutation. The pointer
is invalid after a mutation or destruction. Dispatch is synchronous and instances
must not be accessed concurrently without external synchronization.

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