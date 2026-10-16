/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Debug output as modem tones on the PC speaker (/DEBUGPORT=BEEP)
 *
 * Each debug line is sent as a frame "PPPP.LL:text*CCCC\r\n" (page, line,
 * CRC-16/CCITT in hex) with asynchronous 8N1 FSK: Bell 202 (1200 baud,
 * mark 1200 Hz, space 2200 Hz) or Bell 103 (300 baud, mark 1270 Hz,
 * space 1070 Hz). After a page, the frame "PPPP.00:END" is sent and the
 * boot waits for a key: Enter or Space - next page, R - send the page
 * again, C - continue without pauses.
 *
 * The square wave is produced by toggling the speaker data bit of port 61h
 * with the timer 2 gate off (SPKR = OUT2 AND bit 1, OUT2 stays high), and
 * timed with the TSC. The TSC rate is measured with the timer 2 one-shot
 * mode at the start of each page.
 *
 * The receiver is sdk/tools/kdbeep/kdbeep_rx.py (a WAV file recorded from
 * the headphone jack or with a microphone).
 */

#ifndef KDBEEP_HOST_TEST
#include <ntoskrnl.h>
#include "kd.h"

#define NDEBUG
#include <debug.h>

CHAR
KdbpTryGetCharKeyboard(PULONG ScanCode, ULONG Retry);
#endif

#if defined(_M_IX86) || defined(_M_AMD64) || defined(KDBEEP_HOST_TEST)

/* GLOBALS *******************************************************************/

#define KDBEEP_PIT_FREQUENCY        1193182
#define KDBEEP_PIT_CHANNEL2_PORT    0x42
#define KDBEEP_PIT_CONTROL_PORT     0x43
/* Channel 2, low then high byte, mode 0 (interrupt on terminal count), binary */
#define KDBEEP_PIT_ONE_SHOT         0xB0
#define KDBEEP_CALIBRATION_HZ       50     /* 20 ms */
#define KDBEEP_CALIBRATION_LOOPS    0x4000000

#define KDBEEP_SPEAKER_PORT         0x61
#define KDBEEP_SPEAKER_GATE2        0x01
#define KDBEEP_SPEAKER_DATA         0x02
#define KDBEEP_SPEAKER_NMI_BITS     0x0C
#define KDBEEP_SPEAKER_OUT2         0x20

#define KDBEEP_PREAMBLE_MS          150
#define KDBEEP_TRAILER_BITS         2
#define KDBEEP_LINE_TEXT_MAX        100
/* "PPPP.LL:" + text + "*CCCC\r\n" */
#define KDBEEP_FRAME_MAX            (8 + KDBEEP_LINE_TEXT_MAX + 7)
#define KDBEEP_PAGE_LINES_MAX       64

/* From <LoaderBlock->LoadOptions>: " .. /DEBUGPORT=BEEP:1200,24,1 .." */
ULONG KdBeepBaudRate = 1200;     // 300 or 1200
ULONG KdBeepPageLines = 24;      // 1 .. 64
BOOLEAN KdBeepPagePause = TRUE;  // wait for a key after each page

typedef struct _KDBEEP_TRANSMITTER
{
    ULONG64 SymbolEnd;
    ULONG64 NextToggle;
    ULONG64 BitTicks;
    ULONG64 HalfPeriod;
    UCHAR PortValue;
} KDBEEP_TRANSMITTER, *PKDBEEP_TRANSMITTER;

static KSPIN_LOCK KdpBeepSpinLock;
static ULONG64 KdpBeepTscPerSecond = 0;
static ULONG KdpBeepMarkFrequency = 1200;
static ULONG KdpBeepSpaceFrequency = 2200;

static CHAR KdpBeepLineText[KDBEEP_LINE_TEXT_MAX];
static ULONG KdpBeepLineLength = 0;

/* The frames of the current page, kept to send them again on request */
static CHAR KdpBeepPageFrames[KDBEEP_PAGE_LINES_MAX][KDBEEP_FRAME_MAX];
static ULONG KdpBeepPageFrameLength[KDBEEP_PAGE_LINES_MAX];
static ULONG KdpBeepPageLineCount = 0;
static ULONG KdpBeepPageNumber = 1;

/* FUNCTIONS *****************************************************************/

/* Measures the TSC rate against a one-shot of the timer 2 */
static BOOLEAN
KdpBeepCalibrate(VOID)
{
    const ULONG Count = KDBEEP_PIT_FREQUENCY / KDBEEP_CALIBRATION_HZ;
    UCHAR SavedValue;
    ULONG64 StartTime, EndTime;
    ULONG LoopCount = 0;
    ULONG_PTR Flags;

    Flags = __readeflags();
    _disable();

    /* Gate on, speaker off */
    SavedValue = __inbyte(KDBEEP_SPEAKER_PORT);
    __outbyte(KDBEEP_SPEAKER_PORT,
              (SavedValue & KDBEEP_SPEAKER_NMI_BITS) | KDBEEP_SPEAKER_GATE2);

    __outbyte(KDBEEP_PIT_CONTROL_PORT, KDBEEP_PIT_ONE_SHOT);
    __outbyte(KDBEEP_PIT_CHANNEL2_PORT, (UCHAR)(Count & 0xFF));
    __outbyte(KDBEEP_PIT_CHANNEL2_PORT, (UCHAR)(Count >> 8));
    StartTime = __rdtsc();

    /* OUT2 goes high at the terminal count */
    while (!(__inbyte(KDBEEP_SPEAKER_PORT) & KDBEEP_SPEAKER_OUT2))
    {
        if (++LoopCount > KDBEEP_CALIBRATION_LOOPS)
            break;
    }
    EndTime = __rdtsc();

    __outbyte(KDBEEP_SPEAKER_PORT, SavedValue & 0x0F);
    __writeeflags(Flags);

    if (LoopCount > KDBEEP_CALIBRATION_LOOPS || EndTime <= StartTime)
        return FALSE;

    KdpBeepTscPerSecond = (EndTime - StartTime) * KDBEEP_CALIBRATION_HZ;
    return TRUE;
}

/* Sends one tone until the end of the symbol, keeping the phase of the wave */
static VOID
KdpBeepSendSymbol(
    _Inout_ PKDBEEP_TRANSMITTER Transmitter,
    _In_ ULONG Frequency,
    _In_ ULONG64 Duration)
{
    ULONG64 HalfPeriod = KdpBeepTscPerSecond / (2 * Frequency);
    ULONG64 SymbolStart = Transmitter->SymbolEnd;
    ULONG64 SymbolEnd = SymbolStart + Duration;
    ULONG64 CurrentTime;

    /* The rest of the current half-cycle goes on at the new frequency,
     * otherwise the tone would change up to a half-cycle late */
    if (HalfPeriod != Transmitter->HalfPeriod)
    {
        if (Transmitter->NextToggle > SymbolStart && Transmitter->HalfPeriod)
        {
            Transmitter->NextToggle = SymbolStart +
                (Transmitter->NextToggle - SymbolStart) * HalfPeriod /
                Transmitter->HalfPeriod;
        }
        Transmitter->HalfPeriod = HalfPeriod;
    }

    while (TRUE)
    {
        CurrentTime = __rdtsc();
        if (CurrentTime >= SymbolEnd)
            break;

        if (CurrentTime >= Transmitter->NextToggle)
        {
            Transmitter->PortValue ^= KDBEEP_SPEAKER_DATA;
            __outbyte(KDBEEP_SPEAKER_PORT, Transmitter->PortValue);

            Transmitter->NextToggle += HalfPeriod;
            /* Do not try to catch up after a long delay (SMI) */
            if (Transmitter->NextToggle < CurrentTime)
                Transmitter->NextToggle = CurrentTime + HalfPeriod;
        }
    }

    Transmitter->SymbolEnd = SymbolEnd;
}

static VOID
KdpBeepSendByte(
    _Inout_ PKDBEEP_TRANSMITTER Transmitter,
    _In_ UCHAR Value)
{
    ULONG BitIndex;
    ULONG64 CurrentTime;
    ULONG_PTR Flags;

    Flags = __readeflags();
    _disable();

    /* An interrupt between bytes only extends the idle (mark) time */
    CurrentTime = __rdtsc();
    if (CurrentTime > Transmitter->SymbolEnd)
        Transmitter->SymbolEnd = CurrentTime;

    /* Start bit, 8 data bits (LSB first), stop bit */
    KdpBeepSendSymbol(Transmitter, KdpBeepSpaceFrequency, Transmitter->BitTicks);
    for (BitIndex = 0; BitIndex < 8; BitIndex++)
    {
        KdpBeepSendSymbol(Transmitter,
                          ((Value >> BitIndex) & 1) ? KdpBeepMarkFrequency
                                                    : KdpBeepSpaceFrequency,
                          Transmitter->BitTicks);
    }
    KdpBeepSendSymbol(Transmitter, KdpBeepMarkFrequency, Transmitter->BitTicks);

    __writeeflags(Flags);
}

static VOID
KdpBeepSendFrame(
    _In_reads_(Length) PCCH Frame,
    _In_ ULONG Length)
{
    KDBEEP_TRANSMITTER Transmitter;
    UCHAR SavedValue;
    ULONG Index;

    SavedValue = __inbyte(KDBEEP_SPEAKER_PORT);

    /* Gate off (OUT2 stays high), the speaker follows the data bit */
    Transmitter.PortValue = SavedValue & KDBEEP_SPEAKER_NMI_BITS;
    __outbyte(KDBEEP_SPEAKER_PORT, Transmitter.PortValue);

    Transmitter.BitTicks = KdpBeepTscPerSecond / KdBeepBaudRate;
    Transmitter.SymbolEnd = __rdtsc();
    Transmitter.NextToggle = Transmitter.SymbolEnd;
    Transmitter.HalfPeriod = 0;

    /* Mark tone for the receiver to find the carrier */
    KdpBeepSendSymbol(&Transmitter, KdpBeepMarkFrequency,
                      KdpBeepTscPerSecond * KDBEEP_PREAMBLE_MS / 1000);

    for (Index = 0; Index < Length; Index++)
        KdpBeepSendByte(&Transmitter, (UCHAR)Frame[Index]);

    KdpBeepSendSymbol(&Transmitter, KdpBeepMarkFrequency,
                      Transmitter.BitTicks * KDBEEP_TRAILER_BITS);

    __outbyte(KDBEEP_SPEAKER_PORT, SavedValue & 0x0F);
}

static USHORT
KdpBeepCrc16(
    _In_reads_(Length) PCCH Data,
    _In_ ULONG Length)
{
    USHORT Crc = 0xFFFF;
    ULONG Index, BitIndex;

    for (Index = 0; Index < Length; Index++)
    {
        Crc ^= (USHORT)((UCHAR)Data[Index]) << 8;
        for (BitIndex = 0; BitIndex < 8; BitIndex++)
            Crc = (Crc & 0x8000) ? (USHORT)((Crc << 1) ^ 0x1021) : (USHORT)(Crc << 1);
    }

    return Crc;
}

static PCHAR
KdpBeepPutDecimal(
    _Out_writes_(Digits) PCHAR Output,
    _In_ ULONG Value,
    _In_ ULONG Digits)
{
    ULONG Index;

    for (Index = Digits; Index > 0; Index--)
    {
        Output[Index - 1] = (CHAR)('0' + Value % 10);
        Value /= 10;
    }

    return Output + Digits;
}

/* Builds "PPPP.LL:text*CCCC\r\n", returns its length */
static ULONG
KdpBeepFormatFrame(
    _Out_writes_(KDBEEP_FRAME_MAX) PCHAR Frame,
    _In_ ULONG PageNumber,
    _In_ ULONG LineNumber,
    _In_reads_(TextLength) PCCH Text,
    _In_ ULONG TextLength)
{
    static const CHAR HexDigits[] = "0123456789ABCDEF";
    PCHAR Output = Frame;
    USHORT Crc;
    ULONG Index;

    Output = KdpBeepPutDecimal(Output, PageNumber % 10000, 4);
    *Output++ = '.';
    Output = KdpBeepPutDecimal(Output, LineNumber, 2);
    *Output++ = ':';
    for (Index = 0; Index < TextLength; Index++)
        *Output++ = Text[Index];

    Crc = KdpBeepCrc16(Frame, (ULONG)(Output - Frame));
    *Output++ = '*';
    for (Index = 0; Index < 4; Index++)
        *Output++ = HexDigits[(Crc >> (12 - 4 * Index)) & 0xF];
    *Output++ = '\r';
    *Output++ = '\n';

    return (ULONG)(Output - Frame);
}

static VOID
KdpBeepSendPage(VOID)
{
    ULONG LineIndex;

    for (LineIndex = 0; LineIndex < KdpBeepPageLineCount; LineIndex++)
    {
        KdpBeepSendFrame(KdpBeepPageFrames[LineIndex],
                         KdpBeepPageFrameLength[LineIndex]);
    }
}

static VOID
KdpBeepEndPage(VOID)
{
    static const CHAR EndText[] = "END";
    CHAR Frame[KDBEEP_FRAME_MAX];
    ULONG FrameLength;
    ULONG ScanCode;
    CHAR Key;

    FrameLength = KdpBeepFormatFrame(Frame, KdpBeepPageNumber, 0,
                                     EndText, sizeof(EndText) - 1);
    KdpBeepSendFrame(Frame, FrameLength);

    if (KdBeepPagePause)
        HalDisplayString("KDBEEP: page sent. Enter - next, R - repeat, C - no pauses\r\n");

    while (KdBeepPagePause)
    {
        Key = KdbpTryGetCharKeyboard(&ScanCode, 0);
        if (Key == '\r' || Key == ' ')
        {
            break;
        }
        else if (Key == 'c' || Key == 'C')
        {
            KdBeepPagePause = FALSE;
        }
        else if (Key == 'r' || Key == 'R')
        {
            KdpBeepSendPage();
            KdpBeepSendFrame(Frame, FrameLength);
        }
    }

    KdpBeepPageNumber++;
    KdpBeepPageLineCount = 0;
}

static VOID
KdpBeepEndLine(VOID)
{
    ULONG LineIndex = KdpBeepPageLineCount;

    /* The CPU clock may differ from the previous page (power state) */
    if (LineIndex == 0)
        KdpBeepCalibrate();

    KdpBeepPageFrameLength[LineIndex] =
        KdpBeepFormatFrame(KdpBeepPageFrames[LineIndex],
                           KdpBeepPageNumber, LineIndex + 1,
                           KdpBeepLineText, KdpBeepLineLength);
    KdpBeepLineLength = 0;

    KdpBeepSendFrame(KdpBeepPageFrames[LineIndex],
                     KdpBeepPageFrameLength[LineIndex]);

    KdpBeepPageLineCount++;
    if (KdpBeepPageLineCount >= KdBeepPageLines)
        KdpBeepEndPage();
}

static VOID
NTAPI
KdpBeepPrint(
    _In_ PCCH String,
    _In_ ULONG Length)
{
    PCCH Current;
    CHAR Character;
    KIRQL OldIrql;

    /* Acquire the printing spinlock without waiting at raised IRQL */
    OldIrql = KdbpAcquireLock(&KdpBeepSpinLock);

    for (Current = String; Current < String + Length && *Current; Current++)
    {
        Character = *Current;

        if (Character == '\n')
        {
            KdpBeepEndLine();
            continue;
        }
        if (Character == '\r')
            continue;

        if (Character == '\t')
            Character = ' ';
        else if ((UCHAR)Character < 0x20 || (UCHAR)Character > 0x7E)
            Character = '?';

        KdpBeepLineText[KdpBeepLineLength++] = Character;

        /* A long line is split */
        if (KdpBeepLineLength >= KDBEEP_LINE_TEXT_MAX)
            KdpBeepEndLine();
    }

    /* Release the spinlock */
    KdbpReleaseLock(&KdpBeepSpinLock, OldIrql);
}

#ifndef KDBEEP_HOST_TEST
NTSTATUS
NTAPI
KdpBeepInit(
    _In_ PKD_DISPATCH_TABLE DispatchTable,
    _In_ ULONG BootPhase)
{
    if (!KdpDebugMode.Beep)
        return STATUS_PORT_DISCONNECTED;

    if (BootPhase == 0)
    {
        if (KdBeepBaudRate == 300)
        {
            /* Bell 103, originate */
            KdpBeepMarkFrequency = 1270;
            KdpBeepSpaceFrequency = 1070;
        }
        else
        {
            /* Bell 202 */
            KdBeepBaudRate = 1200;
            KdpBeepMarkFrequency = 1200;
            KdpBeepSpaceFrequency = 2200;
        }

        if (KdBeepPageLines < 1 || KdBeepPageLines > KDBEEP_PAGE_LINES_MAX)
            KdBeepPageLines = 24;

        if (!KdpBeepCalibrate())
        {
            KdpDebugMode.Beep = FALSE;
            return STATUS_DEVICE_DOES_NOT_EXIST;
        }

        KeInitializeSpinLock(&KdpBeepSpinLock);

        /* Write out the functions that we support for now */
        DispatchTable->KdpPrintRoutine = KdpBeepPrint;

        /* Register for BootPhase 1 initialization and as a Provider */
        DispatchTable->KdpInitRoutine = KdpBeepInit;
        InsertTailList(&KdProviders, &DispatchTable->KdProvidersList);
    }
    else if (BootPhase == 1)
    {
        /* Announce ourselves */
        HalDisplayString(KdBeepBaudRate == 300 ?
                         "   Beep debugging enabled (Bell 103, 300 baud)\r\n" :
                         "   Beep debugging enabled (Bell 202, 1200 baud)\r\n");
    }

    return STATUS_SUCCESS;
}
#endif /* !KDBEEP_HOST_TEST */

#else /* No PC speaker */

ULONG KdBeepBaudRate = 1200;
ULONG KdBeepPageLines = 24;
BOOLEAN KdBeepPagePause = TRUE;

NTSTATUS
NTAPI
KdpBeepInit(
    _In_ PKD_DISPATCH_TABLE DispatchTable,
    _In_ ULONG BootPhase)
{
    UNREFERENCED_PARAMETER(DispatchTable);
    UNREFERENCED_PARAMETER(BootPhase);

    KdpDebugMode.Beep = FALSE;
    return STATUS_NOT_SUPPORTED;
}

#endif

/* EOF */
