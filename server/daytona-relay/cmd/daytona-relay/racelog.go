package main

// What the server saw of each player in a race (a ring), for its log: set
// against the players' own logs (daytona.log, the games' "online race"
// lines) by room, name and time (both in UTC). Logged when the ring ends and
// every raceLogEvery while it runs.

import (
	"fmt"
	"log"
	"sync/atomic"
	"time"
)

const raceLogEvery = time.Minute

type raceStats struct {
	in, inUDP, out atomic.Int64 // game messages from the player (inUDP: delivered from a datagram), to it
	queueMax       atomic.Int64 // its send queue's longest (frames waiting for its connection)
	rttMin, rttMax atomic.Int64 // its latency samples in the race, µs
	rttSum, rttN   atomic.Int64
	drops, backs   atomic.Int64 // connection dropped (place kept), back in the race
	paced          atomic.Int64 // blocks the server sent it itself, its predecessor late (pace.go)
}

func (st *raceStats) reset() {
	for _, v := range []*atomic.Int64{&st.in, &st.inUDP, &st.out, &st.queueMax, &st.rttMin, &st.rttMax, &st.rttSum, &st.rttN,
		&st.drops, &st.backs, &st.paced} {
		v.Store(0)
	}
}

// copyFrom: a player back with a new connection keeps its race's numbers.
func (st *raceStats) copyFrom(o *raceStats) {
	pairs := [][2]*atomic.Int64{{&st.in, &o.in}, {&st.inUDP, &o.inUDP}, {&st.out, &o.out}, {&st.queueMax, &o.queueMax},
		{&st.rttMin, &o.rttMin}, {&st.rttMax, &o.rttMax}, {&st.rttSum, &o.rttSum}, {&st.rttN, &o.rttN},
		{&st.drops, &o.drops}, {&st.backs, &o.backs}, {&st.paced, &o.paced}}
	for _, p := range pairs {
		p[0].Store(p[1].Load())
	}
}

func (st *raceStats) latency(us int64) {
	if st.rttN.Add(1) == 1 || us < st.rttMin.Load() {
		st.rttMin.Store(us)
	}
	if us > st.rttMax.Load() {
		st.rttMax.Store(us)
	}
	st.rttSum.Add(us)
}

func (st *raceStats) queued(n int) {
	if int64(n) > st.queueMax.Load() {
		st.queueMax.Store(int64(n))
	}
}

// logRing: the race's players as this server saw them. Caller holds s.mu.
func (s *server) logRing(r *room, why string) {
	if len(r.ring) == 0 || r.ringSince.IsZero() {
		return
	}
	secs := time.Since(r.ringSince).Seconds()
	log.Printf("room %s: race of %d players %s, %.0f s since it started", r.code, len(r.ring), why, secs)
	for i, m := range r.ring {
		st := &m.race
		in := st.in.Load()
		lat := "--"
		if n := st.rttN.Load(); n > 0 {
			lat = fmt.Sprintf("%d/%d/%d ms", st.rttMin.Load()/1000, st.rttSum.Load()/n/1000, st.rttMax.Load()/1000)
		}
		rate := 0.0
		if secs > 1 {
			rate = float64(in) / secs
		}
		log.Printf("room %s:   %d %q: %d messages in (%.1f/s, %d first by UDP), %d out; latency min/avg/max %s; "+
			"send queue max %d; dropped %d, back %d; %d sent by the server on its own", r.code, i+1, m.name, in, rate,
			st.inUDP.Load(), st.out.Load(), lat, st.queueMax.Load(), st.drops.Load(), st.backs.Load(), st.paced.Load())
	}
	r.ringLogged = time.Now()
}

// logRaces: every raceLogEvery, the races running. Takes s.mu.
func (s *server) logRaces() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, r := range s.rooms {
		if len(r.ring) > 0 && time.Since(r.ringLogged) >= raceLogEvery {
			s.logRing(r, "running")
		}
	}
}
