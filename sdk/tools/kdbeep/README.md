# Debug output as modem tones (/DEBUGPORT=BEEP)

For machines without a serial port or network. The kernel sends every debug
line to the PC speaker as an FSK modem signal, and `kdbeep_rx.py` decodes a
recording of it.

## Boot option

```
/DEBUGPORT=BEEP[:baud[,lines[,pause]]]
```

- `baud`: `1200` (Bell 202, 1200/2200 Hz, default) or `300` (Bell 103,
  1270/1070 Hz, for a recording made with a microphone).
- `lines`: lines per page, 1..64, default 24.
- `pause`: `1` (default) waits for a key after each page, `0` does not wait.

Example: `/DEBUGPORT=BEEP:300,20,1`. It can be combined with other ports,
e.g. `/DEBUGPORT=SCREEN /DEBUGPORT=BEEP`.

After a page the boot stops and waits for a key on the PS/2 keyboard:

- Enter or Space: next page;
- R: send the page again (if a line was not received);
- C: continue without pauses.

A line of 80 characters takes about 1 s at 1200 baud and 3.5 s at 300 baud
(calculated), and the boot waits while it is sent.

## Recording

Record the headphone jack into a line input, or the laptop speaker with a
microphone (phone, voice recorder), as a WAV file; 44.1 or 48 kHz, mono or
stereo, 16 bit. Whether the speaker signal reaches the headphone jack
depends on the board: check by listening to the BIOS beep with headphones.

## Decoding

```
python kdbeep_rx.py recording.wav            # baud rate found automatically
python kdbeep_rx.py recording.wav --baud 300
python kdbeep_rx.py recording.wav --raw      # bytes without frame checks
```

Python 3.4 or later, no extra modules (3.8 is the last version for
Windows 7). The output lists the pages; lines with a bad CRC are shown as
missing, then the page can be sent again with R.

Frame format: `PPPP.LL:text*CCCC\r\n`, page and line in decimal, CRC-16/CCITT
(0xFFFF, poly 0x1021) of `PPPP.LL:text` in hex. Line `00` with the text `END`
ends a page.

## Host test

`kdbeep_host_test.c` builds `ntoskrnl/kd/kdbeep.c` with a simulated TSC,
timer 2 and port 61h, and writes the speaker signal to a WAV file:

```
gcc -O2 -o kdbeep_host_test kdbeep_host_test.c -lm
./kdbeep_host_test test.wav 1200 4 30     # baud, lines per page, noise %
python3 kdbeep_rx.py test.wav
```
