/*
 * PROJECT:     FreeLoader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Saves the kernel RAM debug log (/DEBUGPORT=RAM) to a file
 *              on the boot partition after a warm reset.
 */

#include <freeldr.h>
#include <reactos/kdramlog.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(DISK);

#define TAG_RAMLOG 'gLmR'

typedef struct _RAMLOG_WRITE_CONTEXT
{
    UCHAR DriveNumber;
    ULONGLONG PartitionStartSector;
    PUCHAR Data;
    ULONG DataSectors;
    ULONG SectorsWritten;
} RAMLOG_WRITE_CONTEXT, *PRAMLOG_WRITE_CONTEXT;

/* Returns the byte at a position of the log, counted from its oldest byte */
static CHAR
PcRamLogGetChar(
    IN volatile KDRAMLOG_HEADER* Header,
    IN ULONG Index)
{
    volatile CHAR* Data = (volatile CHAR*)(Header + 1);
    ULONG Start = Header->Wrapped ? Header->WritePosition : 0;

    return Data[(Start + Index) % Header->BufferSize];
}

/* Number of bytes a log character takes in the file: NUL bytes are
 * dropped and a bare LF becomes CR LF, for Windows text viewers. */
static ULONG
PcRamLogCharCost(
    IN volatile KDRAMLOG_HEADER* Header,
    IN ULONG Index)
{
    CHAR Current = PcRamLogGetChar(Header, Index);

    if (Current == ANSI_NULL)
        return 0;
    if (Current == '\n' && (Index == 0 || PcRamLogGetChar(Header, Index - 1) != '\r'))
        return 2;
    return 1;
}

/* Formats the log into Text. When it does not fit, the oldest part is dropped. */
static ULONG
PcRamLogFormat(
    IN volatile KDRAMLOG_HEADER* Header,
    OUT PCHAR Text,
    IN ULONG TextSize)
{
    ULONG LogLength, FirstIndex, Index, Used, Cost, Available;
    BOOLEAN Truncated;
    CHAR Current;

    LogLength = Header->Wrapped ? Header->BufferSize : Header->WritePosition;
    Truncated = (Header->Wrapped != 0);

    /* Leave room for the title line and a final line break */
    RtlStringCbPrintfA(Text, TextSize, "===== RAM debug log saved by FreeLoader =====\r\n");
    Used = (ULONG)strlen(Text);
    Available = TextSize - Used - 2;

    /* Find the oldest character that still fits, walking from the newest */
    FirstIndex = LogLength;
    Cost = 0;
    while (FirstIndex > 0)
    {
        ULONG CharCost = PcRamLogCharCost(Header, FirstIndex - 1);
        if (Cost + CharCost > Available)
        {
            Truncated = TRUE;
            break;
        }
        Cost += CharCost;
        FirstIndex--;
    }

    if (Truncated)
    {
        static const CHAR LostMarker[] = "[... older messages lost ...]\r\n";
        if (Available - Cost >= sizeof(LostMarker) - 1)
        {
            RtlCopyMemory(Text + Used, LostMarker, sizeof(LostMarker) - 1);
            Used += sizeof(LostMarker) - 1;
        }
    }

    for (Index = FirstIndex; Index < LogLength; Index++)
    {
        Current = PcRamLogGetChar(Header, Index);
        if (Current == ANSI_NULL)
            continue;
        if (PcRamLogCharCost(Header, Index) == 2)
            Text[Used++] = '\r';
        Text[Used++] = Current;
    }

    Text[Used++] = '\r';
    Text[Used++] = '\n';
    return Used;
}

static BOOLEAN
PcRamLogWriteRun(
    IN ULONG SectorNumber,
    IN ULONG SectorCount,
    IN PVOID Context)
{
    PRAMLOG_WRITE_CONTEXT WriteContext = Context;
    ULONG MaxSectors = (ULONG)(DiskReadBufferSize / SECTOR_SIZE);
    ULONGLONG DiskSector = WriteContext->PartitionStartSector + SectorNumber;
    ULONG ChunkSectors;

    while (SectorCount > 0 && WriteContext->SectorsWritten < WriteContext->DataSectors)
    {
        ChunkSectors = min(SectorCount, MaxSectors);
        ChunkSectors = min(ChunkSectors, WriteContext->DataSectors - WriteContext->SectorsWritten);

        /* The BIOS can only transfer from memory below 1 MB */
        RtlCopyMemory(DiskReadBuffer,
                      WriteContext->Data + WriteContext->SectorsWritten * SECTOR_SIZE,
                      ChunkSectors * SECTOR_SIZE);

        if (!PcDiskWriteLogicalSectors(WriteContext->DriveNumber,
                                       DiskSector,
                                       ChunkSectors,
                                       DiskReadBuffer))
        {
            return FALSE;
        }

        DiskSector += ChunkSectors;
        SectorCount -= ChunkSectors;
        WriteContext->SectorsWritten += ChunkSectors;
    }

    return TRUE;
}

/*
 * If the previous boot left a kernel RAM debug log in the reserved region,
 * writes it over the contents of RAMLOG.TXT in the root of the boot
 * partition. The file must exist and be large enough; its size is not
 * changed, the unused part is filled with spaces.
 */
VOID
PcRamLogSave(VOID)
{
    volatile KDRAMLOG_HEADER* Header = (volatile KDRAMLOG_HEADER*)KDRAMLOG_PHYSICAL_ADDRESS;
    CHAR FilePath[MAX_PATH];
    FILEINFORMATION FileInformation;
    RAMLOG_WRITE_CONTEXT WriteContext;
    ULONG FileId, FileSize, BufferSize, TextLength;
    PCHAR Text;
    ARC_STATUS Status;

    if (!PcRamLogRegionReserved || !KdRamLogIsHeaderValid(Header))
        return;

    if (FrldrBootDrive < 0x80 || FrldrBootPartition == 0xFF)
    {
        UiMessageBox("A kernel RAM debug log was found in memory,\n"
                     "but it can be saved only to a hard disk or USB disk boot partition.");
        return;
    }

    RtlStringCbPrintfA(FilePath, sizeof(FilePath), "%s\\%s", FrLdrBootPath, KDRAMLOG_FILE_NAME);

    Status = ArcOpen(FilePath, OpenReadOnly, &FileId);
    if (Status != ESUCCESS)
    {
        UiMessageBox("A kernel RAM debug log was found in memory,\n"
                     "but %s does not exist in the root of the boot partition.\n"
                     "Create this file (1 MB or more) to save the log.",
                     KDRAMLOG_FILE_NAME);
        return;
    }

    Status = ArcGetFileInformation(FileId, &FileInformation);
    FileSize = FileInformation.EndingAddress.LowPart;
    if (Status != ESUCCESS || FileSize < 4096)
    {
        ArcClose(FileId);
        UiMessageBox("%s is too small to hold the kernel RAM debug log.", KDRAMLOG_FILE_NAME);
        return;
    }

    /* Get the start of the partition, to turn volume sectors into disk sectors */
    Status = ArcGetFileInformation(FsGetDeviceId(FileId), &FileInformation);
    if (Status != ESUCCESS)
    {
        ArcClose(FileId);
        return;
    }

    BufferSize = ALIGN_UP_BY(FileSize, SECTOR_SIZE);
    Text = FrLdrTempAlloc(BufferSize, TAG_RAMLOG);
    if (!Text)
    {
        ArcClose(FileId);
        return;
    }

    TextLength = PcRamLogFormat(Header, Text, FileSize);
    RtlFillMemory(Text + TextLength, BufferSize - TextLength, ' ');

    WriteContext.DriveNumber = FrldrBootDrive;
    WriteContext.PartitionStartSector = FileInformation.StartingAddress.QuadPart / SECTOR_SIZE;
    WriteContext.Data = (PUCHAR)Text;
    WriteContext.DataSectors = BufferSize / SECTOR_SIZE;
    WriteContext.SectorsWritten = 0;

    Status = FatEnumFileSectors(FileId, PcRamLogWriteRun, &WriteContext);

    FrLdrTempFree(Text, TAG_RAMLOG);
    ArcClose(FileId);

    if (Status != ESUCCESS)
    {
        ERR("FatEnumFileSectors() failed: %lu (%lu sectors written)\n",
            Status, WriteContext.SectorsWritten);
        UiMessageBox("Failed to save the kernel RAM debug log to %s (error %lu).\n"
                     "The boot partition must be FAT and the disk writable.",
                     KDRAMLOG_FILE_NAME, Status);
        return;
    }

    /* Do not save the same log again on the next boot */
    Header->Signature = 0;

    UiMessageBox("The kernel RAM debug log was saved to %s\n"
                 "in the root of the boot partition (%lu bytes).",
                 KDRAMLOG_FILE_NAME, TextLength);
}
