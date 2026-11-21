# mega-gopher

A Gopher browser for the MEGA65. Pick a bookmark or type an address,
then move through Gopher menus, read text files, run searches, and
download files and images to a disk image.

## Starting

Mount `GOPHER.D81` and type `RUN "GOPHER"`, or choose Gopher from the
[Net-Tools](https://github.com/silent-command/mega-menu) menu. It needs a
network cable. Going back and downloading use the 8 MB attic RAM.

## Keys

On the start screen, type a number and press RETURN: 1 to type an
address, 2 and up for a bookmark. D and a number deletes a bookmark; Q
or RUN/STOP quits to BASIC.

In a menu:

| Key | Does |
|---|---|
| CRSR up / down | move |
| RETURN | open the item (a menu, a text file, a search, or a download) |
| G | save the item to disk without opening it |
| A | type an address |
| M | bookmark this page, or remove the bookmark |
| RUN/STOP | back |
| CLR/HOME | the first page you opened (press again for the start screen) |
| HELP | the start screen |
| MEGA+F / MEGA+B | change the text / background colour |

Reading a text file:

| Key | Does |
|---|---|
| CRSR up / down | scroll a line |
| RETURN | next page |
| CLR/HOME | top of the file |
| G | save it to disk |
| RUN/STOP | back to the menu |
| HELP | the start screen |

RUN/STOP cancels any prompt.

## Credits

Created by Justin Cameron. Apache License 2.0; see [LICENSE](LICENSE)
and [NOTICE](NOTICE).
