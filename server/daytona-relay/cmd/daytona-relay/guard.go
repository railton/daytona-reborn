package main

// Abuse limits: what one connection, one address or one room may take of
// the server, so a player cannot push the others out and a few addresses
// cannot hold every room.
//
//   - Each connection's frames are rate limited (a token bucket for frames,
//     one for bytes, one for control frames): a game sends about 120 frames
//     a second and a few control frames, far below them. One that floods is
//     disconnected itself; before, its flood filled the other players' send
//     queues and the server disconnected them instead.
//   - Each player's datagrams are rate limited the same way; the excess is
//     dropped (a datagram's source can be forged, so it never disconnects).
//   - One address opens at most maxRoomsPerIP rooms.
//   - A room with its creator alone for roomAlone, or without a race and
//     without game data for roomIdle, is closed (error "idle"). A room whose
//     games exchange data (a race, or linked between races) is never idle.
//   - One address gets at most joinTriesPerMin failed attempts a minute
//     (a room that does not exist, a wrong password): guessing room codes
//     or the password goes no faster than that (error "slow_down").

import (
	"errors"
	"log"
	"time"

	"daytona-relay/proto"
)

const (
	frameRate, frameBurst     = 400, 1000         // frames a second, from one connection
	byteRate, byteBurst       = 1 << 20, 2 << 20  // bytes a second
	controlRate, controlBurst = 20, 60            // control (C) frames a second
	datagramRate              = 400               // datagrams a second, from one player
	datagramBurst             = 1000              //
	datagramByteRate          = 2 << 20           // datagram bytes a second
	janitorEvery              = 30 * time.Second  // how often idle rooms and old counters are looked at
	defaultRoomAlone          = 30 * time.Minute  // DAYTONA_ROOM_ALONE_MIN
	defaultRoomIdle           = 120 * time.Minute // DAYTONA_ROOM_IDLE_MIN
	defaultRoomsPerIP         = 8                 // DAYTONA_MAX_ROOMS_PER_IP
	defaultJoinTries          = 20                // DAYTONA_JOIN_TRIES_PER_MIN
)

// bucket: a token bucket; take spends n tokens if it has them.
type bucket struct {
	tokens, rate, burst float64
	last                time.Time
}

func newBucket(rate, burst float64, now time.Time) bucket {
	return bucket{tokens: burst, rate: rate, burst: burst, last: now}
}

func (b *bucket) take(n float64, now time.Time) bool {
	if d := now.Sub(b.last).Seconds(); d > 0 {
		b.tokens = min(b.burst, b.tokens+d*b.rate)
		b.last = now
	}
	if b.tokens < n {
		return false
	}
	b.tokens -= n
	return true
}

// limits: one connection's buckets (its reading goroutine only).
type limits struct {
	frames, bytes, control bucket
}

func newLimits(now time.Time) limits {
	return limits{newBucket(frameRate, frameBurst, now), newBucket(byteRate, byteBurst, now),
		newBucket(controlRate, controlBurst, now)}
}

// allow: whether one more frame of this connection's is within its limits.
func (l *limits) allow(f proto.Frame, now time.Time) bool {
	if !l.frames.take(1, now) || !l.bytes.take(float64(len(f.Payload)+7), now) {
		return false
	}
	return f.Type != proto.TypeControl || l.control.take(1, now)
}

// datagramLimits: one player's datagram buckets (under server.mu).
type datagramLimits struct {
	set          bool
	count, bytes bucket
}

func (d *datagramLimits) allow(n int, now time.Time) bool {
	if !d.set {
		d.count, d.bytes, d.set = newBucket(datagramRate, datagramBurst, now), newBucket(datagramByteRate, datagramByteRate, now), true
	}
	return d.count.take(1, now) && d.bytes.take(float64(n), now)
}

// tries: failed attempts from one address in the current minute.
type tries struct {
	since time.Time
	n     int
}

// tooManyTries: whether ip has used up its failed attempts this minute.
func (s *server) tooManyTries(ip string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	t := s.failures[ip]
	return t != nil && s.now().Sub(t.since) < time.Minute && t.n >= s.joinTriesPerMin
}

// failed counts a refused attempt of ip's when it could be a guess.
func (s *server) failed(ip string, err error) {
	var e *proto.Error
	if !errors.As(err, &e) || (e.Code != "not_found" && e.Code != "password") {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	t := s.failures[ip]
	if t == nil || now.Sub(t.since) >= time.Minute {
		t = &tries{since: now}
		s.failures[ip] = t
	}
	t.n++
	if t.n == s.joinTriesPerMin {
		log.Printf("%s: %d failed attempts in a minute: slowed down", ip, t.n)
	}
}

// roomsOf: the rooms ip created that are still open. Caller holds s.mu.
func (s *server) roomsOf(ip string) int {
	n := 0
	for _, r := range s.rooms {
		if r.creatorIP == ip {
			n++
		}
	}
	return n
}

// janitor closes idle rooms and forgets old attempt counters. Takes s.mu.
func (s *server) janitor() {
	s.mu.Lock()
	now := s.now()
	for ip, t := range s.failures {
		if now.Sub(t.since) >= time.Minute {
			delete(s.failures, ip)
		}
	}
	for _, r := range s.rooms {
		if r.auto > 0 {
			continue // the daytona-link helper's rooms: no owner to start a race, they wait for their size
		}
		why := ""
		if len(r.members) == 1 && now.Sub(r.aloneSince) >= s.roomAlone {
			why = "its creator alone for " + s.roomAlone.String()
		} else if now.Sub(r.active) >= s.roomIdle {
			why = "no race for " + s.roomIdle.String()
		}
		if why == "" {
			continue
		}
		log.Printf("room %s: closed, %s", r.code, why)
		for _, m := range append([]*client(nil), r.members...) {
			// the error first: the writer sends it, then closes the connection
			m.send(proto.Frame{Type: proto.TypeError, Payload: proto.ErrorPayload(
				proto.Errf("idle", "the room was closed: nobody raced in it for a long time"))})
			m.away = false
			s.remove(m)
		}
	}
	s.mu.Unlock()
}
