/*
 * PROJECT:     ReactOS Kernel / FreeLoader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Debug log kept in reserved physical memory (/DEBUGPORT=RAM).
 *              The kernel writes debug messages into this region, and
 *              FreeLoader saves it to a file on the boot partition on the
 *              next boot after a warm reset.
 */

#pragma once

/* Fixed physical location of the region. FreeLoader reserves it as
 * LoaderSpecialMemory, and the kernel uses it only if it finds this
 * reservation in the loader block memory descriptors. */
#define KDRAMLOG_PHYSICAL_ADDRESS   0x00E00000
#define KDRAMLOG_REGION_SIZE        0x00100000

#define KDRAMLOG_SIGNATURE          0x474C4D52 /* 'RMLG' */

/* Name of the file on the boot partition that receives the log.
 * It must already exist: FreeLoader overwrites its contents but
 * does not change its size. */
#define KDRAMLOG_FILE_NAME          "RAMLOG.TXT"

typedef struct _KDRAMLOG_HEADER
{
    ULONG Signature;
    ULONG HeaderSize;    /* sizeof(KDRAMLOG_HEADER) */
    ULONG BufferSize;    /* Size of the data area following the header */
    ULONG WritePosition; /* Offset in the data area of the next write */
    ULONG Wrapped;       /* Non-zero once the data area has wrapped */
    ULONG Reserved[3];
} KDRAMLOG_HEADER, *PKDRAMLOG_HEADER;

#define KDRAMLOG_BUFFER_SIZE (KDRAMLOG_REGION_SIZE - sizeof(KDRAMLOG_HEADER))

FORCEINLINE
BOOLEAN
KdRamLogIsHeaderValid(
    _In_ volatile KDRAMLOG_HEADER* Header)
{
    return (Header->Signature == KDRAMLOG_SIGNATURE &&
            Header->HeaderSize == sizeof(KDRAMLOG_HEADER) &&
            Header->BufferSize == KDRAMLOG_BUFFER_SIZE &&
            Header->WritePosition < KDRAMLOG_BUFFER_SIZE);
}
