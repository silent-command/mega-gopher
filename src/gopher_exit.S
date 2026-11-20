/* Copyright 2026 Justin Cameron
 * SPDX-License-Identifier: Apache-2.0 */
; Leaving for BASIC. The C version of this (mega-ftp's m65_exit.c) works
; there, but linking it into this program crashed start-up: with the
; function merely referenced, the network bring-up jumped into zero page
; (PC at $00F0, seen on the serial monitor), while the same program with
; the reference removed booted. Nothing in the function runs at boot, so
; that is link-time code generation reshaping the rest of the program,
; the family of fault REQUIREMENTS.md 2.34 records. Assembly is outside
; LTO's reach.
;
; What it does: hold the 45E100 in reset, wait half a second (its events reach the IRQ
; vector past the enables, mega-net 5.16, and nothing will service them
; now), drain the keyboard queue, copy the stub to $1FB0 (spare low RAM,
; gopher_scratch.h) and run it from there: clear the MAP, restore $01 and
; $D030 to what the runtime's .fini uses, and jump through the KERNAL
; reset vector. The ROM re-initialises the screen editor, the DOS and
; BASIC's zero page as at power-on; Hyppo does not run again, so the disk
; mounted from BASIC stays mounted. It must run from outside $2000-$BFFF:
; mapping the BASIC ROM back over that window hides this program's own
; code (mega-ftp REQUIREMENTS.md 5.7).

        .text
        .global gopher_exit_to_basic
gopher_exit_to_basic:
        ; The ethernet controller: held in reset, unless the collection's
        ; handoff block ("MNC" at $FF87FF0) says its dispatcher runs next.
        ; Then it is left running, link and all, for the next program's
        ; INIT_WARM (mega-net 5.23). $FB-$FE as below, under SEI.
        php
        sei
        lda #$f0
        sta $fb
        lda #$7f
        sta $fc
        lda #$f8
        sta $fd
        lda #$0f
        sta $fe                 ; $fb-$fe -> $0FF87FF0
        .byte $a3, $00          ; ldz #0
        .byte $ea, $b2, $fb     ; lda [$fb],z
        cmp #77
        bne 8f
        .byte $1b               ; inz
        .byte $ea, $b2, $fb
        cmp #78
        bne 8f
        .byte $1b
        .byte $ea, $b2, $fb
        cmp #67
        beq 9f
8:      lda #0
        sta $d6e0               ; held in reset
9:      .byte $a3, $00          ; ldz #0
        plp
        ; Settle for half a second, interrupts still on: any event the
        ; controller had in flight is taken by the program's own handler,
        ; and the key that asked for this is released. The ROM enters its
        ; machine-language monitor when RUN/STOP is held at reset, which is
        ; exactly what a reset a few milliseconds after the keypress saw.
        ldy #25
4:      lda $d7fa
5:      cmp $d7fa
        beq 5b
        dey
        bne 4b
        sei
1:      lda $d610
        beq 2f
        sta $d610               ; pop one key
        jmp 1b
        ; The font: conioinit() pointed the character set at the ROM's copy
        ; in bank 2 ($D06A = 2) and turned the VIC-IV hot registers off, so
        ; the ROM's reset no longer recomputes that pointer and BASIC came
        ; back typing into an unreadable screen (mega-ftp 5.9). Bank 0
        ; again, hot registers on.
        lda $d05d
        ora #$80
        sta $d05d               ; hot registers first: turning them on can
        lda #0
        sta $d068               ; the ROM's lowercase set again, not the copy at $11000 (ssh 5.25)
        lda #$d8
        sta $d069
        lda #0                  ; re-derive the font pointer from the classic
        sta $d06a               ; registers, so the bank goes to 0 after, and
2:      ; Black between programs (the Net-Tools collection, mega-menu/README.md;
        ; the C of this is black_until_dispatch in mega-ftp's m65_exit.c).
        ; The ROM's reset paints colour 6 and reloads the palette into the
        ; bank $D070 selects for writing, leaving $D070 alone. So colour 6
        ; is made black in bank 3, the one BASIC draws from, and the
        ; ROM's writes are sent to bank 0, until the collection's dispatcher
        ; puts both back. Only when the handoff block ($FF87FF0 = "MNC")
        ; is there.
        lda #$f0
        sta $fb
        lda #$7f
        sta $fc
        lda #$f8
        sta $fd
        lda #$0f
        sta $fe                 ; $fb-$fe -> $0FF87FF0
        .byte $a3, $00          ; ldz #0
        .byte $ea, $b2, $fb     ; lda [$fb],z
        cmp #77
        bne 7f
        .byte $1b               ; inz
        .byte $ea, $b2, $fb
        cmp #78
        bne 7f
        .byte $1b
        .byte $ea, $b2, $fb
        cmp #67
        bne 7f
        lda #$ff
        sta $d070               ; show and write bank 3
        lda #0
        sta $d106
        sta $d206
        sta $d306
        lda #$3f
        sta $d070               ; the ROM's writes: bank 0
7:      ldx #0                  ; the font's bank goes once more in the stub, last thing before the jump
3:      lda stub,x
        sta $1fb0,x
        inx
        cpx #(stub_end-stub)
        bne 3b
        jmp $1fb0

stub:
        sei
        lda #0
        tax
        tay
        .byte $4b               ; taz
        .byte $5c               ; map: none, anywhere
        .byte $ea               ; eom
        lda #$3f
        sta $01
        lda #$64
        sta $d030               ; ROM at $C000 and $E000, as .fini
        lda #0
        sta $d06a               ; the font's bank, once more, after every other write
        ; That reset uses the IEC serial bus whenever a disk image is
        ; mounted, and here one always is. A drive on the IEC port that is
        ; switched off holds the bus lines low, and the ROM waits forever:
        ; the banner, a blinking drive light, no READY. Switch the drive on
        ; or unplug it (mega-net PLATFORM.md, trap 17).
        jmp ($fffc)             ; the KERNAL's reset entry
stub_end:
