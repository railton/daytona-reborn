package linkcodec

import (
	"math/rand"
	"testing"
)

// blocks like a race's: 8 cars' slots, each changing a few dozen bytes a
// frame (1 to 6% of a block changes, measured on captured sessions)
func raceBlocks(n int) [][]byte {
	rng := rand.New(rand.NewSource(1))
	f := make([]byte, 1+8*Slot)
	f[0] = 1
	rng.Read(f[1:])
	out := make([][]byte, n)
	for i := range out {
		for k := 0; k < 30; k++ {
			f[1+rng.Intn(8*Slot)] = byte(rng.Intn(256))
		}
		out[i] = append([]byte(nil), f...)
	}
	return out
}

func BenchmarkEncode(b *testing.B) {
	blocks := raceBlocks(256)
	var c Codec
	var out []byte
	b.SetBytes(int64(len(blocks[0])))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		out = c.Encode(out[:0], blocks[i%len(blocks)])
	}
}

func BenchmarkDecode(b *testing.B) {
	blocks := raceBlocks(256)
	var enc Codec
	msgs := make([][]byte, len(blocks))
	for i, f := range blocks {
		msgs[i] = enc.Encode(nil, f)
	}
	b.SetBytes(int64(len(blocks[0])))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		var dec Codec
		for _, m := range msgs[:8] {
			dec.Decode(m)
		}
	}
}

// differing counts as a byte-by-byte loop does (below its limit).
func TestDiffering(t *testing.T) {
	rng := rand.New(rand.NewSource(2))
	a, b := make([]byte, Slot), make([]byte, Slot)
	for n := 0; n < 2000; n++ {
		rng.Read(a)
		copy(b, a)
		for k := rng.Intn(Slot); k > 0; k-- {
			b[rng.Intn(Slot)] ^= byte(1 << rng.Intn(8))
		}
		want := 0
		for i := range a {
			if a[i] != b[i] {
				want++
			}
		}
		if got := differing(a, b, Slot+1); got != want {
			t.Fatalf("differing %d, want %d", got, want)
		}
		if limit := rng.Intn(Slot) + 1; want >= limit && differing(a, b, limit) < limit {
			t.Fatal("stopped below its limit")
		}
	}
}
