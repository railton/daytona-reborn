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

import (
	"encoding/binary"
	"log"
	"net"

	"daytona-relay/proto"
)

const (
	maxDatagram  = 1200 // bytes: under the usual path MTU, no fragmentation
	maxRepeated  = 2    // earlier messages repeated in each datagram
	maxOutOfStep = 64   // numbers kept waiting for an earlier one
)

type udpState struct {
	udp      bool         // its hello said udp=1
	udpAddr  *net.UDPAddr // where its datagrams come from (its 'h')
	rxEpoch  uint32
	rxNext   uint32
	rxEarly  map[uint32][]byte // arrived before rxNext
	txEpoch  uint32
	txNext   uint32
	txRecent [][]byte // the last messages sent, numbered, for repetition
}

// receiveNumbered takes message seq of c's (from TCP or UDP): once, in order.
// Caller holds s.mu.
func (s *server) receiveNumbered(c *client, epoch, seq uint32, msg []byte) {
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
	if !next.udp {
		next.send(f)
		return
	}
	if next.txEpoch != f.Epoch {
		next.txEpoch, next.txNext, next.txRecent = f.Epoch, 0, nil
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
	// the datagram: this message and the recent ones that fit
	dg := []byte{'d', 0, 0, 0, 0, 0}
	binary.BigEndian.PutUint32(dg[1:], f.Epoch)
	add := func(m []byte) bool { // m: seq(4) + message
		if len(dg)+6+len(m)-4 > maxDatagram {
			return false
		}
		dg = append(dg, m[:4]...)
		dg = binary.BigEndian.AppendUint16(dg, uint16(len(m)-4))
		dg = append(dg, m[4:]...)
		dg[5]++
		return true
	}
	if add(numbered) {
		for i := len(next.txRecent) - 1; i >= 0; i-- {
			if !add(next.txRecent[i]) {
				break
			}
		}
		s.udpConn.WriteToUDP(dg, next.udpAddr)
	}
	next.txRecent = append(next.txRecent, numbered)
	if len(next.txRecent) > maxRepeated {
		next.txRecent = next.txRecent[1:]
	}
}

// serveUDP reads the games' datagrams.
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
		case 'h': // a game's address: its key says who it is
			key := string(dg[1:])
			for _, r := range s.rooms {
				for _, m := range r.members {
					if m.udp && m.key != "" && m.key == key {
						if m.udpAddr == nil || m.udpAddr.String() != addr.String() {
							if s.udpByAddr == nil {
								s.udpByAddr = map[string]*client{}
							}
							m.udpAddr = addr
							s.udpByAddr[addr.String()] = m
						}
					}
				}
			}
		case 'd':
			if c := s.udpByAddr[addr.String()]; c != nil && c.udpAddr != nil && len(dg) >= 6 {
				epoch := binary.BigEndian.Uint32(dg[1:])
				count, p := int(dg[5]), 6
				for i := 0; i < count && p+6 <= len(dg); i++ {
					seq := binary.BigEndian.Uint32(dg[p:])
					l := int(binary.BigEndian.Uint16(dg[p+4:]))
					p += 6
					if p+l > len(dg) {
						break
					}
					s.receiveNumbered(c, epoch, seq, dg[p:p+l])
					p += l
				}
			}
		}
		s.mu.Unlock()
	}
}
