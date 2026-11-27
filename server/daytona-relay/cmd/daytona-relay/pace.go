package main

// The star mode's own pace (DAYTONA_STAR_PACE_MS): a player's blocks are
// composed when its predecessor in the ring sends (compose), so a
// predecessor whose data stalls (a burst of loss: its messages wait for TCP's
// retransmission, 250 to 350 ms) stalled every other car for that player too,
// not only its own (measured in rooms of 4 and 6: 92 % smooth for every car
// seen by the player after the lossy one, against 97 % and more for the
// others). When a predecessor is late by paceAfter, the server sends the
// player its next block itself, once a frame: the last block it sent that
// player, with every car's next slot (slotFor), the late car's the same
// again (it waits). The late car's own slot goes to its successor in order,
// from its history, as every other car's does. Each block sent so counts one owed: the late car's blocks that
// then come in a burst (its backlog, the first one included) are taken into
// its history (keepOwn) but not sent, up to that many, so the player gets one
// block a frame throughout, never a burst; one at the frame's pace again
// clears what is owed. Only while the race runs steadily: every player's
// blocks of one size (the game's link reads blocks of a fixed size, set by
// its mode: a race ending changes it), the late one's for paceSteady blocks
// before; at most paceMaxRun frames in a row (its place kept after a drop).

import "time"

const (
	defaultPaceAfter = 35 * time.Millisecond // DAYTONA_STAR_PACE_MS; 0: off (the predecessor's pace only)
	framePeriod      = time.Second * 1000 / 57524
	paceEvery        = 2 * time.Millisecond
	paceSteady       = 170 // blocks of one size in a row (3 s) before the server sends any itself
	paceActive       = resumeGrace + 2*time.Second
	paceMaxRun       = 12 * 57524 / 1000 // frames in a row, at most (12 s: a dropped player's place is kept 10 s)
)

// paced: what the own pace keeps per player (as a predecessor: its blocks;
// as a destination: the last block sent to it), under server.mu.
type paced struct {
	lastIn         time.Time // its last block in
	inSize, steady int       // that block's size, blocks of that size in a row
	lastOut        []byte    // the last block sent to it, decoded
	lastOutAt      time.Time // when, or when one of its predecessor's was taken in its place
	synthRun, owed int       // blocks sent by the server in a row; its predecessor's blocks still to take in instead of sending
}

func (p *paced) resetPace() { *p = paced{lastOut: p.lastOut[:0]} }

// sawBlock: c sent a block of size n. Caller holds s.mu.
func (s *server) sawBlock(c *client, n int) {
	c.pace.lastIn = s.now()
	if n == c.pace.inSize {
		c.pace.steady++
	} else {
		c.pace.inSize, c.pace.steady = n, 1
	}
}

// paceTick: every paceEvery, the blocks the server sends itself. Takes s.mu.
func (s *server) paceTick() {
	if s.paceAfter <= 0 || !s.star || s.historyKeep == 0 {
		return
	}
	s.mu.Lock()
	now := s.now()
	for _, r := range s.rooms {
		n := len(r.ring)
		if n < 3 || r.auto != 0 {
			continue // two players: no other car to keep moving
		}
		for at, c := range r.ring {
			if next := r.ring[(at+1)%n]; c != nil && next != nil && next != c {
				s.paceOne(r, at, c, next, now)
			}
		}
	}
	s.mu.Unlock()
	s.flushDatagrams()
}

// paceOne: next's block, sent by the server, when c (at in the ring) is late.
// Caller holds s.mu.
func (s *server) paceOne(r *room, at int, c, next *client, now time.Time) {
	p := &next.pace
	if c.pace.steady < paceSteady || len(p.lastOut) != c.pace.inSize || now.Sub(c.pace.lastIn) > paceActive ||
		p.synthRun >= paceMaxRun {
		return
	}
	for _, m := range r.ring { // the others still in the race's mode (it ending changes their blocks' size)
		if m != c && m != nil && m.pace.inSize != len(p.lastOut) {
			return
		}
	}
	gap := s.paceAfter
	if p.synthRun > 0 {
		gap = framePeriod
	}
	if now.Sub(p.lastOutAt) < gap {
		return
	}
	if slot := s.slotFor(next, c); slot != nil {
		copy(p.lastOut[1:], slot) // c's own: its next one if taken in since, else the same (its car waits)
	}
	s.fillOthers(r, at, next, p.lastOut)
	s.sendData(next, framed(r.epoch, next.enc.Encode(nil, p.lastOut)))
	if p.synthRun > 0 && now.Sub(p.lastOutAt) < 2*framePeriod {
		p.lastOutAt = p.lastOutAt.Add(framePeriod) // a frame's pace, not the ticker's
	} else {
		p.lastOutAt = now
	}
	p.synthRun++
	p.owed++
	next.race.paced.Add(1)
}
