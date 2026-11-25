package main

// Numbered game data, by TCP and UDP (a game whose hello says udp=1).
//
// Every game message travels both ways at once: in its TCP D frame (a
// 4-byte sequence number, then the message) and in a UDP datagram that also
// repeats the previous ones. The receiver takes each number once, from
// whichever comes first, and delivers them in order. A datagram lost on the
// way is covered by the next one, a frame later, instead of by a TCP
// retransmission (at least a round trip); TCP stays the backstop, so with
// UDP blocked everything works as before. Numbers start again with each
// ring (epoch), on both sides.
//
// Datagrams (both ways):
//
//	'h' key            game -> relay: who this address is (its Status.Key); every 2 s
//	'd' epoch(4) n(1) n x [seq(4) len(2) message]   numbered messages, newest first
//
// The messages a player gets in one go (a frame's block and token) share one
// datagram, with the udpRepeat messages before them: built under the
// server's lock, sent once it is released (flushDatagrams).

import (
	"encoding/binary"
	"log"
	"net"
	"time"

	"daytona-relay/proto"
)

const (
	maxDatagram   = 1200 // bytes: under the usual path MTU, no fragmentation
	defaultRepeat = 2    // earlier messages repeated in each datagram (DAYTONA_UDP_REPEAT)
	maxOutOfStep  = 64   // numbers kept waiting for an earlier one
)

type udpState struct {
	udp      bool         // its hello said udp=1
	udpAddr  *net.UDPAddr // where its datagrams come from (its 'h')
	rxEpoch  uint32
	rxNext   uint32
	rxEarly  map[uint32][]byte // arrived before rxNext
	txEpoch  uint32
	txNext   uint32
	txRecent [][]byte // the last messages sent by UDP, numbered, for repetition
	txPend   [][]byte // numbered messages for its next datagram (flushDatagrams)
}

// datagram: one ready to send, outside the server's lock.
type datagram struct {
	to   *net.UDPAddr
	data []byte
}

// receiveNumbered takes message seq of c's (from TCP or UDP): once, in order.
// Caller holds s.mu.
func (s *server) receiveNumbered(c *client, epoch, seq uint32, msg []byte, viaUDP bool) {
	r := c.room
	if r == nil || epoch != r.epoch {
		return
	}
	if c.rxEarly == nil || c.rxEpoch != epoch {
		c.rxEpoch, c.rxNext, c.rxEarly = epoch, 0, map[uint32][]byte{}
	}
	switch {
	case seq < c.rxNext:
		return // had it
	case seq > c.rxNext:
		if len(c.rxEarly) < maxOutOfStep {
			c.rxEarly[seq] = append([]byte(nil), msg...)
		}
		return
	}
	if viaUDP {
		c.race.inUDP.Add(1)
	}
	s.forwardLocked(c, framed(epoch, msg))
	c.rxNext++
	for {
		m, ok := c.rxEarly[c.rxNext]
		if !ok {
			return
		}
		delete(c.rxEarly, c.rxNext)
		s.forwardLocked(c, framed(epoch, m))
		c.rxNext++
	}
}

func framed(epoch uint32, msg []byte) proto.Frame {
	return proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: msg}
}

// sendData sends one game message to next: plain (TCP) or numbered (TCP and
// UDP). Caller holds s.mu.
func (s *server) sendData(next *client, f proto.Frame) {
	next.race.out.Add(1) // the log: messages to it, its queue at its longest (racelog.go)
	next.race.queued(len(next.out))
	if !next.udp {
		next.send(f)
		return
	}
	if next.txEpoch != f.Epoch {
		next.txEpoch, next.txNext, next.txRecent, next.txPend = f.Epoch, 0, nil, nil
	}
	seq := next.txNext
	next.txNext++
	numbered := make([]byte, 4+len(f.Payload))
	binary.BigEndian.PutUint32(numbered, seq)
	copy(numbered[4:], f.Payload)
	next.send(proto.Frame{Type: proto.TypeData, Epoch: f.Epoch, Payload: numbered})
	if s.udpConn == nil || next.udpAddr == nil {
		return
	}
	if len(next.txPend) == 0 {
		s.udpPending = append(s.udpPending, next)
	}
	next.txPend = append(next.txPend, numbered)
}

// buildDatagrams turns c's pending messages into datagrams (newest first,
// then the ones before them for repetition). Caller holds s.mu.
func (s *server) buildDatagrams(c *client, out []datagram) []datagram {
	pend := c.txPend
	c.txPend = nil
	if c.udpAddr == nil || len(pend) == 0 {
		return out
	}
	var dg []byte
	start := func() {
		dg = []byte{'d', 0, 0, 0, 0, 0}
		binary.BigEndian.PutUint32(dg[1:], c.txEpoch)
	}
	add := func(m []byte) bool { // m: seq(4) + message
		if len(dg)+2+len(m) > maxDatagram || dg[5] == 255 {
			return false
		}
		dg = append(dg, m[:4]...)
		dg = binary.BigEndian.AppendUint16(dg, uint16(len(m)-4))
		dg = append(dg, m[4:]...)
		dg[5]++
		return true
	}
	start()
	for i := len(pend) - 1; i >= 0; i-- {
		if !add(pend[i]) {
			if dg[5] > 0 {
				out = append(out, datagram{c.udpAddr, dg})
			}
			start()
			if !add(pend[i]) { // larger than a datagram: TCP alone carries it
				continue
			}
		}
	}
	for i := len(c.txRecent) - 1; i >= 0 && add(c.txRecent[i]); i-- {
	}
	if dg[5] > 0 {
		out = append(out, datagram{c.udpAddr, dg})
	}
	c.txRecent = append(c.txRecent, pend...)
	if n := len(c.txRecent) - s.udpRepeat; n > 0 {
		c.txRecent = append(c.txRecent[:0], c.txRecent[n:]...)
	}
	return out
}

// takeDatagrams: every player's pending datagrams, built. Caller holds s.mu.
func (s *server) takeDatagrams() []datagram {
	var out []datagram
	for _, c := range s.udpPending {
		out = s.buildDatagrams(c, out)
	}
	s.udpPending = s.udpPending[:0]
	return out
}

// sendDatagrams sends them; the caller does not hold s.mu.
func (s *server) sendDatagrams(dgs []datagram) {
	for _, d := range dgs {
		s.udpConn.WriteToUDP(d.data, d.to)
	}
}

// flushDatagrams builds and sends the pending datagrams. Takes s.mu.
func (s *server) flushDatagrams() {
	if s.udpConn == nil {
		return
	}
	s.mu.Lock()
	dgs := s.takeDatagrams()
	s.mu.Unlock()
	s.sendDatagrams(dgs)
}

// forgetAddr: c's datagram address is no longer c's. Caller holds s.mu.
func (s *server) forgetAddr(c *client) {
	if c.udpAddr != nil && s.udpByAddr[c.udpAddr.String()] == c {
		delete(s.udpByAddr, c.udpAddr.String())
	}
}

// serveUDP reads the games' datagrams (rate limited per player: guard.go).
func (s *server) serveUDP() {
	buf := make([]byte, 64*1024)
	for {
		n, addr, err := s.udpConn.ReadFromUDP(buf)
		if err != nil {
			log.Printf("udp: %v", err)
			return
		}
		if n < 1 {
			continue
		}
		dg := buf[:n]
		s.mu.Lock()
		switch dg[0] {
		case 'h': // a game's address: its key says who it is; only from that player's own address
			if m := s.byKey[string(dg[1:])]; m != nil && m.udp && m.room != nil && addr.IP.String() == m.ip {
				if m.udpAddr == nil || m.udpAddr.String() != addr.String() {
					s.forgetAddr(m)
					m.udpAddr = addr
					s.udpByAddr[addr.String()] = m
				}
			}
		case 'd':
			if c := s.udpByAddr[addr.String()]; c != nil && c.udpAddr != nil && len(dg) >= 6 && c.dgram.allow(n, time.Now()) {
				epoch := binary.BigEndian.Uint32(dg[1:])
				count, p := int(dg[5]), 6
				for i := 0; i < count && p+6 <= len(dg); i++ {
					seq := binary.BigEndian.Uint32(dg[p:])
					l := int(binary.BigEndian.Uint16(dg[p+4:]))
					p += 6
					if p+l > len(dg) {
						break
					}
					s.receiveNumbered(c, epoch, seq, dg[p:p+l], true)
					p += l
				}
			}
		}
		dgs := s.takeDatagrams()
		s.mu.Unlock()
		s.sendDatagrams(dgs)
	}
}
