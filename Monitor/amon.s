|
| Assembly language startup file for the
| Miniframe boot rom monitor.
| This code is added to the standard Miniframe boot rom's
| at a link-time defined address (see the makefile).
|
| T. Trebisky  8/28/89
|
| ROM_BASE will never change, for this machine.
| The next numbers are crucial - must match values used elsewhere
|   ROM_OFFSET must match the way the roms were patched/burned.
|   ROM_COUNT could be defined in terms of ROM_OFFSET, but we just
|    grind it out by hand and stick it here, it is the count of
|    4 byte pieces (minus 1 for the dbra) that get moved.
|    For the pair of 2764's we are using this is:
|    (0x4000-ROM_OFFSET)/4 - 1

ROM_BASE = 0x800000

ROM_OFFSET = 0x1000
ROM_COUNT = 0xbff

VEC_BASE = 0x40000	| put a table of vectors here

GCRADDR = 0x450000	| includes LED's
LEDON  =  0x3000	| all 4 LED are on
LEDOF  =  0x3f00	| all 4 LED are off

|  This code first fires up in ROM at the entry point below
|    (at address ROM_BASE+ROM_OFFSET), it makes a copy of itself
|    in RAM (at whatever address it is linked to run at) which it
|    then jumps into to run.  The code must be position independent
|    up to the point where it jumps into the RAM copy.

| This first entry point is entered only on system reset

	.text
	.globl	mon_ent
mon_ent:	
	clrw	sp@-		| leave space for phony vector

	movw	#0x3700,sp@-	| fake sr for testing	

	moveml	d0-d7/a0-a7,sp@-


	jmp	rament		| into the RAM copy

rament:
	bsr	hinit		| hardware initialization hook
	pea	sp@
	bsr	_cmon		| enter C initialization
	addql	#4,sp
	moveml	sp@+,d0-d7/a0-a7
	rte			| C fixes stack so this works

utrap:	
	moveml	d0-d7/a0-a7,sp@-
	pea	sp@
	bsr	_cutrap		| enter C handler
	addql	#4,sp
	moveml	sp@+,d0-d7/a0-a7
	rte

ttrap:	
	moveml	d0-d7/a0-a7,sp@-
	pea	sp@
	bsr	_cttrap		| enter C handler
	addql	#4,sp
	moveml	sp@+,d0-d7/a0-a7
	rte

| hardware initialization:

| set up vector table and interrupt handlers

hinit:


	rts		| end of hinit

| little delay loop
| remember, dbra works on a word (16 bits) only

DCOUNT1 =  1000		| delay count
DCOUNT2 =  2000		| delay count

delay:
	movl	#DCOUNT1,d2
ld1:	movl	#DCOUNT2,d1
ld2:	dbra	d1,ld2
	dbra	d2,ld1
	rts
