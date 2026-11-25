#!/usr/bin/env python3
"""wc3n_check. UDP listener that validates WC3N input packets on the wire.

Usage: python3 scripts/wc3n_check.py [port] [count]
Defaults: port 3443, count 120 (2 s at 60 Hz). Exit 0 iff every assertion
holds: magic, monotonic seq, throttle range, buttons width, and observed
axis ranges reported. The high buttons byte is reported per packet class:
0 throughout means a pre-flags sender, nonzero means a game-instance client.
"""

import socket
import struct
import sys

MAGIC = 0x4E334357
FMT = "<IIiiiiI"  # magic, seq, pitch, lateral, rudder, throttle, buttons


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 3443
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    s.settimeout(5.0)
    print(f"listening udp/{port}, wanting {want} packets")
    got = 0
    last_seq = None
    seq_mono = True
    ranges = {"pitch": [1 << 30, -(1 << 30)], "lateral": [1 << 30, -(1 << 30)]}
    throttle_bad = 0
    buttons_wide = 0
    high_byte_nonzero = 0
    while got < want:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            print(f"timeout after {got} packets")
            break
        if len(data) != struct.calcsize(FMT):
            print(f"FAIL length {len(data)} from {addr}")
            return 1
        magic, seq, pitch, lat, rud, thr, btn = struct.unpack(FMT, data)
        if magic != MAGIC:
            print(f"FAIL magic {magic:#x} from {addr}")
            return 1
        if last_seq is not None and seq != last_seq + 1:
            seq_mono = False
        last_seq = seq
        if not 0 <= thr <= 100:
            throttle_bad += 1
        if btn > 0xFFFF:
            buttons_wide += 1
        if (btn >> 8) & 0xFF:
            high_byte_nonzero += 1
        ranges["pitch"][0] = min(ranges["pitch"][0], pitch)
        ranges["pitch"][1] = max(ranges["pitch"][1], pitch)
        ranges["lateral"][0] = min(ranges["lateral"][0], lat)
        ranges["lateral"][1] = max(ranges["lateral"][1], lat)
        got += 1
    ok = got > 0 and seq_mono and throttle_bad == 0 and buttons_wide == 0
    print(f"packets={got} seq_monotonic={seq_mono} "
          f"throttle_out_of_range={throttle_bad} buttons_wide={buttons_wide} "
          f"high_byte_nonzero={high_byte_nonzero}")
    print(f"pitch_observed=[{ranges['pitch'][0]},{ranges['pitch'][1]}] "
          f"lateral_observed=[{ranges['lateral'][0]},{ranges['lateral'][1]}]")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
