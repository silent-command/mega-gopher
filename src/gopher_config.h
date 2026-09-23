/* Persistent settings, stored as GOPHER.CFG on the .d81 the program runs
 * from (FR-12).
 *
 * The .d81 is both the runtime medium and the distribution medium, so a
 * config file there travels with the program and survives reboots without
 * needing SD-card writes -- it is written through the F011/CBM DOS layer in
 * gopher_cbmdos.c, which is the only disk path on this machine that does
 * not disturb $d030 or the interrupt state.
 *
 * Note this means a bookmark saved before sharing the image becomes the
 * recipient's start page too. */
#ifndef GOPHER_CONFIG_H
#define GOPHER_CONFIG_H

struct location; /* defined in gopher.c */

/* How many bookmarks a config file holds. Twelve is what the start
 * chooser can list and still leave room for its three fixed options, the
 * prompt and the input line on a 25-row screen. */
#define CFG_MAX_BOOKMARKS 12

/* Returned by config_load when there is no config file at all, as opposed
 * to a config file that lists no bookmarks. The caller needs to tell those
 * apart: the first means "never configured", and the built-in start sites
 * are seeded; the second means the user deleted them, which must be
 * respected. A file that exists but is not ours reads as 0, not this, so
 * defaults are never seeded over somebody else's file. */
#define CFG_NO_FILE 0xff

/* Reads up to `max` bookmarks into `list`. Returns how many were read, or
 * CFG_NO_FILE if no config file could be read. Files written by the
 * earlier single-start-page format are still read. */
unsigned char config_load(struct location *list, unsigned char max,
                          unsigned char drive);

/* Writes `count` bookmarks. A count of 0 still writes the file, holding
 * just the magic line: that is what records "the user cleared the list"
 * rather than "this disk was never configured". Returns 1 on success. */
unsigned char config_save(const struct location *list, unsigned char count,
                          unsigned char drive);

#endif
