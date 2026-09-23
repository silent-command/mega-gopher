#ifndef GOPHER_BOOT_H
#define GOPHER_BOOT_H

/* Loads mega-net's image and the far-call trampoline from the boot disk
 * into the fixed addresses they run at, so the program can be run on
 * its own. Returns 1 on success, 0 on failure (with *err pointing at a
 * short reason). Must be called before any meganet_call(). */
unsigned char gopher_boot_load(const char **err);

/* F011 drive number (0 or 1, i.e. device 8 or 9) that ETHBIN was loaded
 * from. Only meaningful after a successful gopher_boot_load(). */
extern unsigned char boot_drive;

#endif
