BUILDDIR_win32 = build/win32


all: $(BUILDDIR_win32)/ppwin32.exe

$(BUILDDIR_win32):
	@$(info [MKDIR] $@)
	@mkdir -p $(BUILDDIR_win32)

OBJs = \
        editor_core.obj \
        virtual_buffer.obj \
        lexer_basic.obj \
        lexer_pascal.obj \
        lexer_batch.obj \
        lexer_html.obj \
        lexer_c_cpp.obj \
        lexer_ini.obj \
        lexer_intel_asm.obj \
        lexer_makefile.obj \
        lexer_config_sys.obj

OBJs_win32 = $(addprefix $(BUILDDIR_win32)/,$(OBJs))
OBJs_win32 += \
        $(BUILDDIR_win32)/main_win32.obj


PCRE2 = \
        $(wildcard third_party/pcre2/src/pcre2_*.c)

PCRE2_OBJs = \
        $(patsubst third_party/pcre2/src/%.c,$(BUILDDIR_win32)/%.obj,$(PCRE2))


PCRE_DEFs = \
        -DHAVE_CONFIG_H \
        -DPCRE2_CODE_UNIT_WIDTH=8 \
        -DPCRE2_STATIC \
        -D_CRT_SECURE_NO_WARNINGS

INCLUDES = \
        -Isrc \
        -I$(WATCOM)/h \
        -I$(WATCOM)/h/nt

LIBS = \
        -L$(WATCOM)/lib386 \
        -L$(WATCOM)/lib386/nt \
        gdi32.lib \
        user32.lib \
        comdlg32.lib

RFLAGS_win32 = -q -r -bt=nt -d_WIN32

vpath %.c src/
vpath %.c platform/
vpath %.c third_party/pcre2/src/

$(BUILDDIR_win32)/%.obj: %.c | $(BUILDDIR_win32)
	@$(info OWCC [WIN32] $@)
	@owcc -Wc,-wcd=138 $(PCRE_DEFs) $(INCLUDES) -std=c99 -bwin32 -c $< -o $@

RESOURCE_win32 = $(BUILDDIR_win32)/parapad_win32.res

$(RESOURCE_win32): platform/parapad_win32.rc
	$(info WRC [WIN32] $@)
	@wrc $< $(RFLAGS_win32) -i=src/ -fo=$@

$(BUILDDIR_win32)/ppwin32.exe: $(OBJs_win32) $(PCRE2_OBJs) $(RESOURCE_win32)
	@$(info OWCC [WIN32] $@)
	@owcc -bwin32 $(OBJs_win32) $(PCRE2_OBJs) $(LIBS) -o$@
	@$(info WRC [WIN32] $@)
	@wrc -q $(RESOURCE_win32) $@

