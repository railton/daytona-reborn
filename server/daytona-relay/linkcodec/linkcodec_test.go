package linkcodec

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"math/rand"
	"os"
	"testing"
)

// The game's C++ codec (src/app/link_codec.cpp) wrote testdata/cpp_stream.txt
// from synthetic frames: "F<frame hex>" then "M<its message hex>". This codec
// must encode them to the same bytes and decode them back.
func TestSameAsTheGame(t *testing.T) {
	f, err := os.Open("testdata/cpp_stream.txt")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var enc, dec Codec
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	var frame []byte
	pairs := 0
	for sc.Scan() {
		line := sc.Text()
		b, err := hex.DecodeString(line[1:])
		if err != nil {
			t.Fatal(err)
		}
		switch line[0] {
		case 'F':
			frame = b
		case 'M':
			if got := enc.Encode(nil, frame); !bytes.Equal(got, b) {
				t.Fatalf("pair %d: Go encodes %d bytes, C++ %d", pairs, len(got), len(b))
			}
			back, err := dec.Decode(b)
			if err != nil {
				t.Fatalf("pair %d: %v", pairs, err)
			}
			want := frame
			if frame[0] == 0xfc { // a token keeps bytes 0 and 1
				want = make([]byte, len(frame))
				want[0], want[1] = frame[0], frame[1]
			}
			if !bytes.Equal(back, want) {
				t.Fatalf("pair %d: decoded frame differs", pairs)
			}
			pairs++
		}
	}
	if pairs != 12 {
		t.Fatalf("%d pairs", pairs)
	}
}

func TestRoundTripAndRefusals(t *testing.T) {
	rng := rand.New(rand.NewSource(3))
	var enc, dec Codec
	block := make([]byte, 1+8*Slot)
	block[0] = 3
	for i := 0; i < 300; i++ {
		copy(block[1+Slot:], block[1:1+7*Slot])
		for k := 0; k < 30; k++ {
			block[1+rng.Intn(150)] = byte(rng.Intn(256))
		}
		m := enc.Encode(nil, block)
		back, err := dec.Decode(m)
		if err != nil || !bytes.Equal(back, block) {
			t.Fatalf("frame %d: %v", i, err)
		}
	}
	var fresh Codec
	for _, m := range [][]byte{{}, {7}, {1, 0x0e, 1}, {1, 0, 0, 7}, {2, 3}, {2, 3, 17}, {2, 3, 1, 0, 0}} {
		if _, err := fresh.Decode(m); err == nil {
			t.Errorf("accepted %x", m)
		}
	}
}
