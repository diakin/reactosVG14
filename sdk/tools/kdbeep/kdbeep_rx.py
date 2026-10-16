#!/usr/bin/env python3
#
# PROJECT:     ReactOS Kernel
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Decodes the /DEBUGPORT=BEEP debug output from a WAV recording
#
# Usage: kdbeep_rx.py recording.wav [--baud 1200|300|auto] [--raw]
#
# The recording is any PCM WAV file (8/16/24/32 bit, mono or stereo).
# Frames are "PPPP.LL:text*CCCC" with a CRC-16/CCITT of "PPPP.LL:text".
# Pure Python, no extra modules; works with Python 3.4 and later
# (3.8 is the last version for Windows 7).

import argparse
import array
import cmath
import math
import re
import sys
import wave

MODEMS = {
    1200: (1200.0, 2200.0),  # Bell 202: mark, space
    300: (1270.0, 1070.0),   # Bell 103 originate: mark, space
}

FRAME_PATTERN = re.compile(r'^(\d{4})\.(\d{2}):(.*)\*([0-9A-F]{4})$')


def read_wav(path):
    with wave.open(path, 'rb') as wav_file:
        channel_count = wav_file.getnchannels()
        sample_width = wav_file.getsampwidth()
        sample_rate = wav_file.getframerate()
        raw_data = wav_file.readframes(wav_file.getnframes())

    if sample_width == 1:
        values = [byte_value - 128 for byte_value in raw_data]
    elif sample_width == 2:
        values = array.array('h')
        values.frombytes(raw_data)
        if sys.byteorder == 'big':
            values.byteswap()
    elif sample_width == 3:
        values = [int.from_bytes(raw_data[index:index + 3], 'little', signed=True)
                  for index in range(0, len(raw_data), 3)]
    elif sample_width == 4:
        values = array.array('i')
        values.frombytes(raw_data)
        if sys.byteorder == 'big':
            values.byteswap()
    else:
        raise ValueError('unsupported sample width %d' % sample_width)

    # Only the first channel
    samples = array.array('f', values[0::channel_count])
    return samples, sample_rate


def tone_energies(samples, sample_rate, frequency, window):
    """Energy of one tone over a sliding window ending at each sample."""
    step = cmath.exp(-2j * math.pi * frequency / sample_rate)
    phasor = 1 + 0j
    history = [0j] * window
    window_sum = 0j
    energies = array.array('f', bytes(4 * len(samples)))
    for index, sample in enumerate(samples):
        product = sample * phasor
        slot = index % window
        window_sum += product - history[slot]
        history[slot] = product
        energies[index] = window_sum.real * window_sum.real + window_sum.imag * window_sum.imag
        phasor *= step
        if slot == 0:
            # Keep the rotating phasor on the unit circle
            phasor /= abs(phasor)
    return energies


def percentile(values, fraction):
    subset = sorted(values[0::max(1, len(values) // 20000)])
    return subset[min(len(subset) - 1, int(len(subset) * fraction))]


def demodulate(samples, sample_rate, baud_rate):
    """Returns the bytes found by an asynchronous 8N1 receiver."""
    mark_frequency, space_frequency = MODEMS[baud_rate]
    bit_length = int(round(sample_rate / baud_rate))
    mark_energy = tone_energies(samples, sample_rate, mark_frequency, bit_length)
    space_energy = tone_energies(samples, sample_rate, space_frequency, bit_length)

    total_energy = [mark + space for mark, space in zip(mark_energy, space_energy)]
    carrier_threshold = 0.02 * percentile(total_energy, 0.999)

    def is_carrier(index):
        return total_energy[index] > carrier_threshold

    def is_mark(index):
        return mark_energy[index] > space_energy[index]

    output = bytearray()
    sample_count = len(samples)
    index = bit_length
    while index < sample_count:
        # A start bit: mark followed by space, with a carrier
        if not (is_carrier(index) and not is_mark(index) and is_mark(index - 1)):
            index += 1
            continue

        # The window is half inside the start bit here
        start_time = index - bit_length // 2

        # The whole bit before it is mark (a stop bit or idle)
        if not (is_carrier(start_time - 1) and is_mark(start_time - 1)):
            index += 1
            continue
        last_sample = start_time + 10 * bit_length - 1
        if last_sample >= sample_count:
            break

        def bit_sample(bit_index):
            return start_time + (bit_index + 1) * bit_length - 1

        if is_mark(bit_sample(0)) or not is_mark(bit_sample(9)) or not is_carrier(bit_sample(9)):
            index += 1
            continue

        value = 0
        for bit_index in range(8):
            if is_mark(bit_sample(bit_index + 1)):
                value |= 1 << bit_index
        output.append(value)

        # Continue in the middle of the stop bit
        index = start_time + 9 * bit_length + bit_length // 2
    return bytes(output)


def crc16(data):
    crc = 0xFFFF
    for byte_value in data:
        crc ^= byte_value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def parse_frames(raw_bytes):
    """Returns (frames, bad_count); a frame is (page, line, text)."""
    frames = []
    bad_count = 0
    for raw_line in raw_bytes.split(b'\n'):
        raw_line = raw_line.rstrip(b'\r')
        # Garbage before the frame (noise as a start bit) is dropped
        start = re.search(rb'\d{4}\.\d{2}:', raw_line)
        if not start:
            if raw_line.strip():
                bad_count += 1
            continue
        raw_line = raw_line[start.start():]
        text_line = raw_line.decode('latin-1')
        match = FRAME_PATTERN.match(text_line)
        if not match:
            bad_count += 1
            continue
        body = raw_line[:raw_line.rindex(b'*')]
        if crc16(body) != int(match.group(4), 16):
            bad_count += 1
            continue
        frames.append((int(match.group(1)), int(match.group(2)), match.group(3)))
    return frames, bad_count


def print_log(frames, output):
    pages = {}
    ended = set()
    for page, line, text in frames:
        if line == 0:
            ended.add(page)
        else:
            pages.setdefault(page, {})[line] = text  # a repeat replaces it

    for page in sorted(pages):
        lines = pages[page]
        last_line = max(lines)
        missing = [line for line in range(1, last_line + 1) if line not in lines]
        state = 'complete' if page in ended and not missing else 'INCOMPLETE'
        output.write('===== page %04d: %d lines, %s' % (page, len(lines), state))
        if missing:
            output.write(', missing %s' % ','.join(str(line) for line in missing))
        output.write(' =====\n')
        for line in range(1, last_line + 1):
            output.write(lines.get(line, '<missing line %d>' % line) + '\n')


def main():
    parser = argparse.ArgumentParser(description='Decode /DEBUGPORT=BEEP tones')
    parser.add_argument('wav', help='PCM WAV recording')
    parser.add_argument('--baud', default='auto', choices=['auto', '1200', '300'])
    parser.add_argument('--raw', action='store_true',
                        help='print the received bytes without checking frames')
    arguments = parser.parse_args()

    samples, sample_rate = read_wav(arguments.wav)
    if sample_rate < 8000:
        sys.exit('the sample rate must be at least 8000 Hz')

    if arguments.baud == 'auto':
        baud_rates = [1200, 300]
    else:
        baud_rates = [int(arguments.baud)]

    best = None
    for baud_rate in baud_rates:
        raw_bytes = demodulate(samples, sample_rate, baud_rate)
        frames, bad_count = parse_frames(raw_bytes)
        sys.stderr.write('%d baud: %d bytes, %d good frames, %d bad frames\n'
                         % (baud_rate, len(raw_bytes), len(frames), bad_count))
        if best is None or len(frames) > len(best[1]):
            best = (raw_bytes, frames)

    if arguments.raw:
        sys.stdout.write(best[0].decode('latin-1'))
    else:
        print_log(best[1], sys.stdout)


if __name__ == '__main__':
    main()
