# PCRE2 10.47

Source: https://github.com/PCRE2Project/pcre2/tree/pcre2-10.47

The interpreter C sources and headers are unmodified upstream files from the
`pcre2-10.47` tag. `pcre2.h.generic` is installed as `src/pcre2.h`, and
`pcre2_chartables.c.dist` as `src/pcre2_chartables.c`. Upstream licensing and
manual-build instructions are retained in `LICENCE.md` and `NON-AUTOTOOLS-BUILD`.

`src/config.h` is Parapad's configuration wrapper around upstream
`config.h.generic`. Only the static 8-bit interpreter is enabled; JIT, Unicode,
POSIX wrappers, utilities, and upstream tests are not built. No Windows-specific
regex APIs or runtime regex DLLs are required. Actual Windows 95 deployment
still requires a compatible compiler/CRT and native validation.

The Visual Studio project compiles the vendored `pcre2_*.c` files with
`HAVE_CONFIG_H`, `PCRE2_CODE_UNIT_WIDTH=8`, and `PCRE2_STATIC` defined. Include
`pcre2_jit_compile.c` even without JIT: it supplies the public API stubs.
When upgrading, review the upstream source manifest and configuration defaults,
update both this version record and the application documentation, and retain
the upstream license.