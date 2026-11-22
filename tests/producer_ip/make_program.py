"""Create a wholly synthetic i960 program; no ROM or game data required."""
import struct
import sys
from pathlib import Path

program = bytearray(0x1014)
words = {
    0: 0x2000, 4: 0x2000, 12: 0x1000,  # unmapped SAT/PRCB, reset entry
    0x1000: 0x8c000000 | (4 << 19) | (12 << 10),  # lda displacement,r4
    0x1004: 0x00800000,
    0x1008: 0x92000000 | (5 << 19) | (4 << 14) | (4 << 10),  # st r5,(r4)
    0x100c: 0x92000000 | (6 << 19) | (4 << 14) | (4 << 10),  # st r6,(r4)
    0x1010: 0x82000000 | (7 << 19) | (4 << 14) | 0x2000 | 1, # stob r7,1(r4)
}
for address, value in words.items():
    struct.pack_into('<I', program, address, value)
Path(sys.argv[1]).write_bytes(program)
