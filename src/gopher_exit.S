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
        lda #0
        sta $d6e0               ; ethernet controller: held in reset
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
2:      ldx #0                  ; once more in the stub, last thing before the jump
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
        jmp ($fffc)             ; the KERNAL's reset entry
stub_end:
