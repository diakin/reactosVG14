/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Host test of ntoskrnl/kd/kdbeep.c: runs the transmitter
 *              against a simulated TSC, timer 2 and port 61h, and writes
 *              the speaker signal to a WAV file for kdbeep_rx.py.
 *
 * Build:  gcc -O2 -o kdbeep_host_test kdbeep_host_test.c
 * Run:    kdbeep_host_test out.wav [baud [lines [noise_percent [input.txt]]]]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define KDBEEP_HOST_TEST

typedef unsigned char UCHAR, BOOLEAN;
typedef char CHAR, *PCHAR;
typedef const char *PCCH;
typedef unsigned short USHORT;
typedef unsigned int ULONG, *PULONG;
typedef unsigned long long ULONG64;
typedef uintptr_t ULONG_PTR, KSPIN_LOCK, *PKSPIN_LOCK;
typedef UCHAR KIRQL;
#define VOID void
#define NTAPI
#define TRUE 1
#define FALSE 0
#define _In_
#define _Inout_
#define _In_reads_(x)
#define _Out_writes_(x)

/* Simulated machine *********************************************************/

#define SIM_TSC_PER_SECOND   1600000000ULL  /* Core Duo T2050 */
#define SIM_RDTSC_COST       40             /* 25 ns */
#define SIM_PORT_COST        1600           /* 1 us */
#define SIM_PIT_FREQUENCY    1193182ULL

static ULONG64 SimTsc;
static UCHAR SimPort61;
static ULONG64 SimOneShotEnd;
static ULONG SimPitCount, SimPitBytes;

typedef struct _SIM_EDGE
{
    ULONG64 Time;
    UCHAR Level;
} SIM_EDGE;

static SIM_EDGE *SimEdges;
static size_t SimEdgeCount, SimEdgeCapacity;

static void
SimAddEdge(UCHAR Level)
{
    if (SimEdgeCount == SimEdgeCapacity)
    {
        SimEdgeCapacity = SimEdgeCapacity ? SimEdgeCapacity * 2 : 65536;
        SimEdges = realloc(SimEdges, SimEdgeCapacity * sizeof(SIM_EDGE));
    }
    SimEdges[SimEdgeCount].Time = SimTsc;
    SimEdges[SimEdgeCount].Level = Level;
    SimEdgeCount++;
}

static ULONG64 __rdtsc(void)
{
    SimTsc += SIM_RDTSC_COST;
    return SimTsc;
}

static UCHAR __inbyte(unsigned short Port)
{
    SimTsc += SIM_PORT_COST;
    if (Port == 0x61)
        return SimPort61 | ((SimTsc >= SimOneShotEnd) ? 0x20 : 0);
    return 0xFF;
}

static void __outbyte(unsigned short Port, UCHAR Value)
{
    SimTsc += SIM_PORT_COST;
    if (Port == 0x61)
    {
        if ((Value ^ SimPort61) & 0x02)
            SimAddEdge((Value >> 1) & 1);
        SimPort61 = Value & 0x0F;
    }
    else if (Port == 0x43)
    {
        SimPitBytes = 0;
        SimPitCount = 0;
        SimOneShotEnd = ~0ULL;
    }
    else if (Port == 0x42)
    {
        SimPitCount |= (ULONG)Value << (8 * SimPitBytes);
        if (++SimPitBytes == 2)
            SimOneShotEnd = SimTsc + SimPitCount * SIM_TSC_PER_SECOND / SIM_PIT_FREQUENCY;
    }
}

static ULONG_PTR __readeflags(void) { return 0x200; }
static void __writeeflags(ULONG_PTR Flags) { (void)Flags; }
static void _disable(void) { }

static KIRQL KdbpAcquireLock(PKSPIN_LOCK SpinLock) { (void)SpinLock; return 0; }
static void KdbpReleaseLock(PKSPIN_LOCK SpinLock, KIRQL OldIrql) { (void)SpinLock; (void)OldIrql; }
static void HalDisplayString(PCCH String) { fputs(String, stderr); }

static const char *SimKeys = "R\r";  /* repeat the first page, then next */
static CHAR KdbpTryGetCharKeyboard(PULONG ScanCode, ULONG Retry)
{
    (void)Retry;
    *ScanCode = 0;
    SimTsc += SIM_TSC_PER_SECOND;  /* a second to press the key */
    if (*SimKeys)
        return *SimKeys++;
    return '\r';
}

#include "../../../ntoskrnl/kd/kdbeep.c"

/* WAV output ****************************************************************/

static void
WriteLittle(FILE *File, ULONG Value, int ByteCount)
{
    int Index;
    for (Index = 0; Index < ByteCount; Index++)
        fputc((Value >> (8 * Index)) & 0xFF, File);
}

static void
WriteWav(const char *Path, ULONG SampleRate, double NoiseLevel)
{
    ULONG64 EndTime = SimTsc + SIM_TSC_PER_SECOND / 2;
    ULONG SampleCount = (ULONG)(EndTime * SampleRate / SIM_TSC_PER_SECOND);
    FILE *File = fopen(Path, "wb");
    size_t EdgeIndex = 0;
    double Level = 0, Previous = 0, Filtered = 0;
    ULONG SampleIndex;
    int Value;

    WriteLittle(File, 0x46464952, 4);  /* RIFF */
    WriteLittle(File, 36 + SampleCount * 2, 4);
    WriteLittle(File, 0x45564157, 4);  /* WAVE */
    WriteLittle(File, 0x20746D66, 4);  /* fmt  */
    WriteLittle(File, 16, 4);
    WriteLittle(File, 1, 2);
    WriteLittle(File, 1, 2);
    WriteLittle(File, SampleRate, 4);
    WriteLittle(File, SampleRate * 2, 4);
    WriteLittle(File, 2, 2);
    WriteLittle(File, 16, 2);
    WriteLittle(File, 0x61746164, 4);  /* data */
    WriteLittle(File, SampleCount * 2, 4);

    srand(1);
    for (SampleIndex = 0; SampleIndex < SampleCount; SampleIndex++)
    {
        ULONG64 Time = (ULONG64)SampleIndex * SIM_TSC_PER_SECOND / SampleRate;
        while (EdgeIndex < SimEdgeCount && SimEdges[EdgeIndex].Time <= Time)
        {
            Level = SimEdges[EdgeIndex].Level ? 1.0 : -1.0;
            EdgeIndex++;
        }
        /* AC coupling of the audio path */
        Filtered = 0.995 * (Filtered + Level - Previous);
        Previous = Level;
        Value = (int)(12000 * (Filtered + NoiseLevel * ((double)rand() / RAND_MAX * 2 - 1)));
        if (Value > 32767) Value = 32767;
        if (Value < -32768) Value = -32768;
        WriteLittle(File, (ULONG)(Value & 0xFFFF), 2);
    }

    fclose(File);
}

int
main(int argc, char **argv)
{
    static const char DefaultLog[] =
        "(ntoskrnl/ke/i386/cpu.c:123) Supported CPU features: 0x1234\n"
        "Booting ReactOS 0.4.14-vgal-2\n"
        "\tTab and \x01 control characters\n"
        "\n"
        "(ntoskrnl/io/pnpmgr/pnpinit.c:2605) IopMarkBootPartition\n"
        "A very long line that is longer than one hundred characters so that it is split "
        "into two frames by the transmitter of the kernel\n"
        "*** STOP: 0x0000007B (0xF789A63C,0xC0000034,0x00000000,0x00000000)\n";
    const char *LogText = DefaultLog;
    char *FileText = NULL;
    double NoiseLevel;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s out.wav [baud [lines [noise_percent [input.txt]]]]\n", argv[0]);
        return 1;
    }

    KdBeepBaudRate = (argc > 2) ? (ULONG)atoi(argv[2]) : 1200;
    KdBeepPageLines = (argc > 3) ? (ULONG)atoi(argv[3]) : 4;
    NoiseLevel = (argc > 4) ? atof(argv[4]) / 100.0 : 0.0;
    if (argc > 5)
    {
        FILE *Input = fopen(argv[5], "rb");
        long Size;
        fseek(Input, 0, SEEK_END);
        Size = ftell(Input);
        fseek(Input, 0, SEEK_SET);
        FileText = calloc(1, Size + 1);
        if (fread(FileText, 1, Size, Input) != (size_t)Size)
            return 1;
        fclose(Input);
        LogText = FileText;
    }

    if (KdBeepBaudRate == 300)
    {
        KdpBeepMarkFrequency = 1270;
        KdpBeepSpaceFrequency = 1070;
    }

    /* Silence before the first frame */
    SimTsc = SIM_TSC_PER_SECOND / 4;
    if (!KdpBeepCalibrate())
    {
        fprintf(stderr, "calibration failed\n");
        return 1;
    }
    fprintf(stderr, "calibrated TSC: %llu Hz (simulated %llu Hz)\n",
            KdpBeepTscPerSecond, SIM_TSC_PER_SECOND);

    /* Print in pieces, with boot work between them */
    while (*LogText)
    {
        size_t Length = strcspn(LogText, "\n");
        if (LogText[Length] == '\n')
            Length++;
        KdpBeepPrint(LogText, (ULONG)Length);
        LogText += Length;
        SimTsc += SIM_TSC_PER_SECOND / 20;
    }

    fprintf(stderr, "%zu edges, %.1f s\n", SimEdgeCount, (double)SimTsc / SIM_TSC_PER_SECOND);
    WriteWav(argv[1], 48000, NoiseLevel);
    free(FileText);
    return 0;
}
