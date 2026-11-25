// Package linkcodec is the game's link codec (src/app/link_codec.h), for the
// relay's star mode: the relay decodes each player's comm-board frames and
// encodes the frames it composes for each player. One Codec a direction a
// ring, as in the game; the formats must stay identical:
//
//	0x00 raw:   the frame as it is
//	0x01 token: u16 length, byte 1 (the rest decodes as zeros)
//	0x02 block: id, then per 448-byte slot: a reference (0 zeros, 1..8 that
//	            slot of the previous block, 9..16 of the one before) and the
//	            XOR against it as (varint zero run, varint literal count, literals)*
package linkcodec

import (
	"encoding/binary"
	"errors"
	"math/bits"
)

const (
	Slot     = 0x1c0
	MaxFrame = 0x4000
)

var ErrMalformed = errors.New("linkcodec: malformed message")

type Codec struct {
	hist [2][]byte // the previous block (no id), the one before
}

func (c *Codec) Reset() { c.hist[0], c.hist[1] = nil, nil }

func (c *Codec) ref(r byte) []byte {
	if r == 0 || r > 16 {
		return nil
	}
	h := c.hist[(r-1)/8]
	off := int((r-1)%8) * Slot
	if off+Slot > len(h) {
		return nil
	}
	return h[off : off+Slot]
}

func (c *Codec) push(block []byte) {
	old := c.hist[1] // its buffer is reused: nothing else holds it
	c.hist[1] = c.hist[0]
	c.hist[0] = append(old[:0], block...)
}

func putVar(out []byte, n int) []byte {
	for n >= 0x80 {
		out = append(out, byte(n)|0x80)
		n >>= 7
	}
	return append(out, byte(n))
}

func getVar(m []byte, p int) (int, int, bool) {
	n := 0
	for shift := 0; shift < 28; shift += 7 {
		if p >= len(m) {
			return 0, p, false
		}
		c := m[p]
		p++
		n |= int(c&0x7f) << shift
		if c < 0x80 {
			return n, p, true
		}
	}
	return 0, p, false
}

// IsBlock: a comm-board data block (an id and whole slots), what Encode
// compresses against the history.
func IsBlock(f []byte) bool { return len(f) >= 2 && f[0] <= 8 && (len(f)-1)%Slot == 0 }

var zeroSlot [Slot]byte

// differing counts the bytes where a and b (Slot bytes each) differ, 8 at a
// time; it stops once the count reaches limit (the reference has lost to a
// better one: only a smaller count is taken), so the encoding is the same.
func differing(a, b []byte, limit int) int {
	d := 0
	for i := 0; i < Slot; i += 8 {
		x := binary.LittleEndian.Uint64(a[i:]) ^ binary.LittleEndian.Uint64(b[i:])
		if x == 0 {
			continue
		}
		x |= x >> 4 // each byte's low bit: whether that byte differs
		x |= x >> 2
		x |= x >> 1
		d += bits.OnesCount64(x & 0x0101010101010101)
		if d >= limit {
			return d
		}
	}
	return d
}

// Encode appends the message for one wire frame.
func (c *Codec) Encode(out, f []byte) []byte {
	n := len(f)
	if n >= 2 && n <= 0xffff && f[0] == 0xfc {
		return append(out, 1, byte(n>>8), byte(n), f[1])
	}
	if !IsBlock(f) {
		out = append(out, 0)
		return append(out, f...)
	}
	out = append(out, 2, f[0])
	b := f[1:]
	for off := 0; off < len(b); off += Slot {
		cur := b[off : off+Slot]
		same := byte(1 + off/Slot)
		best, bestN := byte(0), Slot+1
		consider := func(r byte) bool {
			d := 0
			if r == 0 {
				d = differing(cur, zeroSlot[:], bestN)
			} else if x := c.ref(r); x != nil {
				d = differing(cur, x, bestN)
			} else {
				return false
			}
			if d < bestN {
				best, bestN = r, d
			}
			return d == 0
		}
		if !(same <= 16 && consider(same)) {
			for r := byte(0); r <= 16; r++ {
				if r != same && consider(r) {
					break
				}
			}
		}
		out = append(out, best)
		x := c.ref(best)
		at := func(i int) byte {
			if x != nil {
				return cur[i] ^ x[i]
			}
			return cur[i]
		}
		for i := 0; i < Slot; {
			j := i
			for j < Slot && at(j) == 0 {
				j++
			}
			out = putVar(out, j-i)
			i = j
			for j < Slot {
				if at(j) == 0 {
					if j+2 < Slot && at(j+1) == 0 && at(j+2) == 0 {
						break
					}
					k := j
					for k < Slot && at(k) == 0 {
						k++
					}
					if k == Slot {
						break
					}
				}
				j++
			}
			out = putVar(out, j-i)
			for ; i < j; i++ {
				out = append(out, at(i))
			}
		}
	}
	c.push(b)
	return out
}

// Decode returns the frame of one message.
func (c *Codec) Decode(m []byte) ([]byte, error) {
	if len(m) == 0 {
		return nil, ErrMalformed
	}
	switch m[0] {
	case 0:
		if len(m)-1 > MaxFrame {
			return nil, ErrMalformed
		}
		return append([]byte(nil), m[1:]...), nil
	case 1:
		if len(m) != 4 {
			return nil, ErrMalformed
		}
		n := int(m[1])<<8 | int(m[2])
		if n < 2 || n > MaxFrame {
			return nil, ErrMalformed
		}
		t := make([]byte, n)
		t[0], t[1] = 0xfc, m[3]
		return t, nil
	case 2:
	default:
		return nil, ErrMalformed
	}
	if len(m) < 2 {
		return nil, ErrMalformed
	}
	frame := make([]byte, 1, 1+8*Slot) // the id, then the block
	frame[0] = m[1]
	p := 2
	for p < len(m) {
		if len(frame)-1+Slot > MaxFrame {
			return nil, ErrMalformed
		}
		r := m[p]
		p++
		if r > 16 {
			return nil, ErrMalformed
		}
		x := c.ref(r)
		if r != 0 && x == nil {
			return nil, ErrMalformed
		}
		for i := 0; i < Slot; {
			var zeros, literals int
			var ok bool
			if zeros, p, ok = getVar(m, p); !ok {
				return nil, ErrMalformed
			}
			if literals, p, ok = getVar(m, p); !ok {
				return nil, ErrMalformed
			}
			if zeros > Slot-i || literals > Slot-i-zeros || len(m)-p < literals {
				return nil, ErrMalformed
			}
			if x != nil {
				frame = append(frame, x[i:i+zeros]...)
			} else {
				frame = append(frame, zeroSlot[:zeros]...)
			}
			i += zeros
			if x != nil {
				for k := 0; k < literals; k++ {
					frame = append(frame, m[p+k]^x[i+k])
				}
			} else {
				frame = append(frame, m[p:p+literals]...)
			}
			p += literals
			i += literals
		}
	}
	if len(frame) == 1 {
		return nil, ErrMalformed
	}
	c.push(frame[1:])
	return frame, nil
}
