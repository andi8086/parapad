all: ppwin32.exe


OBJs = \
        main.obj \
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

PCRE2 = \
        $(wildcard third_party/pcre2/src/pcre2_*.c)

SRCs = $(patsubst %.obj,%.c,$(OBJs))


PCRE_DEFs = \
        -DHAVE_CONFIG_H \
        -DPCRE2_CODE_UNIT_WIDTH=8 \
        -DPCRE2_STATIC \
        -D_CRT_SECURE_NO_WARNINGS

INCLUDES = \
        -I$(WATCOM)/h \
        -I$(WATCOM)/h/nt \
        -L$(WATCOM)/lib386 \
        -L$(WATCOM)/lib386/nt

LIBS = \
        gdi32.lib \
        user32.lib \
        comdlg32.lib

ppwin32.exe: $(SRCs) $(PCRE2)
	owcc -Wc,-wcd=138 $(PCRE_DEFs) $(INCLUDES) -std=c99 -bwin32 $^ $(LIBS) -o$@

