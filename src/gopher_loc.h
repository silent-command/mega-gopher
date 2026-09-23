/* struct location, shared.
 *
 * This header exists because it was NOT shared, and that caused a real
 * bug. gopher_config.c used to carry its own copy of the definition with
 * a comment saying it mirrored "just what we need rather than moving the
 * definition, which would churn the whole client". The copy went stale:
 * LOC_SELECTOR_LEN grew to 80 in gopher.c (to hold selector + TAB + query
 * for type-7 searches) while the mirror stayed at 47.
 *
 * The two structs then disagreed by 33 bytes, so `port` sat at a different
 * offset in each. Saving a bookmark read the port from inside the selector
 * array and wrote 0; loading one would have written the port back into the
 * middle of the selector. Nothing warned, because each file compiled
 * against a self-consistent definition.
 *
 * A duplicated type definition is a latent bug with a delay fuse. Keep it
 * in one place.
 */
#ifndef GOPHER_LOC_H
#define GOPHER_LOC_H

#define LOC_HOST_LEN 31

/* Holds selector + TAB + query for a type-7 search, not just a selector,
 * which is why this is larger than ITEM_SELECTOR_LEN. Each of the
 * MAX_HISTORY saved locations carries one, so raising it costs
 * MAX_HISTORY times the increase -- see the sizing note in
 * REQUIREMENTS.md before growing it further. */
#define LOC_SELECTOR_LEN 80

struct location {
  char host[LOC_HOST_LEN + 1];
  char selector[LOC_SELECTOR_LEN + 1];
  unsigned int port;
};

#endif
