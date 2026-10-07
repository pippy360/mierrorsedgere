"""LZO1X decompression in pure Python, for machines with no liblzo2 (Windows).

A port of the decoder in src/assets/upk_loader.cpp. The state carried between
instructions is what the short decoders get wrong: an instruction byte below 16
means three different things depending on what came before it -

    after a match with no trailing literals   a literal run
    after a match with 1-3 trailing literals  M1: 2 bytes from 1..0x400 back
    after a literal run                       M1: 3 bytes from 0x801..0xC00 back

Mirror's Edge packages use all three. A decoder that treats the last as the
second still gets most blocks right, then comes out a byte short on one of them
and shifts every export after it.
"""


class LZOError(ValueError):
    pass


def decompress(src, expected_len):
    """Decompress one LZO1X block. Raises LZOError unless it yields exactly expected_len bytes."""
    n = len(src)
    out = bytearray()
    ip = 0
    LITERAL_RUN = 4  # state: the previous instruction was a literal run

    def copy_match(dist, length):
        pos = len(out) - dist
        if dist <= 0 or pos < 0:
            raise LZOError("match reaches %d bytes back with %d written" % (dist, len(out)))
        if dist >= length:
            out.extend(out[pos:pos + length])
        else:
            # Overlapping: the run repeats the last `dist` bytes.
            chunk = bytes(out[pos:])
            reps, rem = divmod(length, dist)
            out.extend(chunk * reps + chunk[:rem])

    try:
        state = 0
        if src[0] > 17:
            t = src[0] - 17
            ip = 1
            out.extend(src[ip:ip + t])
            ip += t
            state = t if t < 4 else LITERAL_RUN

        while True:
            t = src[ip]
            ip += 1
            if t < 16:
                if state == 0:
                    if t == 0:
                        while src[ip] == 0:
                            t += 255
                            ip += 1
                        t += 15 + src[ip]
                        ip += 1
                    t += 3
                    out.extend(src[ip:ip + t])
                    ip += t
                    state = LITERAL_RUN
                    continue
                trailing = t & 3
                if state == LITERAL_RUN:
                    copy_match(0x801 + (t >> 2) + (src[ip] << 2), 3)
                else:
                    copy_match(1 + (t >> 2) + (src[ip] << 2), 2)
                ip += 1
            elif t >= 64:
                trailing = t & 3
                dist = 1 + ((t >> 2) & 7) + (src[ip] << 3)
                ip += 1
                copy_match(dist, (t >> 5) + 1)
            elif t >= 32:
                length = t & 31
                if length == 0:
                    while src[ip] == 0:
                        length += 255
                        ip += 1
                    length += 31 + src[ip]
                    ip += 1
                trailing = src[ip] & 3
                dist = 1 + (src[ip] >> 2) + (src[ip + 1] << 6)
                ip += 2
                copy_match(dist, length + 2)
            else:
                high = (t & 8) << 11
                length = t & 7
                if length == 0:
                    while src[ip] == 0:
                        length += 255
                        ip += 1
                    length += 7 + src[ip]
                    ip += 1
                trailing = src[ip] & 3
                dist = high + (src[ip] >> 2) + (src[ip + 1] << 6)
                ip += 2
                if dist == 0:
                    break  # end of stream
                copy_match(dist + 0x4000, length + 2)

            if trailing:
                out.extend(src[ip:ip + trailing])
                ip += trailing
            state = trailing
    except IndexError:
        raise LZOError("compressed block ends mid-instruction at byte %d of %d" % (ip, n))

    if len(out) != expected_len:
        raise LZOError("block decompressed to %d bytes, expected %d" % (len(out), expected_len))
    return bytes(out)
