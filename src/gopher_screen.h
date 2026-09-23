/* 80-column screen setup for the gopher client. See REQUIREMENTS.md
 * section 7 for the full story on why each of these steps exists --
 * skipping any of them causes either screen corruption (bad screen RAM
 * placement), a visual 40-column wrap despite being in 80-column pixel
 * mode (missing row-width register pokes), or interleaved garbled text
 * (mixing stdio printf()/puts() with conio cputc()/cputs()).
 *
 * RULE: once gopher_screen_init() has run, never call stdio's printf()
 * or puts() again. For dynamic text, sprintf() into a local buffer then
 * display with cprintf() (cast to const unsigned char*) -- cputs()
 * doesn't interpret '\n', only cprintf()/_cprintf() does. */
#ifndef GOPHER_SCREEN_H
#define GOPHER_SCREEN_H

void gopher_screen_init(void);

#endif

/* Literal-safe text output.
 *
 * CC65's charmap rewrites C string literals into screen codes at compile
 * time, so under CC65 a literal can go straight to cputs(). llvm-mos does
 * no such rewriting -- literals stay plain ASCII and render as garbage for
 * lowercase (uppercase happens to survive, which makes this easy to miss).
 * These wrappers translate at runtime where the toolchain doesn't, so
 * callers can pass ordinary C strings in either build.
 *
 * Network-sourced text is raw ASCII in BOTH builds and is translated by
 * its own caller; that asymmetry between literals and network data is the
 * root of R-5, R-6 and the title-bar bug. */
void gopher_puts(const char *s);
void gopher_putsxy(unsigned char x, unsigned char y, const char *s);
char gopher_ascii_to_screencode(char c);

/* Colour handling: border and background are inherited from whatever the
 * user had set before running (the program never writes $d020/$d021), and
 * the text colour is taken from the ROM's current-colour byte at $0286
 * rather than forced to white. */
unsigned char gopher_screen_text_colour(void);
void gopher_screen_cycle_text_colour(void);

/* Advances background and border together, keeping them identical. */
void gopher_screen_cycle_background(void);

/* Folds UTF-8 into one displayable byte per character (see the long note
 * in gopher_screen.c). dst must hold dstmax+1 bytes. */
unsigned char gopher_fold_utf8(char *dst, const char *src, unsigned char dstmax);
