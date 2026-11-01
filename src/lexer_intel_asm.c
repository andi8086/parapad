#include "lexer_intel_asm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char integer_instructions[] =
    "aaa aad aam aas adc add and arpl bound bsf bsr bswap bt btc btr bts "
    "call cbw cdq clc cld cli clts cmc cmp cmps cmpsb cmpsw cmpsd "
    "cmpxchg cmpxchg8b cpuid cwd cwde daa das dec div enter enterd enterw esc "
    "hlt idiv imul in inc ins insb insw insd int int1 int3 into invd invlpg "
    "iret iretd iretw jcxz jecxz jmp lahf lar lds lea leave leaved leavew "
    "les lfs lgdt lgs lidt lldt lmsw lods lodsb lodsw lodsd loop loopd loopw "
    "loope looped loopew loopne loopned loopnew loopnz loopnzd loopnzw "
    "loopz loopzd loopzw lsl lss ltr mov movs movsb movsw movsd movsx movzx "
    "mul neg nop not or out outs outsb outsw outsd pop popa popad popaw "
    "popf popfd popfw push pusha pushad pushaw pushf pushfd pushfw "
    "rcl rcr rdmsr rdpmc rdtsc ret retd retw retf retfd retfw retn retnd retnw "
    "rol ror rsm sahf sal sar sbb scas scasb scasw scasd sgdt shl shld shr "
    "shrd sidt sldt smsw stc std sti stos stosb stosw stosd str sub test "
    "verr verw wait wbinvd wrmsr xadd xchg xlat xlatb xor";

static const char x87_instructions[] =
    "f2xm1 fabs fadd faddp fbld fbstp fchs fclex fcom fcomp fcompp fcos "
    "fdecstp fdisi fdiv fdivp fdivr fdivrp feni ffree fiadd ficom ficomp "
    "fidiv fidivr fild fimul fincstp finit fist fistp fisub fisubr fld fld1 "
    "fldcw fldenv fldenvw fldenvd fldl2e fldl2t fldlg2 fldln2 fldpi fldz "
    "fmul fmulp fnclex fndisi fneni fninit fnop fnsave fnsavew fnsaved "
    "fnstcw fnstenv fnstenvw fnstenvd fnstsw fpatan fprem fprem1 fptan "
    "frndint frstor frstorw frstord fsave fsavew fsaved fscale fsetpm fsin "
    "fsincos fsqrt fst fstcw fstenv fstenvw fstenvd fstp fstsw fsub fsubp "
    "fsubr fsubrp ftst fucom fucomp fucompp fwait fxam fxch fxtract fyl2x fyl2xp1";

static const char mmx_instructions[] =
    "emms movd movq packsswb packssdw packuswb paddb paddw paddd paddsb "
    "paddsw paddusb paddusw pand pandn pcmpeqb pcmpeqw pcmpeqd pcmpgtb "
    "pcmpgtw pcmpgtd pmaddwd pmulhw pmullw por psllw pslld psllq psraw "
    "psrad psrlw psrld psrlq psubb psubw psubd psubsb psubsw psubusb "
    "psubusw punpckhbw punpckhwd punpckhdq punpcklbw punpcklwd punpckldq pxor";

static const char legacy_instructions[] =
    "brkpt cmpxchg486 ffreep frstpm ibts icebp int01 int03 loadall loadall286 "
    "salc setalc smi ud0 ud1 ud2 ud2a ud2b umov xbts";

static const char condition_codes[] =
    "a ae b be c e g ge l le na nae nb nbe nc ne ng nge nl nle no np ns "
    "nz o p pe po s z";

static const char prefixes[] =
    "a16 a32 addr16 addr32 lock o16 o32 data16 data32 rep repe repz repne repnz";

static const char registers[] =
    "al ah bl bh cl ch dl dh ax bx cx dx si di bp sp "
    "eax ebx ecx edx esi edi ebp esp cs ds es fs gs ss ip eip flags eflags "
    "cr0 cr2 cr3 cr4 dr0 dr1 dr2 dr3 dr4 dr5 dr6 dr7 "
    "tr3 tr4 tr5 tr6 tr7 st st0 st1 st2 st3 st4 st5 st6 st7 "
    "mm0 mm1 mm2 mm3 mm4 mm5 mm6 mm7";

static const char qualifiers[] =
    "byte word dword fword qword tword tbyte near far short ptr offset seg "
    "strict to dup length lengthof size sizeof type high low highword lowword "
    "this mod shl shr eq ne lt le gt ge";

static const char directives[] =
    ".8086 .8087 .186 .286 .286p .287 .386 .386p .387 .486 .486p .586 .586p "
    ".mmx .code .data .data? .const .stack .model .startup .exit .radix "
    ".if .elseif .else .endif .while .endw .repeat .until .untilcxz .break .continue "
    "align alignb assume absolute bits cpu comment common db dw dd df dq dt "
    "resb resw resd resq rest equ org section segment ends group global globl "
    "public extern extrn externdef export import library format use16 use32 "
    "proc endp proto invoke local label end entry include includelib incbin "
    "macro endm exitm rept repeat irp irpc purge if ifdef ifndef ife ifb ifnb "
    "ifidn ifdif elseif else endif option name title subtitle page radix "
    "times struc struct endstruc union record istruc iend at instance";

static unsigned char lower_ascii(unsigned char character)
{
    return character >= 'A' && character <= 'Z'
        ? (unsigned char)(character + ('a' - 'A')) : character;
}

static int equals_word(const char *text, size_t length, const char *word, size_t word_length)
{
    if (length != word_length)
        return 0;
    for (size_t index = 0; index < length; ++index) {
        if (lower_ascii((unsigned char)text[index]) != (unsigned char)word[index])
            return 0;
    }
    return 1;
}

static int in_words(const char *text, size_t length, const char *words)
{
    while (*words) {
        const char *end = strchr(words, ' ');
        size_t word_length = end ? (size_t)(end - words) : strlen(words);
        if (equals_word(text, length, words, word_length))
            return 1;
        words += word_length;
        if (*words)
            ++words;
    }
    return 0;
}

static int identifier_start(unsigned char character)
{
    character = lower_ascii(character);
    return (character >= 'a' && character <= 'z') || character == '_' ||
        character == '.' || character == '$' || character == '?' || character == '@';
}

static int digit(unsigned char character)
{
    return character >= '0' && character <= '9';
}

static int identifier_part(unsigned char character)
{
    return identifier_start(character) || digit(character);
}

static int number(const char *text, size_t length)
{
    size_t start = 0;
    size_t end = length;
    unsigned int base = 10;
    if (!length || !digit((unsigned char)text[0]))
        return 0;
    if (length > 2 && text[0] == '0') {
        unsigned char prefix = lower_ascii((unsigned char)text[1]);
        if (prefix == 'x' || prefix == 'h') base = 16;
        else if (prefix == 'b' || prefix == 'y') base = 2;
        else if (prefix == 'o' || prefix == 'q') base = 8;
        else if (prefix == 'd' || prefix == 't') base = 10;
        else prefix = 0;
        if (prefix)
            start = 2;
    }
    if (!start && length > 1) {
        unsigned char suffix = lower_ascii((unsigned char)text[length - 1]);
        if (suffix == 'h') base = 16;
        else if (suffix == 'b' || suffix == 'y') base = 2;
        else if (suffix == 'o' || suffix == 'q') base = 8;
        else if (suffix == 'd' || suffix == 't') base = 10;
        else suffix = 0;
        if (suffix)
            --end;
    }
    size_t digits = 0;
    for (size_t index = start; index < end; ++index) {
        unsigned char character = lower_ascii((unsigned char)text[index]);
        if (character == '_')
            continue;
        unsigned int value = digit(character) ? (unsigned int)(character - '0') :
            character >= 'a' && character <= 'f' ? (unsigned int)(character - 'a' + 10) : 16;
        if (value >= base)
            break;
        ++digits;
        if (index + 1 == end)
            return digits != 0;
    }
    if (start || end != length)
        return 0;
    size_t index = 0;
    while (index < length && digit((unsigned char)text[index]))
        ++index;
    int floating = 0;
    if (index < length && text[index] == '.') {
        floating = 1;
        ++index;
        while (index < length && digit((unsigned char)text[index]))
            ++index;
    }
    if (index < length && lower_ascii((unsigned char)text[index]) == 'e') {
        floating = 1;
        ++index;
        if (index < length && (text[index] == '+' || text[index] == '-'))
            ++index;
        size_t exponent = index;
        while (index < length && digit((unsigned char)text[index]))
            ++index;
        if (index == exponent)
            return 0;
    }
    return floating && index == length;
}

static int instruction(const char *text, size_t length)
{
    if (length > 1 && lower_ascii((unsigned char)text[0]) == 'j' &&
        in_words(text + 1, length - 1, condition_codes))
        return 1;
    if (length > 3 && equals_word(text, 3, "set", 3) &&
        in_words(text + 3, length - 3, condition_codes))
        return 1;
    return in_words(text, length, integer_instructions) ||
        in_words(text, length, x87_instructions) ||
        in_words(text, length, mmx_instructions) ||
        in_words(text, length, legacy_instructions) || in_words(text, length, prefixes);
}

static int supports_extension(const char *extension)
{
    return extension && in_words(extension, strlen(extension), ".asm .inc .s");
}

static void free_line(LexerLine *line)
{
    free(line->tokens);
    free(line->end_state);
    memset(line, 0, sizeof(*line));
}

static int append_token(LexerLine *line, size_t *capacity, size_t start,
    size_t length, LexerTokenKind kind)
{
    if (!length)
        return 1;
    if (line->count && line->tokens[line->count - 1].kind == kind) {
        line->tokens[line->count - 1].length += length;
        return 1;
    }
    if (line->count == *capacity) {
        size_t maximum = SIZE_MAX / sizeof(*line->tokens);
        if (*capacity > maximum / 2)
            return 0;
        size_t next = *capacity ? *capacity * 2 : 16;
        LexerToken *tokens = realloc(line->tokens, next * sizeof(*tokens));
        if (!tokens)
            return 0;
        line->tokens = tokens;
        *capacity = next;
    }
    LexerToken *token = &line->tokens[line->count++];
    token->start = start;
    token->length = length;
    token->kind = kind;
    return 1;
}

static int lex_line(const char *text, size_t length, const void *state, LexerLine *result)
{
    if (!result || (!text && length))
        return 0;
    LexerLine line = { 0 };
    size_t capacity = 0;
    size_t position = 0;
    int first_word = 1;
    unsigned char delimiter = state ? *(const unsigned char *)state : 0;
    while (position < length) {
        size_t start = position;
        LexerTokenKind kind = LEXER_TEXT;
        unsigned char character = (unsigned char)text[position];
        if (delimiter) {
            kind = LEXER_COMMENT;
            while (position < length && (unsigned char)text[position] != delimiter)
                ++position;
            if (position < length) {
                ++position;
                delimiter = 0;
            }
        } else if (character == ';') {
            kind = LEXER_COMMENT;
            position = length;
        } else if (character == '\'' || character == '"' || character == '`') {
            kind = LEXER_STRING;
            ++position;
            while (position < length) {
                if (character == '`' && text[position] == '\\' && position + 1 < length) {
                    position += 2;
                } else if ((unsigned char)text[position++] == character) {
                    if (position < length && (unsigned char)text[position] == character)
                        ++position;
                    else
                        break;
                }
            }
            first_word = 0;
        } else if (identifier_part(character) || character == '%') {
            ++position;
            while (position < length && identifier_part((unsigned char)text[position]))
                ++position;
            if (digit(character)) {
                if (position < length && (text[position] == '+' || text[position] == '-') &&
                    lower_ascii((unsigned char)text[position - 1]) == 'e') {
                    ++position;
                    while (position < length && digit((unsigned char)text[position]))
                        ++position;
                }
                if (number(text + start, position - start))
                    kind = LEXER_NUMBER;
            } else {
                size_t following = position;
                while (following < length && (text[following] == ' ' || text[following] == '\t'))
                    ++following;
                int label = following < length && text[following] == ':';
                if (!label) {
                    if (character == '%' || in_words(text + start, position - start, directives))
                        kind = LEXER_PREPROCESSOR;
                    else if (instruction(text + start, position - start) ||
                        in_words(text + start, position - start, registers) ||
                        in_words(text + start, position - start, qualifiers))
                        kind = LEXER_KEYWORD;
                    if (first_word && equals_word(text + start, position - start, "comment", 7) &&
                        following < length && (unsigned char)text[following] > ' ') {
                        delimiter = (unsigned char)text[following];
                        position = following + 1;
                        kind = LEXER_COMMENT;
                    }
                }
                first_word = label;
            }
        } else {
            ++position;
        }
        if (!append_token(&line, &capacity, start, position - start, kind)) {
            free_line(&line);
            return 0;
        }
    }
    if (delimiter) {
        unsigned char *ending = malloc(sizeof(*ending));
        if (!ending) {
            free_line(&line);
            return 0;
        }
        *ending = delimiter;
        line.end_state = ending;
    }
    *result = line;
    return 1;
}

static int states_equal(const void *left, const void *right)
{
    unsigned char left_delimiter = left ? *(const unsigned char *)left : 0;
    unsigned char right_delimiter = right ? *(const unsigned char *)right : 0;
    return left_delimiter == right_delimiter;
}

const Lexer lexer_intel_asm = {
    "Intel Assembly (Pentium MMX)", supports_extension, lex_line, states_equal, free_line, NULL
};