/* hard.c
	winchester formatter and diagnostic for Miniframe.
	tjt  6/4/90	- wrote floppy.c
	tjt  12/22/90	- converted floppy.c to hard.c
	tjt  1/8/91	- Version 1.0 operational and useful
	tjt  2/2/91	- Version 1.1 add spiral skewing
	tjt  2/19/91	- Version 1.2 add disk parameter table
	tjt  2/24/91	- Version 1.3 split out menus, bad block handling
	tjt  2/26/91	- Version 1.4 write boot image to disk
*/

#ifdef NEVER
/* these options are NOT on. */
#define DEBUG
#else
/* these options ARE on. */
#endif

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned long u_long;
#include "dkbad.h"
#include "volhdr.h"

/* Disk DMA controller registers */
#define DISK_DMA_COUNT		(short *) 0xc80000	/* r/w word count */
#define DISK_DMA_LADDR		(short *) 0xc80002	/* wo  low 16 bits */
#define DISK_DMA_UADDR_W	(short *) 0xc80006	/* wo upper 5 bits */
#define DISK_DMA_UADDR_R	(short *) 0xc80008	/* wo upper 5 bits */

/* Now here is a strange place to hide a bit.
 * In the slow communications status register, we have a bit
 * that goes low whenever a floppy drive is physically connected
 * to the the 34 pin connector, whether it is powered up, ready, or whatever.
 */
#define SC_STAT			(short *) 0xc30008
#define SC_FDNP			0x01

/* Disk Bus Interface Unit registers */
#define FD_RESET_ON		(short *) 0xc60020
#define FD_RESET_OFF		(short *) 0xc60022
#define HD_RESET_ON		(short *) 0xc60024
#define HD_RESET_OFF		(short *) 0xc60026
#define FD_MOTOR_ON		(short *) 0xc60028
#define FD_MOTOR_OFF		(short *) 0xc6002A
#define HD_DMA_ENABLE		(short *) 0xc6002C
#define FD_DMA_ENABLE		(short *) 0xc6002E
#define DISK_DMA_DISABLE	(short *) 0xc60030
#define FD_SINGLE		(short *) 0xc60032
#define FD_DOUBLE		(short *) 0xc60034
#define DISK_BIU_RESET		(short *) 0xc60036

/* the WD1010 HDC chip */
#define HD_EFLAGS	(short *) 0xc60002
#define HD_WPC		(short *) 0xc60002
#define HD_SCOUNT	(short *) 0xc60004
#define HD_SNUM		(short *) 0xc60006
#define HD_CYLOW	(short *) 0xc60008
#define HD_CYHIGH	(short *) 0xc6000A
#define HD_SDH		(short *) 0xc6000C
#define HD_STATUS	(short *) 0xc6000E
#define HD_COMMAND	(short *) 0xc6000E

/* here are the 6 WD1010 comands */
#define HDC_RESTORE	0x10
#define HDC_SEEK	0x70
#define HDC_READ	0x20
#define HDC_WRITE	0x30
#define HDC_SCANID	0x40
#define HDC_FORMAT	0x50

#define HDC_IEOC	0x08	/* interrupt at end of cmd */
#define HDC_MULT	0x04	/* multi-sector read/write */

/* here are bits in the WD1010 status register */
#define HDS_BUSY	0x80
#define HDS_DRDY	0x40
#define HDS_WF		0x20
#define HDS_SC		0x10	/* seek complete */
#define HDS_DRQ		0x08
#define HDS_CIP		0x02	/* command in progress */
#define HDS_ERR		0x01

#define HDS_MASK	(HDS_BUSY|HDS_WF|HDS_CIP|HDS_ERR)	/* 0xA3 */

/* here are bits in the WD1010 error register */
#define HDE_BBLK	0x80
#define HDE_CRC		0x40
#define HDE_IDNF	0x10
#define HDE_ABORT	0x04
#define HDE_TK0		0x02
#define HDE_DAM		0x01

/* the WD2797 FDC chip */
#define FD_COMMAND	(short *) 0xc60010
#define FD_STATUS	(short *) 0xc60010
#define FD_TRACK	(short *) 0xc60012
#define FD_SECTOR	(short *) 0xc60014
#define FD_DATA		(short *) 0xc60016

/* bits in the WD2797 status register */
#define FD_NREADY	0x80
#define FD_WPROT	0x40
#define FD_HLOADED	0x20
#define FD_SEEKERR	0x10
#define FD_CRCERR	0x08
#define FD_TRACK0	0x04
#define FD_INDEX	0x02
#define FD_BUSY		0x01
/* these bits change for type II and III commands */
#define FD_RTYPE	0x20		/* on read, 1 = deleted data */
#define FD_RNF		0x10		/* record not found */
#define FD_LDATA	0x04		/* lost data */
#define FD_DRQ		0x02

/* the 8259 interrupt controller chip */
#define PIC_A0		(short *) 0xc90000
#define PIC_A1		(short *) 0xc90002

/* here is the way we initialize the PIC */
#define ICW1	0x13
#define ICW2	0x0
#define ICW4	0x3
#define OCW1	0xff

/* Here is the deal on the Miniframe interrupt setup - 
	Level 7 (NMI) autovector: parity, page-fault, mmuerr, mnp
	Level 6 Timer autovector
	Level 5 8274 autovector
	Level 4 8259 normal vector
	Level 3 expansion 8259 normal vector (not for us)
	Level 2 expansion autovector (not for us)
	Level 1 printer autovector
    On the 8259, the following is connected -
	0 = FCOMM, FCTXU  status asserted
	1 = FCOMM, FCRXSA status asserted
	2 = FCOMM, FCCARRIER  status asserted
	3 = FCOMM, FCTC   status asserted
	4 = Fast timer
	5 = Disk  overflow, underrun
	6 = Hard Disk EOT
	7 = Floppy Disk EOT
*/

/* these codes are returned by hddone()	*/
#define HDD_EFLAGS	0x01	/* reserved for controller detected errors */
#define HDD_OVERRUN	0x02	/* (or underrun) */
#define HDD_TIMEOUT	0x04	/* we gave up waiting */

/* here is a structure to store information about bad sectors */
struct badblk {
	u_short b_cyl;
	u_char b_head;
	u_char b_sect;
	u_short b_flags;
	struct badblk *b_forw;
	struct badblk *b_back;
};

#define NULBAD (struct badblk *) 0

/* why the sector was considered bad */
#define BF_WRITE 0x01	/* error writing to it */
#define BF_READ	 0x02	/* error reading it */
#define BF_DATA	 0x04	/* data read does not match what was written */
#define BF_HAND	 0x08	/* selected by hand entry */
/* some housekeeping flags */
#define BF_ALLOC 0x10	/* replacement sector has been allocated */
#define BF_MARKED 0x20	/* original has been marked bad */
/* this means the data on disk is fully consistent */
#define BF_NEW	 0x40	/* found this on latest test */
#define BF_MAPPED 0x80	/* has been marked bad and mapped to replacement */

struct dskadd {
	u_short	n_cyl;
	u_short	n_head;
	u_short	n_sec;
};

#define NDRIVES	2	/* miniframe only has 2 hard drives */
struct disktab {
	char	*d_type;		/* string naming disk type */
	u_short	d_secsize;
	u_short	d_ntracks;
	u_short	d_nsectors;
	u_short	d_ncylinders;
	u_short	d_precomp;
	u_short	d_steprate;
	u_short	d_skew;
	u_short	d_nbad;
	struct badblk	*d_bforw;
	struct badblk	*d_bback;
	struct badblk	*d_badfree;	/* always points to free list pool */
	struct badblk	*d_nxtbad;	/* next available free item */
	u_short	d_maxbad;
	struct dkbad	*d_binfo;	/* bad block table, as read from disk */
	struct dskadd	d_curblk;
};
struct disktab diskinfo[NDRIVES];
struct dkbad diskbad[NDRIVES];

short drive;
struct disktab *cd;

#define READ	1
#define WRITE	2

/* temporary drive geometry stuff - eventually should be
 * replaced by a table/menu.
 */
char stname[] = "ST251-1";
#define NCYL	820		/* cylinders on a ST251-1 */
#define NHEADS	6		/* heads on a ST251-1 */
#define PRECOMP	900		/* ST251 does not require rwc */
#define STEPRATE 0		/* 0x0 = .35ms (full tilt) */

/* these things never really change, but we still put them in a table */
#define NSECTOR	17		/* sectors per track */
#define SSIZE	512		/* single sector */
/* ?? I wonder if we could format 9 * 1k sectors ?? */

#define MAXHEADS	8	/* max heads ever allowed */
#define MAXSEC		17	/* max sectors per track ever allowed */
#define TSIZE (SSIZE*NSECTOR)	/* entire track */

char *iobuf;

char	*prompt();
long	atol();
unsigned long atoh();

/* called before main - allows hardware initialization */ 
configure()
{
}

extern char end;
static char *nalloc = (char *) 0;

/* really simple minded memory allocator.
 * - no check for running past end of memory.
 * - but it does force even address alignment.
 */
char *
malloc(size)
u_long size;
{
	register char *rval;

	if ( nalloc == (char *) 0 )
	    nalloc = (char *) (((u_long) &end + 1) & ~1);

	rval = nalloc;
	nalloc = (char *) (((u_long) nalloc + size + 1) & ~1);
	return ( rval );
}

static char uname[] = "userdisk";

/* interactive  entry of disk parameters */
setparam()
{
	cd->d_type = uname;
	cd->d_secsize = SSIZE;
	cd->d_ntracks = getparam("heads ");
	cd->d_nsectors = NSECTOR;
	cd->d_ncylinders = getparam("cylinders ");
	cd->d_precomp = getparam("precomp cylinder ");
	cd->d_steprate = getparam("step rate ");
	cd->d_skew = 1;
	cd->d_nbad = 0;
	cd->d_bforw = NULBAD;
	cd->d_bback = NULBAD;
	cd->d_nxtbad = cd->d_badfree;
	cd->d_maxbad = cd->d_ntracks * (cd->d_nsectors - 1 );
	if ( cd->d_maxbad > MAXBAD )
	    cd->d_maxbad = MAXBAD;
	/* NOW should read in bad sector table from the disk */
}

getparam(msg)
char *msg;
{
	char *cp;
	int val;

	cp = prompt( msg );
	val = atol(cp);
/*	printf("%d\n",val);	*/
	return ( val );
}

gethparam(msg)
char *msg;
{
	char *cp;
	int val;

	cp = prompt( msg );
	val = atoh(cp);
	return ( val );
}

dkinit(dnum,dname)
char *dname;
{
	register struct disktab *dp;

	dp = &diskinfo[dnum];
	dp->d_type = dname;
	dp->d_secsize = SSIZE;
	dp->d_ntracks = NHEADS;
	dp->d_nsectors = NSECTOR;
	dp->d_ncylinders = NCYL;
	dp->d_precomp = PRECOMP;
	dp->d_steprate = STEPRATE;
	dp->d_skew = 1;
	dp->d_nbad = 0;
	dp->d_bforw = NULBAD;
	dp->d_bback = NULBAD;
	dp->d_nxtbad = dp->d_badfree =
	    (struct badblk *) malloc ( MAXBAD * sizeof ( struct badblk ) );
	dp->d_maxbad = dp->d_ntracks * (dp->d_nsectors - 1 );
	if ( dp->d_maxbad > MAXBAD )
	    dp->d_maxbad = MAXBAD;
	/* NOW should read in bad sector table from the disk */
	dp->d_binfo = &diskbad[dnum];
}

main()
{
	int times;
	int cyl;
	int fill;
	char *cp;
	register i;

	printf("Type return when ready");
	cp = prompt((char *) 0 );	/* wait for user */

	printf("Winchester test (ver 1.4) starting\n");

	/* must be biggest we will EVER need */
	iobuf = malloc ( TSIZE );

	for ( cp = iobuf; cp < &iobuf[SSIZE]; )
	    *cp++ = 0xae;

	/* current block for sector read/write */
	cd->d_curblk.n_cyl = 0;
	cd->d_curblk.n_head = 0;
	cd->d_curblk.n_sec = 0;

	*HD_SDH = 0x18;		/* impossible (select no drive) */

	printf("io buffer is at: ");
	hex8(iobuf);
	putchar('\n');

	picinit();
	/* initialize drive info */
	dkinit(0,stname);
	dkinit(1,stname);
	cd = &diskinfo[drive=0];
	hdinit(drive);

	for ( ;; ) {
	    printf("win[%d]: ",drive);
	    cp = prompt((char *) 0 );
	    if ( *cp == '\0' ) {
		printf("ebits:  %x\n",(*HD_EFLAGS)&0xff);
		printf("status: %x\n",(*HD_STATUS)&0xff);
		continue;
	    }
	    if ( *cp == 'q' )			/* q - quit */
		break;
	    else if ( *cp == 'i' )		/* i - initialize */
		hdinit(drive);
	    else if ( *cp == 'c' ) {		/* c - change drive */
		cp = prompt("drive (0,1)? ");
		drive = atol(cp) & 1;
		hdinit(drive);
		cd = &diskinfo[drive];
	    } else if ( *cp == 'p' )		/* p - set disk parameters */
		setparam();
	    else if ( *cp == 'h' )		/* h - help */
		hdhelp();
	    else if ( *cp == 't' )		/* t - test submenu */
		tests();
	    else if ( *cp == 'b' )		/* b - bad block submenu */
		bad();
	    else if ( *cp == 'm' )		/* m - mkboot */
		mkboot();
	    else if ( *cp == 'f' )		/* f - format tracks */
		hdformat();
	    else if ( *cp == 'n' ) {		/* n - read next sector */
		int code;
		incblk(&cd->d_curblk,cd->d_nsectors);
		if ( code = winio (iobuf,READ,cd->d_curblk.n_sec,
		    cd->d_curblk.n_cyl,cd->d_curblk.n_head) ) {
		    printf("io error: %x status = %x flags = %x\n",
			code>>8, *HD_STATUS&0xff,*HD_EFLAGS&0xff);
		} else {
		    printf("sector %d, track %d, cylinder %d\n",
		    cd->d_curblk.n_sec,cd->d_curblk.n_head,cd->d_curblk.n_cyl);
		    vxdmp(iobuf,SSIZE);
		}
	    } else if ( *cp == 'r' )		/* r - read sector */
		sectorio(READ);
	    else if ( *cp == 'w' )		/* w - write sector */
		sectorio(WRITE);
	    else if ( *cp == 'd' )		/* d - dump sector buffer */
		vxdmp(iobuf,SSIZE);
	    else if ( *cp == 'e' ) {		/* b - fill sector buffer */
		cp = prompt("fill value (0x00)? ");
		if ( *cp == '\0' )
		    fill = 0;
		else
		    fill = atoh(cp)&0xff;
		for ( i=0; i<SSIZE; i++ ) 
		    iobuf[i] = fill;
	    } else if ( *cp == 's' ) {		/* s - seek */
		/*
		++cp;
		while ( *cp && *cp == ' ' )
		    ++cp;
		*/
		printf("cylinder (0-%d)? ",cd->d_ncylinders-1);
		cp = prompt( (char *) 0 );
		cyl = atol(cp);
		if ( cyl < 0 ) cyl = 0;
		if ( cyl > NCYL-1 ) cyl = cd->d_ncylinders-1;
		hdseek(cyl);
	    } else {
		printf(" ?\n");
	    }
	}
	/* re-enter the gdb nub */
	(*((int (*) ()) 0x6c000) ) ();

	/* return should exit to boot */
}

hdhelp()
{
	printf("i - initialize (restore) drive\n");
	printf("c - change (select) drive\n");
	printf("p - change (set) drive parameters\n");
	printf("f - format range of cylinders\n");
	printf("b - bad block submenu\n");
	printf("t - test submenu\n");
	printf("s - seek to cylinder\n");
	printf("r - read a sector\n");
	printf("n - read next sector\n");
	printf("w - write a sector\n");
	printf("d - display (dump) sector buffer\n");
	printf("e - fill sector buffer\n");
	printf("m - install boot image (mkboot)\n");
	printf("q - exit this program\n");
}

/* bad block handling submenu */
bad()
{
	int cyl, head, sector;
	char *cp;

	for ( ;; ) {
	    printf("Bad block handling menu -\n");
	    printf(" l - display bad block list\n");
	    printf(" r - reread bad block list from disk\n");
	    printf(" w - write bad block list to disk\n");
	    printf(" c - clear (discard) bad block list\n");
	    printf(" e - add entry to bad block list\n");
	    printf(" q - quit to main menu\n");

	    do {
		cp = prompt("selection ? ");
	    } while ( *cp == '\0' );

	    if ( *cp == 'q' )			/* q - quit */
		break;
	    else if ( *cp == 'l' ) {		/* l - list */
		badshow();
		if ( cd->d_nbad > 16 )
		    cp = prompt("Return to continue:");
	    } else if ( *cp == 'r' )		/* r - read */
		rdbad();
	    else if ( *cp == 'w' )		/* w - write */
		wrbad();
	    else if ( *cp == 'c' ) {		/* c - clear */
		cd->d_nbad = 0;
		cd->d_bforw = NULBAD;
		cd->d_bback = NULBAD;
		cd->d_nxtbad = cd->d_badfree;
		badshow();
	    } else if ( *cp == 'e' ) {		/* e - hand entry */
		cyl = getparam("cylinder? ");
		head = getparam("head? ");
		sector = getparam("sector? ");
		(void) benter(cyl, head, sector, BF_HAND);
	    }
	}
}

/* test submenu */
tests()
{
	int cyl, head, sector;
	char *cp;

	for ( ;; ) {
	    printf("Disk test menu -\n");
	    printf(" 1 - sanity check (r/w)\n");
	    printf(" 2 - non-destructive read test\n");
	    printf(" 3 - read/write pattern test\n");
	    printf(" 4 - interleave timing test\n");
	    printf(" 9 - seek test (not recommended)\n");
	    printf(" q - quit to main menu\n");

	    do {
		cp = prompt("selection ? ");
	    } while ( *cp == '\0' );

	    if ( *cp == 'q' )
		break;
	    else if ( *cp == '1' )
		sanity();
	    else if ( *cp == '2' )
		readtest();
	    else if ( *cp == '3' )
		rwtest();
	    else if ( *cp == '4' )
		itest();
	    else if ( *cp == '9' )
		seektest();
	}
}

/* write a bootable image on this disk.
 * The i/o for the boot image is done in 1K blocks,
 * in particular, note that the size of the volume header
 * must be a 1K unit (this is verified below).
 */
#define KSIZE 1024

struct dskadd nxtblk;
struct ctvol sblock;
long lsum();

mkboot()
{
	u_long imsize;
	u_long start, end;
	u_long addr;
	int imblocks;
	int i;

	/* start writing boot image here */
	nxtblk.n_cyl = 0;
	nxtblk.n_head = 0;
	nxtblk.n_sec = 0;

	if ( sizeof(struct ctvol) != KSIZE )
	    printf("Superblock structure malformed\n");

	start = gethparam ( "starting address of image: " );
	end = gethparam ( "ending address of image: " );
	imsize = end - start + 1;
	imblocks = (imsize+KSIZE-1) / KSIZE;

	printf("image size: %d bytes\n",imsize);
	printf("image uses %d 1K blocks\n",imblocks);
	
	zfill ( (char *) &sblock, KSIZE );

	sblock.magic = (long) CTMAGIC;
	sblock.nheads = cd->d_ntracks;
	sblock.sectrk = cd->d_nsectors;
	sblock.seccyl = cd->d_ntracks * cd->d_nsectors;

	sblock.flags = 1;		/* double density floppy */
	sblock.ldrptr = (long) 1;
	sblock.ldrcnt = imblocks;
	sblock.cksum = (long) (-1) -
			lsum ( (long *) &sblock, (KSIZE/sizeof(long))-1 );

	dwrite( (char *) &sblock );

	/* It would seem tidy to zero the unused part of the last block,
	 * but in fact it is unnecessary (It will be the BSS area anyway
	 * and will get zeroed by the startup code if need be), and would
	 * be bad to do unless the last block was copied into a local buffer.
	 */
	addr = start;
	for ( i=0; i<imblocks; i++ ) {
	    dwrite ( (char *) addr );
	    addr += KSIZE;
	}
}

zfill(buf,nby)
register char *buf;
register nby;
{
	while ( nby-- )
		*buf++ = 0;
}

long
lsum (buf, nlong)
register long *buf;
register nlong;
{
	unsigned long sum = 0;

	while ( nlong-- )
		sum += *buf++;
	return ( sum );
}

/* bsize must be an integral number of sectors, at present
 * bsize is always 1024 bytes (KSIZE) in this program.
 */
dwrite(buf)
char *buf;
{
	/* leave off last sector in a track when writing the image
	 * for the boot roms */
	int nspt = cd->d_nsectors & ~1;

	if ( nxtblk.n_cyl != 0 ) {
	    printf("Image too big\n");
	    return;
	}
	if ( winio(buf,WRITE,nxtblk.n_sec,0,nxtblk.n_head) )
	    printf("Boot write error %d %d",nxtblk.n_head,nxtblk.n_sec);

	incblk(&nxtblk,nspt);
	if ( nxtblk.n_cyl != 0 ) {
	    printf("Image too big\n");
	    return;
	}
	if ( winio(buf+SSIZE,WRITE,nxtblk.n_sec,0,nxtblk.n_head) )
	    printf("Boot write error %d %d",nxtblk.n_head,nxtblk.n_sec);
	incblk(&nxtblk,nspt);
}

/* sequence thru the disk blocks
 * Notice: each track gets n_sec-1 blocks written, the boot roms
 * require this kind of business.
 */
incblk(nbp,ns)
struct dskadd *nbp;
{
	if ( ++nbp->n_sec < ns )
	    return;
	nbp->n_sec = 0;
	if ( ++nbp->n_head < cd->d_ntracks )
	    return;
	nbp->n_head = 0;
	++nbp->n_cyl;
}

/* read bad block info from disk */
rdbad()
{
	register struct dkbad *bp = cd->d_binfo;
	register i;
	register num;

	/* there is a copy at the end of each track */
	for ( i=0; i<cd->d_ntracks; i++ ) {
	    if ( winio((char *)bp,READ,cd->d_nsectors-1,cd->d_ncylinders-1,i) )
		printf("trouble reading bad block table from track %d\n",i);
	    else
		break;
	}

	if ( i >= cd->d_ntracks )
	    return(0);

	if ( i != 0 )
	    printf("Bad block list read from track %d\n",i);

	if ( bp->bt_csn != BTMAGIC )
	    return(0);

	num = 0;
	for ( i=0; i<MAXBAD; ++i ) {
	    if ( (short) bp->bt_bad[i].bt_cyl == -1 )
		break;
	    ++num;
	    (void) benter(bp->bt_bad[i].bt_cyl, bp->bt_bad[i].bt_trksec>>8,
		bp->bt_bad[i].bt_trksec&0xff, BF_MAPPED);
	}

	if ( num == 0 )
	    printf("bad block list empty on disk\n");
	else
	    printf("%d bad blocks in list read from disk\n",num);
	return ( 1 );
}

wrbad()
{
	int new, old;
	register struct dkbad *bp = cd->d_binfo;
	register struct badblk *bb;
	register i;

	bb = cd->d_bforw;

	bp->bt_csn = BTMAGIC;
	bp->bt_flag = 0;

	new = old = 0;
	for ( i=0; bb != NULBAD; i++ ) {
	    if ( bb->b_flags & BF_MAPPED )
		++old;
	    else
		++new;

	    bp->bt_bad[i].bt_cyl = bb->b_cyl;
	    bp->bt_bad[i].bt_trksec = bb->b_head<<8 | bb->b_sect;

	    bb = bb->b_forw;
	}

	/* fill rest of table */
	for ( ; i<MAXBAD; ++i ) {
	    bp->bt_bad[i].bt_cyl = -1;
	    bp->bt_bad[i].bt_trksec = -1;
	}

	/* write a copy at the end of each track */
	for ( i=0; i<cd->d_ntracks; i++ ) {
	    if ( winio((char *)bp,WRITE,cd->d_nsectors-1,cd->d_ncylinders-1,i) )
		printf("trouble writing bad block table to track %d\n",i);
	}
}

hdinit(drive)
{
	int stat;
	register timeout;

/*	*HD_RESET_ON = 0;	*/

	*DISK_BIU_RESET = 0;	/***/
	*HD_RESET_OFF = 0;
	*HD_WPC = cd->d_precomp>>2;	/* RWC cylinder */
	if ( drive )
	    *HD_SDH = 0x28;		/* 512 byte sectors, drive 1 */
	else
	    *HD_SDH = 0x20;		/* 512 byte sectors, drive 0 */

	*HD_COMMAND = HDC_RESTORE|cd->d_steprate;	/* restore */
	if ( stat = hddone() )
	    hderr("Restore did not complete normally",stat);
	
	/* should verify track zero status after the restore !!!!! */
	if ( (stat = *HD_STATUS) & HDS_MASK )
	    hderr("Restore failed",stat);
}

sanity()
{
	int cyl, head, sector;
	int scyl, ecyl;
	int lsecsize;
	long *lbuf;
	char *cp;
	register i;
	register long tag;

	lbuf = (long *) iobuf;

	printf("starting cyl (0-%d)? ",cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	scyl = atol(cp);
	if ( scyl < 0 ) scyl = 0;
	if ( scyl > cd->d_ncylinders-1 ) scyl = cd->d_ncylinders-1;

	printf("ending cyl (%d-%d)? ",scyl,cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	ecyl = atol(cp);
	if ( ecyl < 0 ) ecyl = 0;
	if ( ecyl > cd->d_ncylinders-1 ) ecyl = cd->d_ncylinders-1;

	lsecsize = cd->d_secsize / sizeof(long);

	for ( cyl=scyl; cyl<=ecyl; cyl++ )
	    for ( head=0; head<cd->d_ntracks; head++ )
		for ( sector=0; sector<cd->d_nsectors; sector++ ) {
		    tag = cyl<<16 | head<<8 | sector;
		    for ( i=0; i<lsecsize; i++ )
			lbuf[i] = tag;
		    if ( winio((char *)lbuf,WRITE,sector,cyl,head) )
			sanerr("write err",cyl,head,sector);
		}

	for ( cyl=scyl; cyl<=ecyl; cyl++ )
	    for ( head=0; head<cd->d_ntracks; head++ )
		for ( sector=0; sector<cd->d_nsectors; sector++ ) {
		    for ( i=0; i<lsecsize; i++ )
			lbuf[i] = 0;
		    if ( winio((char *)lbuf,READ,sector,cyl,head) )
			sanerr("read err",cyl,head,sector);
		    tag = cyl<<16 | head<<8 | sector;
		    for ( i=0; i<lsecsize; i++ )
			if ( lbuf[i] != tag )
			    sanerr("read err",cyl,head,sector);
		}
	printf("End of sanity check!\n");
}

sanerr(msg,c,h,s)
char *msg;
{
	printf("%s during sanity check, CHS = %d %d %d\n",msg,c,h,s);
}

/* here are the registers in the system timer 8253.
 * a 76.8 kHz clock is the input to timers 0 and 1.
 * the output of timer 1 is input to counter 2.
 * If counter 2 is in mode 3 (like the ROM sets it up), each tick
 *   causes it to decrement by 2.
 * If counter 2 is in mode 2, it decrements by 1 for each tick.
 */
#define TIMER0	((short *) 0xc00000)
#define TIMER1	((short *) 0xc00002)
#define TIMER2	((short *) 0xc00004)
#define TMR_CW	((short *) 0xc00006)

#define CW_C0		0x00	/* select counter 0 */
#define CW_C1		0x40	/* select counter 1 */
#define CW_C2		0x80	/* select counter 2 */

#define CW_LATCH	0x00	/* latch counter */
#define CW_LSB		0x10	/* do lsb only */
#define CW_MSB		0x20	/* do msb only */
#define CW_LM		0x30	/* do lsb, then msb */

#define CW_M0		0x00	/* Mode 0, interrupt on TC */
#define CW_M1		0x02	/* Mode 1, prog. one shot */
#define CW_M2		0x04	/* Mode 2, rate generator */
#define CW_M3		0x06	/* Mode 3, sq. wave generator */
#define CW_M4		0x08	/* Mode 4, sw trig. strobe */
#define CW_M5		0x0a	/* Mode 5, hw trig. strobe */

#define CW_BIN		0x00	/* count in binary (16 bits) */
#define CW_BCD		0x01	/* count in BCD (4 decades) */

#define NLINES 20

/* test interleave via timer */
itest()
{
	short waits[NLINES];
	long delay;
	short sdelay;
	int head, cyl;
	int nhds;
	char *cp;
	register short count;
	register i;

/*
	cp = prompt( "heads per cyl ? " );
	nhds = atol(cp);
	if ( nhds <= 0 ) nhds = 1;
	if ( nhds > cd->d_ntracks ) nhds = 1;
*/
	nhds = cd->d_ntracks;

	cp = prompt( "cyl ? " );
	cyl = atol(cp);
	if ( cyl < 0 ) cyl = 0;
	if ( cyl >= cd->d_ncylinders ) cyl = cd->d_ncylinders-1;

	/* set up timer1 to give us 3200 ticks per second.
	 * this is 76800/3200 = 24  (need lsb only)	*/
	*TMR_CW = (CW_C1|CW_LM|CW_M2|CW_BIN);	/* 0x74 */
	asm("nop");
	*TIMER1 = 24;
	asm("nop");
	*TIMER1 = 0;
	asm("nop");

	for ( i=0; i<NLINES; i++ ) {
	    *TMR_CW = (CW_C2|CW_LM|CW_M2|CW_BIN);
	    asm("nop");
	    *TIMER2 = 0;
	    asm("nop");
	    *TIMER2 = 0;

	    /* This little loop lasts .28 seconds (28 ticks of .01 sec)
	    for ( delay = 50000; delay--; )
		;
	    */

	    for ( head=0; head<nhds; head++ )
		if ( trackio (iobuf,READ,cyl,head) )
		    printf("IO err ");

	    *TMR_CW = (CW_C2|CW_LATCH);
	    asm("nop");
	    /* must mask here, else high bits are ones */
	    count = *TIMER2 & 0xff;		/* lsb */
	    asm("nop");
	    count = -(*TIMER2<<8 | count);	/* msb */
	    waits[i] = count;

#define SAME
#ifndef SAME
	    /* now delay for 1 ms per 180 counts */
	    sdelay = 180 * (i+1);	/* no long mult (mulsi3) yet */
	    for ( delay = sdelay; delay--; )
		;
#endif
	}

	for ( i=0; i<NLINES; i++ ) {
	    printf("count = %d (",waits[i]);
	    hex4(waits[i]);
	    printf(")\n");
	}
}

#define I_ONETOONE	1
#define I_OTHER		0
#define BADONE		0x8000	/* OR this onto fmtid to mark bad */
#define FMTTRYS		10

/* display the bad block table */
badshow()
{
	register i;
	register struct badblk *bp;

	if ( cd->d_nbad == 0 ) {
	    printf("No bad sectors.\n");
	    return;
	}

	printf("\nbad sector table:\n");
	bp = cd->d_bforw;
	for ( i=0; bp != NULBAD; i++ ) {
	    if ( i < 10 ) printf(" ");
	    if ( i < 100 ) printf(" ");
	    printf("%d: cyl %d, head %d, sector %d ", i+1,
	    bp->b_cyl,bp->b_head,bp->b_sect);
	    if ( bp->b_flags & BF_HAND )
		putchar ( 'H' );
	    if ( bp->b_flags & BF_WRITE )
		putchar ( 'W' );
	    if ( bp->b_flags & BF_READ )
		putchar ( 'R' );
	    if ( bp->b_flags & BF_DATA )
		putchar ( 'D' );
	    if ( bp->b_flags & BF_NEW )
		printf(" (new)");
	    if ( bp->b_flags & BF_MAPPED )
		printf(" (mapped)");
	    putchar ( '\n' );
	    bp = bp->b_forw;
	}
	if ( cd->d_nbad > cd->d_maxbad )
	    printf("bad sector table overflowed, info lost!\n");
}

/* clear the NEW bit in all bad sector table entries */
clearnew()
{
	register struct badblk *bp = cd->d_bforw;

	while ( bp != NULBAD ) {
	    bp->b_flags &= ~BF_NEW;
	    bp = bp->b_forw;
	}
}

/* we don't EVER expect to have to use this structure, but just in
 * case !!
 */
struct deadtrack {
	u_short d_cyl;
	u_char d_head;
};

#define MAXDEAD	20
struct deadtrack deadtab[MAXDEAD];
int ndead;

hdformat()
{
	short fmtbuf[MAXHEADS][MAXSEC];
	int cyl, head, times;
	int scyl, ecyl;
	int soft, hard;
	int nsoft, nhard;
	int pass;
	int spt;
	int pos;
	int error;	/* any error in cylinder */
	int terr;	/* any error in track */
	char badsec[MAXSEC];
	register char *cp;
	register i;

	printf("starting cyl (0-%d)? ",cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	scyl = atol(cp);
	if ( scyl < 0 ) scyl = 0;
	if ( scyl > cd->d_ncylinders-1 ) scyl = cd->d_ncylinders-1;

	printf("ending cyl (%d-%d)? ",scyl,cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	ecyl = atol(cp);
	if ( ecyl < 0 ) ecyl = 0;
	if ( ecyl > cd->d_ncylinders-1 ) ecyl = cd->d_ncylinders-1;

	cp = prompt( "iterations ? " );
	times = atol(cp);
	if ( times <= 0 ) times = 1;

#ifdef OLD
/* now this is in the disk parameter structure */
	cp = prompt( "skew ? " );
	if ( *cp == '\0' )
	    skew = 1;
	else {
	    skew = atol(cp);
	    if ( skew <= 0 ) skew = 0;
	}
#endif

#ifdef VARFMT
/* I was curious to find out if I could push the format to 18 or more
 * sectors per track -- no luck (as expected), the whole track ends up
 * unreadable (IDNF) -- so no sense keeping this prompt.
 * Now this is just a bit of history -- if it was desired to allow
 * truly variable sector formats, one would need to define MAXSEC or such
 * and watch how arrays are dimensioned here and there.
 * No point in this, it is clear, at least for this hardware, we are always
 * talking 17 sectors of 512 bytes per track, no two ways about it.
 */
	cp = prompt( "sectors per track? " );
	if ( *cp == '\0' )
	    spt = cd->d_nsectors;
	else
	    spt = atol(cp);
#endif

	spt = cd->d_nsectors;

	/* 1:1 interleave format */
	for ( head=0; head<cd->d_ntracks; head++ ) {
	    pos = (head * cd->d_skew) % spt;
	    for ( i=0; i<spt; ++i ) {
		fmtbuf[head][pos] = i;
/*		fmtbuf[head][pos] = BADONE | i;	*/
		pos = (pos+1) % spt;
	    }
	    printf("head %d:",head);
	    for ( i=0; i<spt; ++i ) {
		if ( fmtbuf[head][i] & BADONE )
		    printf(" B%d", fmtbuf[head][i]&0xff);
		else
		    printf("  %d", fmtbuf[head][i]);
	    }
	    printf("\n");
	}

	hdinit(drive);	/* move heads to track 0 */
	ndead = nhard = nsoft = 0;
	error = 1;	/* trigger first line */
	clearnew();

	for ( cyl=scyl; cyl<=ecyl; cyl++ ) {
	    /* restore information line after errors */
	    if ( error ) {
		printf("formatting cylinder:    0 ");
		outby(cd->d_ntracks,' ');
	    }
	    error = 0;

	    outbs(cd->d_ntracks + 5 );
	    /* do not have %4d, so simulate leading blanks */
	    if ( cyl < 1000 )
		putchar ( ' ' );
	    if ( cyl < 100 )
		putchar ( ' ' );
	    if ( cyl < 10 )
		putchar ( ' ' );
	    printf("%d ",cyl);
	    for ( head=0; head<cd->d_ntracks; head++ ) {
		soft = hard = 0;
		for ( i=0; i<FMTTRYS; i++ ) {
		    /* this loop "pounds" the format into the metal :-) */
		    for ( pass=0; pass<times; ++pass )
			winfmt ((char *)fmtbuf[head],cyl,head,spt,I_ONETOONE);
		    if ( ckfmt(cyl,head,badsec,&terr) )
			hard = 1;
		    else if ( hard ) {	/* correctable error */
			soft = 1;
			hard = 0;
#ifdef	NEVER
			printf("\ncyl %d, head %d ",cyl,head);
			printf("corrected after %d failures\n",i);
#endif
			break;
		    } else {
			break;	/* formatted OK (may have bad blks) */
		    }
		} /* done with the track -- show symbol */

		if ( hard ) {
		    /* we never expect this to happen.
		     * ( a track that simply won't format ).
		     * better let the guy know !!
		     */
		    printf("@");
		    if ( ndead < MAXDEAD ) {
			deadtab[ndead].d_cyl = cyl;
			deadtab[ndead].d_head = head;
			++ndead;
		    }
		    error = 1;
		} else if ( terr ) {
		    showsym(terr,badsec);
		    error = 1;
		} else if ( soft ) {
		    printf("*");
		    error = 1;
		} else
		    printf(".");

		/* collect some statistics */
		nhard += hard;
		nsoft += soft;
	    }
	    if ( error )
		printf("\n");	/* so line with errors doesn't get overwrit */
	}
	if ( ! error )
	    printf("\n");
	printf("%d corrected bad tracks\n",nsoft);
	printf("%d unformattable tracks\n",nhard);
	if ( ndead )
	    printf("list of unformattable tracks:\n");
	for ( i=0; i<ndead; ++i )
	    printf("%d: cylinder, head = %d, %d\n", i+1,
		deadtab[i].d_cyl, deadtab[i].d_head );
	if ( ndead < nhard )
	    printf("*** This is not a full list !\n");

	badshow();	/* show the bad sector list */
}

/* output a series of backspace characters */
outbs(nbs)
{
	while ( nbs-- )
	    putchar ( '\b' );
}
/* output a series of any characters */
outby(nby,by)
{
	while ( nby-- )
	    putchar ( by );
}

/* check a track that was just formatted.
 * quick test - first try to read the whole track, if that works, fine.
 * if that fails, detail out the errors (adding to bad block list).
 * if every sector is bad, set a code so we can try reformatting the track.
 * (it seems that this controller has a quirk of sometimes not formatting
 * tracks right, the WD1010-00 does this, the WD1010-05 seems better.
 *
 * We do the quick test of the format -- only one read try per sector.
 * This won't catch some errors (that are "soft" - sometimes they read,
 * sometimes they don't) we leave this for test 2 and 3.  We mainly want
 * to recognize the case where the track flat didn't format right.  But, as
 * long as we are at it, we keep track of any bad sectors we do discover.
 */

#define FMTPAT	0xffff
ckfmt(cyl,head,badsec,bad)
char badsec[];
int *bad;
{
	int error;
	int count;
	register sector;
	register u_short *wp;
	register u_short *etp;

	for ( sector=0; sector<cd->d_nsectors; ++sector )
		badsec[sector] = 0;

	/* first try to read entire track, if trouble, detail the sectors */
	if ( trackio (iobuf,READ,cyl,head) ) {
	    count = 0;
	    for ( sector=0; sector<cd->d_nsectors; sector++ )
		if ( secchk(cyl,head,sector,FMTPAT) ) {
		    badsec[sector] = 1;
		    ++count;
		}
	    *bad = 1;
	    if ( count == cd->d_nsectors )
		return ( 1 );	/* looks like a bad format */
	    else
		return ( 0 );	/* OK, we are done */
	}

	/* OK, it seemed to read OK, check the data */
	error = 0;
	etp = (u_short *) &iobuf[TSIZE];
	for ( wp=(u_short *)iobuf; wp<etp; ) {
	    if ( *wp++ != FMTPAT ) {
		++error;
		break;
	    }
	}

	if ( error ) {
	    for ( sector=0; sector<cd->d_nsectors; sector++ )
		if ( secchk(cyl,head,sector,FMTPAT) )
		    badsec[sector] = 1;
	}
	*bad = error;
	return ( 0 );
}

/* see if we can read a sector, and validate its contents.
 * any trouble -- try to enter it in the bad sector table.
 */
secchk(cyl,head,sector,patt)
u_short patt;
{
	register u_short *wp;
	register u_short *esp;


	/* can we read it at all ?? */
	if ( winio (iobuf,READ,sector,cyl,head) ) {
	    (void) benter(cyl, head, sector, BF_READ);
	    return ( 1 );
	}

	/* OK, we read it, but is the data right ?? */
	esp = (u_short *) &iobuf[SSIZE];
	for ( wp=(u_short *)iobuf; wp<esp; ) {
	    if ( *wp++ != patt ) {
		(void) benter(cyl, head, sector, BF_DATA);
		return ( 2 );
	    }
	}
	return ( 0 );
}

winfmt (buf,cyl,head,spt,onetoone)
char *buf;
{
	int stat;
	register nio;
	register unsigned dmatmp;

	/* first pulse the reset line (groping in the dark).
	 * ?? maybe only do this as recovery from overrun/underrun.
	 */
	*HD_RESET_ON = 0;
	*HD_RESET_OFF = 0;

	/* First, set up the dma */
	*DISK_DMA_DISABLE = 0;
	*DISK_BIU_RESET = 0;

	nio = 2*spt;
	nio = -(nio>>1);
	*DISK_DMA_COUNT = nio;

	dmatmp = ((unsigned int) buf) >> 1;	/* word address */
	dmatmp &= 0xffff;
#ifdef DEBUG
	printf("lower DMA address: ");
	hex4(dmatmp);
	putchar('\n');
#endif
	*DISK_DMA_LADDR = dmatmp;

	dmatmp = ((unsigned int) buf) >> 1;	/* word address */
	dmatmp >>= 16;
#ifdef DEBUG
	printf("upper DMA address: ");
	hex4(dmatmp);
	putchar('\n');
#endif

	*DISK_DMA_UADDR_W = dmatmp;		/* formatting is a write */

/* Second tell the controller to do the i/o */

	/* this controller does implied seeks */
	*HD_CYHIGH = cyl>>8;
	*HD_CYLOW = cyl;

	*HD_DMA_ENABLE = 0;

	*HD_SCOUNT = spt;	/* sectors in this track */

	if ( onetoone )
	    *HD_SNUM = 53;	/* gap size in bytes */
	else
	    *HD_SNUM = 28;	/* gap size in bytes */

#ifdef DEBUG
	printf("SDH ");
	hex2(0x20 | drive<<3 | head&0x07);
	putchar('\n');
#endif
	*HD_SDH = 0x20 | drive<<3 | head&0x07;

	/* GO for it !! */

	*HD_COMMAND = HDC_FORMAT;

	/* This routine does "noisy" error recovery -- it just prints
	 * messages and never returns a status -- at present this is
	 * not causing any problems, but would be something to tidy up
	 * (in the same fashion as winio() and trackio() ) in the future.
	 */
	if ( stat = hddone() )
	    hderr("Format did not complete normally",stat);

	stat = *HD_STATUS;
#ifdef DEBUG
	printf("Done with status: ");
	hex2(stat);
	putchar('\n');
#endif
	if ( stat & HDS_MASK )
	    hderr("Format IO error",stat);
}

sectorio(iotype)
{
	int cyl, head, sector;
	int code;
	register char *cp;

	printf("cylinder (0-%d)? ",cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	cyl = atol(cp);
	if ( cyl < 0 ) cyl = 0;
	if ( cyl > cd->d_ncylinders-1 ) cyl = cd->d_ncylinders-1;

	printf("head (0,%d)? ",cd->d_ntracks-1);
	cp = prompt( (char *) 0 );
	head = atol(cp);
	if ( head < 0 ) head = 0;
	if ( head > cd->d_ntracks-1 ) head = cd->d_ntracks-1;

	printf("sector (0-%d)? ",cd->d_nsectors-1);
	cp = prompt( (char *) 0 );
	sector = atol(cp);
	if ( sector < 0 ) sector = 0;
	if ( sector > cd->d_nsectors-1 ) sector = cd->d_nsectors-1;

	cd->d_curblk.n_cyl = cyl;
	cd->d_curblk.n_head = head;
	cd->d_curblk.n_sec = sector;

	if ( code = winio (iobuf,iotype,sector,cyl,head) ) {
	    printf("io error: %x status = %x flags = %x\n",
		code>>8, *HD_STATUS&0xff,*HD_EFLAGS&0xff);
	    return;
	}

	if ( iotype == READ )
	    vxdmp(iobuf,SSIZE);
}

/* running this on an entire ST251-1 took 2min 2sec (122sec).
 * This disk has 820*6*17*512 = 41,820 kb, so the effective transfer rate is:
 * 41,820 / 122 = 343 kb/s
 * Not super impressive, but OK (the serial i/o slows it down a bit).
 * (and this includes the seek time).
 * *** NOW faster with skew=1 format, namely 1min 49sec (109sec)
 */
readtest()
{
	int scyl, ecyl, times;
	int cyl, head, sector;
	int error;	/* flag for bad cylinders */
	int terr;	/* flag for bad tracks */
	char badsec[MAXSEC];
	register char *cp;
	register i;

	printf("starting cyl (0-%d)? ",cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	scyl = atol(cp);
	if ( scyl < 0 ) scyl = 0;
	if ( scyl > cd->d_ncylinders-1 ) scyl = cd->d_ncylinders-1;

	printf("ending cyl (%d-%d)? ",scyl,cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	ecyl = atol(cp);
	if ( ecyl < scyl ) ecyl = scyl;
	if ( ecyl > cd->d_ncylinders-1 ) ecyl = cd->d_ncylinders-1;

	cp = prompt( "iterations ? " );
	times = atol(cp);
	if ( times <= 0 ) times = 1;

	error = 1;	/* trigger first info line */
	clearnew();

	for ( cyl=scyl; cyl<=ecyl; cyl++ ) {
	    /* restore information line after errors */
	    if ( error ) {
		printf("reading cylinder:    0 ");
		outby(cd->d_ntracks,' ');
	    }
	    error = 0;

	    outbs ( cd->d_ntracks + 5 );
	    /* do not have %4d, so simulate leading blanks */
	    if ( cyl < 1000 )
		putchar ( ' ' );
	    if ( cyl < 100 )
		putchar ( ' ' );
	    if ( cyl < 10 )
		putchar ( ' ' );
	    printf("%d ",cyl);

	    for ( head=0; head<cd->d_ntracks; head++ ) {
		for ( i=0; i<cd->d_nsectors; ++i )
		    badsec[i] = 0;
		terr = 0;

		for ( i=0; i<times; ++i ) {
		    if ( ! terr )
			if ( trackio (iobuf,READ,cyl,head) )
			    terr = 1;
		    if ( terr ) {
			for ( sector=0; sector<cd->d_nsectors; sector++ ) {
			    if ( badsec[sector] )
				continue;
			    if ( winio (iobuf,READ,sector,cyl,head) ) {
				badsec[sector] = 1;
				(void) benter(cyl, head, sector, BF_READ);
			    }
			}
		    }
		}
		showsym(terr,badsec);
		error += terr;
	    }
	    if ( error )
		printf("\n");
	}

	if ( ! error )
	    printf("\n");
	badshow();	/* display bad sector table */
	/* end of non-destructive read-only test */
}

showsym(err,badsec)
char badsec[];
{
	register num;
	register i;

	if ( err ) {
	    num = 0;
	    for ( i=0; i< cd->d_nsectors; ++i )
		if ( badsec[i] )
		    ++num;
		printf("%c",(num<10) ? '0'+num : 'A'-10+num );
	} else
	    printf(".");
}

/* here are the Purdue/EE severe burnin patterns */
u_short spat[] = {
0xf00f, 0xec6d, 0031463,0070707,0133333,0155555,0161616,0143434,
0107070,0016161,0034343,0044444,0022222,0111111,0125252, 052525,
0125252,0125252,0125252,0125252,0125252,0125252,0125252,0125252,
#ifndef	SHORTPASS
0125252,0125252,0125252,0125252,0125252,0125252,0125252,0125252,
 052525, 052525, 052525, 052525, 052525, 052525, 052525, 052525,
#endif
 052525, 052525, 052525, 052525, 052525, 052525, 052525, 052525
 };
#define	NPT	(sizeof (spat) / sizeof (u_short))

rwtest()
{
	int scyl, ecyl, times;
	int cyl, head;
	int error;
	int severe;
	char badsec[MAXSEC];
	int terr;
	char *resp;
	char *cp;
	u_short patt;
	register i;

#ifdef OLD
	printf("WARNING !!!\n");
	printf("This test destroys all data on the drive\n");
	resp = prompt("Enter Y to continue: ");
	if ( *resp != 'Y' )
	    return;
#endif

	printf("starting cyl (0-%d)? ",cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	scyl = atol(cp);
	if ( scyl < 0 ) scyl = 0;
	if ( scyl > cd->d_ncylinders-1 ) scyl = cd->d_ncylinders-1;

	printf("ending cyl (%d-%d)? ",scyl,cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	ecyl = atol(cp);
	if ( ecyl < scyl ) ecyl = scyl;
	if ( ecyl > cd->d_ncylinders-1 ) ecyl = cd->d_ncylinders-1;

	severe = 0;
	cp = prompt( "pattern (0xaaaa)? " );
	if ( *cp == '\0' )
	    patt = 0xaaaa;
	else if ( *cp == 's' ) {
	    severe = 1;
	    printf("severe burn-in (%d patterns)\n",NPT);
	} else
	    patt = atoh(cp);

	cp = prompt( "iterations ? " );
	times = atol(cp);
	if ( times <= 0 ) times = 1;

	error = 1;	/* trigger first info line */
	clearnew();

	for ( cyl=scyl; cyl<=ecyl; cyl++ ) {
	    /* restore information line after errors */
	    if ( error ) {
		printf("testing cylinder:    0 ");
		outby(cd->d_ntracks,' ');
	    }
	    error = 0;

	    outbs ( cd->d_ntracks + 5 );
	    /* do not have %4d, so simulate leading blanks */
	    if ( cyl < 1000 )
		putchar ( ' ' );
	    if ( cyl < 100 )
		putchar ( ' ' );
	    if ( cyl < 10 )
		putchar ( ' ' );
	    printf("%d ",cyl);

	    for ( head=0; head<cd->d_ntracks; head++ ) {
		for ( i=0; i<cd->d_nsectors; i++ )
		    badsec[i] = 0;
		terr = 0;

		if ( severe ) {
		    for ( i=0; i<NPT; ++i )
			testtrack(cyl, head, times, spat[i], badsec, &terr );
		} else {
		    testtrack(cyl, head, times, patt, badsec, &terr );
		}

		showsym(terr,badsec);
		error += terr;
	    }
	    if ( error )
		printf("\n");
	}

	if ( ! error )
	    printf("\n");
	badshow();	/* display the bad sector table */
	/* end of exhaustive read/write test */
}

testtrack(cyl, head, times, patt, badsec, error)
u_short patt;
char badsec[];
int *error;
{
	int oops = *error;
	int pass;
	int sector;
	register u_short *wp;
	register u_short *ep;

	/* try to read and write whole tracks.
	 * If there is any trouble doing this, detail out
	 * the errors sector by sector.
	 * The full track i/o is *much* faster than doing it
	 * sector by sector (and works most every time).
	 */
	if ( ! oops ) {
	    /* fill the track buffer with the test pattern */
	    ep = (u_short *) &iobuf[TSIZE];
	    for ( wp=(u_short *)iobuf; wp<ep; )
		*wp++ = patt;

	    for ( pass=0; pass<times; ++pass ) {
		if ( trackio (iobuf,WRITE,cyl,head) ) {
		    ++oops;
		    break;
		}
		if ( trackio (iobuf,READ,cyl,head) ) {
		    ++oops;
		    break;
		}
		for ( wp=(u_short *)iobuf; wp<ep; ) {
		    if ( *wp++ != patt ) {
			++oops;
			break;
		    }
		}
	    }
	}
	/* end of quick test */

	if ( oops == 0) {
	    *error = 0;
	    return;
	}

	/* OK this is a bad one, go for detail. */

	for ( pass=0; pass<times; ++pass ) {
	    /* fill the sector buffer with the test pattern */
	    ep = (u_short *) &iobuf[SSIZE];
	    for ( wp=(u_short *)iobuf; wp<ep; )
		*wp++ = patt;

	    /* write -- sector by sector */
	    for ( sector=0; sector<cd->d_nsectors; sector++ ) {
		if ( badsec[sector] )
		    continue;
		if ( winio (iobuf,WRITE,sector,cyl,head) ) {
		    badsec[sector] = 1;
		    (void) benter(cyl, head, sector, BF_WRITE);
		}
	    }

	    /* read -- sector by sector */
	    for ( sector=0; sector<cd->d_nsectors; sector++ ) {
		if ( badsec[sector] )
		    continue;
		if ( secchk(cyl,head,sector,patt) )
		    badsec[sector] = 1;
#ifdef NEVER
/* replaced by secchk() */
		if ( winio (iobuf,READ,sector,cyl,head) ) {
		    badsec[sector] = 1;
		    (void) benter(cyl, head, sector, BF_READ);
		    continue;
		}
		/* the read went OK, now check the data */
		for ( wp=(u_short *)iobuf; wp<ep ; ) {
		    if ( *wp++ != patt ) {
			badsec[sector] = 1;
			(void) benter(cyl, head, sector, BF_DATA);
			break;
		    }
		}
#endif
	    }
	} /* end of "times" loop */

	*error = 1;
}

/* make an entry in the bad sector list.
 * avoid making duplicate entries, OR in the reason codes.
 */
benter( cyl, head, sector, why )
{
	register struct badblk *bp;
	register struct badblk *new;

	for ( bp = cd->d_bforw; bp != NULBAD; bp = bp->b_forw ) {
	    if ( bp->b_cyl==cyl && bp->b_head==head && bp->b_sect==sector ) {
		bp->b_flags |= why;
		return ( 0 );
	    }
	}

	/* Aha!  A new one! */
	++cd->d_nbad;

	if ( cd->d_nxtbad >= cd->d_badfree + cd->d_maxbad ) {
	    /* The table filling up is bad, it means we loose information.
	     * If we really have a disk this messed up, we will have to rethink
	     * this whole scheme (currently the bad block mapping logic uses
	     * a table holding at most 126 bad blocks on the disk itself.)
	     */
	    if ( cd->d_nbad == cd->d_maxbad+1 )
		printf("BAD SECTOR TABLE FULL\n");
	    return ( 1 );
	}

	/* allocate new entry, and fill it. */
	new = cd->d_nxtbad++;
	new->b_cyl = cyl;
	new->b_head = head;
	new->b_sect = sector;
	new->b_flags = (why | BF_NEW);

	if ( (bp = cd->d_bforw) == NULBAD ) {
	    /* first entry ever */
	    new->b_back = new->b_forw = NULBAD;
	    cd->d_bback = cd->d_bforw = new;
	    return ( 1 );
	}

	if ( btest ( new, bp ) < 0 ) {
	    /* add to start of list */
	    new->b_forw = bp;
	    new->b_back = NULBAD;
	    bp->b_back = cd->d_bforw = new;
	    return ( 1 );
	}

	while ( btest ( new, bp ) > 0 ) {
	    if ( bp->b_forw == NULBAD ) {
		/* add to end of list */
		new->b_forw = NULBAD;
		new->b_back = bp;
		cd->d_bback = bp->b_forw = new;
		return ( 1 );
	    }
	    bp = bp->b_forw;
	}

	/* add before this entry */
	new->b_forw = bp;
	new->b_back = bp->b_back;
	bp->b_back->b_forw = new;
	bp->b_back = new;
	return ( 1 );
}

btest ( new, ref )
struct badblk *new, *ref;
{
	if ( new->b_cyl < ref->b_cyl )
	    return ( -1 );
	if ( new->b_cyl > ref->b_cyl )
	    return ( 1 );
	if ( new->b_head < ref->b_head )
	    return ( -1 );
	if ( new->b_head > ref->b_head )
	    return ( 1 );
	return ( new->b_sect - ref->b_sect );
}

winio (buf,io,sector,cyl,head)
char *buf;
{
	int stat;
	int code;
	register nio;
	register unsigned dmatmp;

	/* first pulse the reset line (groping in the dark).
	 * ?? maybe only do this as recovery from overrun/underrun.
	 */
	*HD_RESET_ON = 0;
	*HD_RESET_OFF = 0;

	/* First, set up the dma */
	*DISK_DMA_DISABLE = 0;
	*DISK_BIU_RESET = 0;

	nio = SSIZE;
	nio = -(nio>>1);
	*DISK_DMA_COUNT = nio;

	dmatmp = ((unsigned int) buf) >> 1;	/* word address */
	dmatmp &= 0xffff;
#ifdef DEBUG
	printf("lower DMA address: ");
	hex4(dmatmp);
	putchar('\n');
#endif
	*DISK_DMA_LADDR = dmatmp;

	dmatmp = ((unsigned int) buf) >> 1;	/* word address */
	dmatmp >>= 16;
#ifdef DEBUG
	printf("upper DMA address: ");
	hex4(dmatmp);
	putchar('\n');
#endif

	if ( io == READ )
	    *DISK_DMA_UADDR_R = dmatmp;
	else
	    *DISK_DMA_UADDR_W = dmatmp;

/* Second tell the controller to do the i/o */

	/* this controller does implied seeks */
	*HD_CYHIGH = cyl>>8;
	*HD_CYLOW = cyl;

	*HD_DMA_ENABLE = 0;

#ifdef DEBUG
	printf("Sector %d\n",sector);
#endif
	*HD_SNUM = sector;

#ifdef DEBUG
	printf("SDH ");
	hex2(0x20 | drive<<3 | head&0x07);
	putchar('\n');
#endif
	*HD_SDH = 0x20 | drive<<3 | head&0x07;

	/* single sector reads and writes are done here
	 * for these HD_SCOUNT is ignored.
	 */

	/* using dmatmp as a scratch variable */
	if ( io == READ )
	    dmatmp = (HDC_READ|HDC_IEOC);
	else
	    dmatmp = (HDC_WRITE);

#ifdef DEBUG
	printf("Command to hd controller: ");
	hex2(dmatmp);
	putchar('\n');
#endif

	/* GO for it !! */

	*HD_COMMAND = dmatmp;

	code = hddone()<<8;

	stat = *HD_STATUS;
#ifdef DEBUG
	printf("Done with status: ");
	hex2(stat);
	putchar('\n');
	if ( stat & HDS_MASK ) {
	    printf("io error, cyl %d, head %d, sector %d",cyl,head,sector);
	    hderr("; ",stat);
	    code |= (0x0100 | *HD_EFLAGS&0xff);
	}
#else
	if ( stat & HDS_MASK )
	    code |= (0x0100 | *HD_EFLAGS&0xff);
#endif
	return ( code );
}

trackio (buf,io,cyl,head)
char *buf;
{
	int stat;
	int code;
	register nio;
	register unsigned dmatmp;

	/* first pulse the reset line (groping in the dark).
	 * ?? maybe only do this as recovery from overrun/underrun.
	 */
	*HD_RESET_ON = 0;
	*HD_RESET_OFF = 0;

	/* First, set up the dma */
	*DISK_DMA_DISABLE = 0;
	*DISK_BIU_RESET = 0;

	nio = TSIZE;	/* entire track */
	nio = -(nio>>1);
	*DISK_DMA_COUNT = nio;

	dmatmp = ((unsigned int) buf) >> 1;	/* word address */
	dmatmp &= 0xffff;
#ifdef DEBUG
	printf("lower DMA address: ");
	hex4(dmatmp);
	putchar('\n');
#endif
	*DISK_DMA_LADDR = dmatmp;

	dmatmp = ((unsigned int) buf) >> 1;	/* word address */
	dmatmp >>= 16;
#ifdef DEBUG
	printf("upper DMA address: ");
	hex4(dmatmp);
	putchar('\n');
#endif

	if ( io == READ )
	    *DISK_DMA_UADDR_R = dmatmp;
	else
	    *DISK_DMA_UADDR_W = dmatmp;

/* Second tell the controller to do the i/o */

	/* this controller does implied seeks */
	*HD_CYHIGH = cyl>>8;
	*HD_CYLOW = cyl;

	*HD_DMA_ENABLE = 0;

	/* read or write a whole track */
	*HD_SNUM = 0;
	*HD_SCOUNT = cd->d_nsectors;

#ifdef DEBUG
	printf("SDH ");
	hex2(0x20 | drive<<3 | head&0x07);
	putchar('\n');
#endif
	*HD_SDH = 0x20 | drive<<3 | head&0x07;

	/* using dmatmp as a scratch variable */
	if ( io == READ )
	    dmatmp = (HDC_READ|HDC_IEOC|HDC_MULT);
	else
	    dmatmp = (HDC_WRITE|HDC_MULT);

#ifdef DEBUG
	printf("Command to hd controller: ");
	hex2(dmatmp);
	putchar('\n');
#endif

	/* GO for it !! */

	*HD_COMMAND = dmatmp;

	code = hddone()<<8;

	stat = *HD_STATUS;

#ifdef DEBUG
	printf("Done with status: ");
	hex2(stat);
	putchar('\n');
	if ( stat & HDS_MASK ) {
	    printf("track io error, cyl %d, head %d",cyl,head);
	    printf(",sector %d",*HD_SNUM&0xff);
	    hderr("; ",stat);
	    code |= (0x0100 | *HD_EFLAGS&0xff);
	}
#else
	if ( stat & HDS_MASK )
	    code |= (0x0100 | *HD_EFLAGS&0xff);
#endif
	return ( code );
}

seektest()
{
	int scyl, ecyl;
	char *cp;

	printf("starting cyl (0-%d)? ",cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	scyl = atol(cp);
	if ( scyl < 0 ) scyl = 0;
	if ( scyl > cd->d_ncylinders-1 ) scyl = cd->d_ncylinders-1;

	printf("ending cyl (%d-%d)? ",scyl,cd->d_ncylinders-1);
	cp = prompt( (char *) 0 );
	ecyl = atol(cp);
	if ( ecyl < 0 ) ecyl = 0;
	if ( ecyl > cd->d_ncylinders-1 ) ecyl = cd->d_ncylinders-1;

	while ( scyl <= ecyl ) {
	    printf("Seeking to cylinder %d\n",scyl);
	    hdseek(scyl++);
	    printf("Seeking to cylinder %d\n",ecyl);
	    hdseek(ecyl--);
	}
}

hdseek(cyl)
{
	int stat;


#ifdef DEBUG
	printf("seeking to cylinder %d\n",cyl);
#endif
	*HD_CYHIGH = cyl>>8;
	*HD_CYLOW = cyl;

	*HD_COMMAND = HDC_SEEK|cd->d_steprate;	/* seek */


	(void ) hddone();

#ifdef NEVER
/* I tried this code, inspired by (my memory of) the boot roms, the result
 * running my "seek test" was a horrible noise and the drive locking up
 * until I cycled power - I think I will avoid this (and the pseudo-interrupt
 * seems to work just fine.
 */
/* the boot ROMs check CIP to find out when a seek is done */
	/* wait for seek to start */
	while ( ! (*HD_STATUS & HDS_BUSY) )
	    ;
	while ( *HD_STATUS & HDS_CIP )
	    ;
#endif
	if ( (stat = *HD_STATUS) & HDS_MASK )
	    hderr("Seek error",stat);
}

/* initialize the PIC, this was a real smart thing to do */
picinit()
{
	*PIC_A0 = ICW1;
	*PIC_A1 = ICW2;
	*PIC_A1 = ICW4;

	*PIC_A1 = 0xff;		/* all sources masked */
}

/* This must be long enough for really long seeks - the longest
 * (and slowest) is a restore when the heads were way out at cylinder
 * 800 or such - this can take several seconds.
 * (The restore is done taking one step at a time and waiting for seek
 * complete - the specified step rate is ignored.)
 */
#define PICTIMEOUT	400000		/* was 100000 */

/* note: the "interrupt" is presented only on one read by the 8259
 * when polling like this, you read 0x continuously, then once get
 * 0x8x, then back to 0x as long as you care to keep reading.
 * Also note that on this machine, when reading an 8 bit register
 * on a 16 bit bus, the upper 8 bits gets set to ones, so you get
 * 0xff8x (or 0xff0x, it is not sign extension).
 *
 */
hddone()
{
	long timeout;
	register picstat;
	register code = 0;

#ifdef DEBUG
	int picold = -1;
	printf("begin hddone (wait for IO)\n");
#endif
	*PIC_A1 = 0x9f;	/* allow levels 5 and 6 */
	timeout = PICTIMEOUT;
	while ( timeout-- ) {
	    *PIC_A0 = 0x0e;	/* polling the 8259 */
	    picstat = (*PIC_A0) & 0xff;
#ifdef DEBUG
	    if ( picold == -1 ) {
		printf("PIC status = ");
		hex2(picstat);
		putchar('\n');
		picold = picstat;
	    } else if ( picstat != picold ) {
		printf("PIC status = ");
		hex2(picstat);
		putchar('\n');
		picold = picstat;
	    }
#endif
	    if ( picstat == 0x86 )	/* Winchester EOT */
		break;
	    if ( picstat == 0x85 ) {	/* overrun, underrun */
#ifdef DEBUG
		printf("overrun, underrun\n");
#endif
		code = HDD_OVERRUN;
		break;
	    }
	}
#ifdef DEBUG
	printf("Done with IO wait loop");
	if ( timeout == -1 )
	    printf(" TIMEOUT");
#endif
	if ( timeout == -1 )
	    code = HDD_TIMEOUT;

	*HD_SDH = 0x18;		/* impossible (select no drive) */
	*DISK_DMA_DISABLE = 0;
	*DISK_BIU_RESET = 0;
	*PIC_A1 = 0xff;		/* remask all sources */
#ifdef DEBUG
	putchar('\n');
#endif
	return ( code );
}

hderr(msg,status)
char *msg;
{
	printf("%s, status = ",msg);
	hex2(status);
	printf(", err = ");
	hex2(*HD_EFLAGS);
	putchar('\n');
}

char *
prompt(msg)
char *msg;
{
	static char buf[132];

	if ( msg )
	    printf("%s", msg);
	gets(buf);
	return (buf);
}

/* vxdmp - memory dump, vxworks style */
vxdmp(b,n)
char *b;
{
	register i;
	register j;
	register c;

	for ( i=0; i<n; i+=16,b+=16 ) {
	    hex8(i); printf(":  ");
	    for ( j=0; j<16; j+=2 ) {
		if ( j == 8 )
		    putchar(' ');
		hex2(b[j]); hex2(b[j+1]);
	    }
	    printf(" *");
	    for ( j=0; j<16; j++ ) {
		c = b[j]&0x7f;
		if ( c < ' ' || c > '~' )
		    c = '.';
		putchar(c);
	    }
	    printf("*\n");
	}
}
