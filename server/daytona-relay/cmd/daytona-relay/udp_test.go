package main

import (
	"encoding/binary"
	"net"
	"testing"
)

// A batch's messages share one datagram, newest first, with the udpRepeat
// messages sent before them.
func TestDatagramBatch(t *testing.T) {
	s := newTestServer(t)
	s.udpRepeat = 2
	c := fakeClient("A")
	c.udpAddr = &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 9}
	c.txEpoch = 5
	msg := func(seq uint32, body string) []byte {
		m := binary.BigEndian.AppendUint32(nil, seq)
		return append(m, body...)
	}
	seqs := func(dg []byte) (out []uint32) {
		if dg[0] != 'd' || binary.BigEndian.Uint32(dg[1:]) != 5 {
			t.Fatalf("header %v", dg[:6])
		}
		p := 6
		for i := 0; i < int(dg[5]); i++ {
			out = append(out, binary.BigEndian.Uint32(dg[p:]))
			p += 6 + int(binary.BigEndian.Uint16(dg[p+4:]))
		}
		if p != len(dg) {
			t.Fatalf("datagram of %d bytes, read %d", len(dg), p)
		}
		return out
	}
	batch := func(ms ...[]byte) []uint32 {
		c.txPend = ms
		dgs := s.buildDatagrams(c, nil)
		if len(dgs) != 1 {
			t.Fatalf("%d datagrams for one batch", len(dgs))
		}
		return seqs(dgs[0].data)
	}
	if got := batch(msg(0, "block"), msg(1, "token")); len(got) != 2 || got[0] != 1 || got[1] != 0 {
		t.Fatalf("first batch %v", got)
	}
	if got := batch(msg(2, "block"), msg(3, "token")); len(got) != 4 || got[0] != 3 || got[1] != 2 || got[2] != 1 || got[3] != 0 {
		t.Fatalf("second batch %v (want 3 2, then 1 0 repeated)", got)
	}
	s.udpRepeat = 0
	c.txRecent = nil
	if got := batch(msg(4, "block")); len(got) != 1 {
		t.Fatalf("no repetition: %v", got)
	}
	big := make([]byte, maxDatagram) // larger than a datagram: TCP alone
	c.txPend = [][]byte{msg(5, string(big)), msg(6, "token")}
	dgs := s.buildDatagrams(c, nil)
	if len(dgs) != 1 || len(seqs(dgs[0].data)) != 1 || seqs(dgs[0].data)[0] != 6 {
		t.Fatalf("an oversized message: %d datagrams", len(dgs))
	}
}
