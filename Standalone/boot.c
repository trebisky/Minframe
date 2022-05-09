/*
 * Copyright (c) 1982, 1986 Regents of the University of California.
 * All rights reserved.  The Berkeley software License Agreement
 * specifies the terms and conditions for redistribution.
 *
 *	@(#)boot.c	7.1 (Berkeley) 6/5/86
 */

#include "../h/param.h"
#include "../ufs/inode.h"
#include "../ufs/fs.h"
#include "../h/vm.h"
#include "../machine/a.out.h"
#include "saio.h"
#include "../h/reboot.h"

/*
 * Boot program... arguments passed in r10 and r11 determine
 * whether boot stops to ask for system name and which device
 * boot comes from.
 */

/* Types in r10 specifying major device */
char	devname[][2] = {
	'h','d',	/* 0 = hd */
	'f','d',	/* 1 = fd */
};
#define	MAXTYPE	(sizeof(devname) / sizeof(devname[0]))

#define JUSTASK		/* tjt 3/2/91 */

#define	UNIX	"vmunix"
char line[100];

int	retry = 0;

main()
{
	register unsigned howto, devtype;	/* howto=r11, devtype=r10 */
	int io, i;
	register type, part, unit;
	register char *cp;
	long atol();

#ifdef lint
	howto = 0; devtype = 0;
#endif
	printf("\nStandalone Boot\n");
#ifdef JUSTASK
	howto = RB_ASKNAME|RB_SINGLE;
	devtype = 0;	/* tjt */
#else
	type = (devtype >> B_TYPESHIFT) & B_TYPEMASK;
	unit = (devtype >> B_UNITSHIFT) & B_UNITMASK;
	unit += 8 * ((devtype >> B_ADAPTORSHIFT) & B_ADAPTORMASK);
	part = (devtype >> B_PARTITIONSHIFT) & B_PARTITIONMASK;
	if ((howto & RB_ASKNAME) == 0) {
		if (type >= 0 && type <= MAXTYPE && devname[type][0]) {
			cp = line;
			*cp++ = devname[type][0];
			*cp++ = devname[type][1];
			*cp++ = '(';
			if (unit >= 10)
				*cp++ = unit / 10 + '0';
			*cp++ = unit % 10 + '0';
			*cp++ = ',';
			*cp++ = part + '0';
			*cp++ = ')';
			strcpy(cp, UNIX);
		} else
			howto = RB_SINGLE|RB_ASKNAME;
	}
#endif
	for (;;) {
		if (howto & RB_ASKNAME) {
			printf("Boot : ");	/* tjt */
			gets(line);
		} else
			printf(": %s\n", line);
		io = open(line, 0);
		if (io >= 0) {
			if (howto & RB_ASKNAME) {
				/*
				 * Build up devtype register to pass on to
				 * booted program.
				 */ 
				cp = line;
				for (i = 0; i <= MAXTYPE; i++)
					if ((devname[i][0] == cp[0]) && 
					    (devname[i][1] == cp[1]))
					    	break;
				if (i <= MAXTYPE) {
					devtype = i << B_TYPESHIFT;
					cp += 3;
					i = *cp++ - '0';
					if (*cp >= '0' && *cp <= '9')
						i = i * 10 + *cp++ - '0';
					cp++;
					devtype |= ((i % 8) << B_UNITSHIFT);
					devtype |= ((i / 8) << B_ADAPTORSHIFT);
					devtype |= atol(cp) << B_PARTITIONSHIFT;
				}
			}
			devtype |= B_DEVMAGIC;
			copyunix(howto, devtype, io);
		/*	close(io);	*/
			howto = RB_SINGLE|RB_ASKNAME;
		}
		if (++retry > 2)
			howto = RB_SINGLE|RB_ASKNAME;
	}
}

/*ARGSUSED*/
copyunix(howto, devtype, io)
	register howto, devtype, io;	/* howto=r11, devtype=r10 */
{
	struct exec x;
	register int i;
	char *addr;

	i = read(io, (char *)&x, sizeof x);
	if (i != sizeof x ||
	    (x.a_magic != 0407 && x.a_magic != 0413 && x.a_magic != 0410))
		_stop("Bad format\n");
	printf("%d", x.a_text);
	if (x.a_magic == 0413 && lseek(io, 0x400, 0) == -1)
		goto shread;
	if (read(io, (char *)0, x.a_text) != x.a_text)
		goto shread;
	addr = (char *)x.a_text;
/* XXX  tjt  3/2/91
 * This depends on the value of CLSIZE set in machine/machparam.h
 * no telling if it is set right (also see param.h).
 */
	if (x.a_magic == 0413 || x.a_magic == 0410)
		while ((int)addr & CLOFSET)
			*addr++ = 0;
	printf("+%d", x.a_data);
	if (read(io, addr, x.a_data) != x.a_data)
		goto shread;
	addr += x.a_data;
	printf("+%d", x.a_bss);
	x.a_bss += 128*512;	/* slop */
	for (i = 0; i < x.a_bss; i++)
		*addr++ = 0;
	x.a_entry &= 0x7fffffff;
	printf(" start 0x%x\n", x.a_entry);
	close(io);	/* tjt - turns off floppy motor */
	(*((int (*)()) x.a_entry))();
	return;
shread:
	_stop("Short read\n");
}
