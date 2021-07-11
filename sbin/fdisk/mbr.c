/*	$OpenBSD: mbr.c,v 1.80 2021/07/11 13:38:27 krw Exp $	*/

/*
 * Copyright (c) 1997 Tobias Weingartner
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>	/* DEV_BSIZE */
#include <sys/ioctl.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk.h"
#include "part.h"
#include "misc.h"
#include "mbr.h"
#include "gpt.h"

struct mbr		initial_mbr;

static int		gpt_chk_mbr(struct dos_partition *, uint64_t);

int
MBR_protective_mbr(struct mbr *mbr)
{
	struct dos_partition	dp[NDOSPART], dos_partition;
	int			i;

	if (mbr->offset != 0)
		return -1;

	for (i = 0; i < NDOSPART; i++) {
		PRT_make(&mbr->part[i], mbr->offset, mbr->reloffset,
		    &dos_partition);
		memcpy(&dp[i], &dos_partition, sizeof(dp[i]));
	}

	return gpt_chk_mbr(dp, DL_GETDSIZE(&dl));
}

void
MBR_init_GPT(struct mbr *mbr)
{
	memset(&mbr->part, 0, sizeof(mbr->part));

	/* Use whole disk, starting after MBR.
	 *
	 * Always set the partition size to UINT32_MAX (as MS does). EFI
	 * firmware has been encountered that lies in unpredictable ways
	 * about the size of the disk, thus making it impossible to boot
	 * such devices.
	 */
	mbr->part[0].id = DOSPTYP_EFI;
	mbr->part[0].bs = 1;
	mbr->part[0].ns = UINT32_MAX;

	/* Fix up start/length fields. */
	PRT_fix_CHS(&mbr->part[0]);
}

void
MBR_init(struct mbr *mbr)
{
	extern uint32_t		b_sectors, b_offset;
	extern uint8_t		b_type;
	uint64_t		adj;
	daddr_t			daddr;

	memset(&gh, 0, sizeof(gh));
	memset(&gp, 0, sizeof(gp));

	/*
	 * XXX Do *NOT* zap all MBR parts! Some archs still read initmbr
	 * from disk!! Just mark them inactive until -b goodness spreads
	 * further.
	 */
	mbr->part[0].flag = 0;
	mbr->part[1].flag = 0;
	mbr->part[2].flag = 0;

	mbr->part[3].flag = DOSACTIVE;
	mbr->signature = DOSMBR_SIGNATURE;

	/* Use whole disk. Reserve first track, or first cyl, if possible. */
	mbr->part[3].id = DOSPTYP_OPENBSD;
	if (disk.heads > 1)
		mbr->part[3].shead = 1;
	else
		mbr->part[3].shead = 0;
	if (disk.heads < 2 && disk.cylinders > 1)
		mbr->part[3].scyl = 1;
	else
		mbr->part[3].scyl = 0;
	mbr->part[3].ssect = 1;

	/* Go right to the end */
	mbr->part[3].ecyl = disk.cylinders - 1;
	mbr->part[3].ehead = disk.heads - 1;
	mbr->part[3].esect = disk.sectors;

	/* Fix up start/length fields */
	PRT_fix_BN(&mbr->part[3], 3);

#if defined(__powerpc__) || defined(__mips__)
	/* Now fix up for the MS-DOS boot partition on PowerPC. */
	mbr->part[0].flag = DOSACTIVE;	/* Boot from dos part */
	mbr->part[3].flag = 0;
	mbr->part[3].ns += mbr->part[3].bs;
	mbr->part[3].bs = mbr->part[0].bs + mbr->part[0].ns;
	mbr->part[3].ns -= mbr->part[3].bs;
	PRT_fix_CHS(&mbr->part[3]);
	if ((mbr->part[3].shead != 1) || (mbr->part[3].ssect != 1)) {
		/* align the partition on a cylinder boundary */
		mbr->part[3].shead = 0;
		mbr->part[3].ssect = 1;
		mbr->part[3].scyl += 1;
	}
	/* Fix up start/length fields */
	PRT_fix_BN(&mbr->part[3], 3);
#else
	if (b_sectors > 0) {
		mbr->part[0].id = b_type;
		mbr->part[0].bs = b_offset;
		mbr->part[0].ns = b_sectors;
		PRT_fix_CHS(&mbr->part[0]);
		mbr->part[3].ns += mbr->part[3].bs;
		mbr->part[3].bs = mbr->part[0].bs + mbr->part[0].ns;
		mbr->part[3].ns -= mbr->part[3].bs;
		PRT_fix_CHS(&mbr->part[3]);
	}
#endif

	/* Start OpenBSD MBR partition on a power of 2 block number. */
	daddr = 1;
	while (daddr < DL_SECTOBLK(&dl, mbr->part[3].bs))
		daddr *= 2;
	adj = DL_BLKTOSEC(&dl, daddr) - mbr->part[3].bs;
	mbr->part[3].bs += adj;
	mbr->part[3].ns -= adj;
	PRT_fix_CHS(&mbr->part[3]);
}

void
MBR_parse(struct dos_mbr *dos_mbr, off_t offset, off_t reloff, struct mbr *mbr)
{
	struct dos_partition	dos_parts[NDOSPART];
	int			i;

	memcpy(mbr->code, dos_mbr->dmbr_boot, sizeof(mbr->code));
	mbr->offset = offset;
	mbr->reloffset = reloff;
	mbr->signature = letoh16(dos_mbr->dmbr_sign);

	memcpy(dos_parts, dos_mbr->dmbr_parts, sizeof(dos_parts));

	for (i = 0; i < NDOSPART; i++)
		PRT_parse(&dos_parts[i], offset, reloff, &mbr->part[i]);
}

void
MBR_make(struct mbr *mbr, struct dos_mbr *dos_mbr)
{
	struct dos_partition	dos_partition;
	int			i;

	memcpy(dos_mbr->dmbr_boot, mbr->code, sizeof(dos_mbr->dmbr_boot));
	dos_mbr->dmbr_sign = htole16(DOSMBR_SIGNATURE);

	for (i = 0; i < NDOSPART; i++) {
		PRT_make(&mbr->part[i], mbr->offset, mbr->reloffset,
		    &dos_partition);
		memcpy(&dos_mbr->dmbr_parts[i], &dos_partition,
		    sizeof(dos_mbr->dmbr_parts[i]));
	}
}

void
MBR_print(struct mbr *mbr, char *units)
{
	int			i;

	DISK_printgeometry(NULL);

	/* Header */
	printf("Offset: %lld\t", (long long)mbr->offset);
	printf("Signature: 0x%X\n", (int)mbr->signature);
	PRT_print(0, NULL, units);

	/* Entries */
	for (i = 0; i < NDOSPART; i++)
		PRT_print(i, &mbr->part[i], units);
}

int
MBR_read(off_t where, struct dos_mbr *dos_mbr)
{
	char			*secbuf;

	secbuf = DISK_readsector(where);
	if (secbuf == NULL)
		return -1;

	memcpy(dos_mbr, secbuf, sizeof(*dos_mbr));
	free(secbuf);

	return 0;
}

int
MBR_write(off_t where, struct dos_mbr *dos_mbr)
{
	char			*secbuf;

	secbuf = DISK_readsector(where);
	if (secbuf == NULL)
		return -1;

	/*
	 * Place the new MBR at the start of the sector and
	 * write the sector back to "disk".
	 */
	memcpy(secbuf, dos_mbr, sizeof(*dos_mbr));
	DISK_writesector(secbuf, where);

	/* Refresh in-kernel disklabel from the updated disk information. */
	ioctl(disk.fd, DIOCRLDINFO, 0);

	free(secbuf);

	return 0;
}

/*
 * Return the index into dp[] of the EFI GPT (0xEE) partition, or -1 if no such
 * partition exists.
 *
 * Taken from kern/subr_disk.c.
 *
 */
int
gpt_chk_mbr(struct dos_partition *dp, u_int64_t dsize)
{
	struct dos_partition	*dp2;
	int			 efi, eficnt, found, i;
	uint32_t		 psize;

	found = efi = eficnt = 0;
	for (dp2 = dp, i = 0; i < NDOSPART; i++, dp2++) {
		if (dp2->dp_typ == DOSPTYP_UNUSED)
			continue;
		found++;
		if (dp2->dp_typ != DOSPTYP_EFI)
			continue;
		if (letoh32(dp2->dp_start) != GPTSECTOR)
			continue;
		psize = letoh32(dp2->dp_size);
		if (psize <= (dsize - GPTSECTOR) || psize == UINT32_MAX) {
			efi = i;
			eficnt++;
		}
	}
	if (found == 1 && eficnt == 1)
		return efi;

	return -1;
}
