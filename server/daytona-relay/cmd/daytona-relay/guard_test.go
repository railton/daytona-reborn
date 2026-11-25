package main

import (
	"net"
	"strings"
	"testing"
	"time"

	"daytona-relay/proto"
)

// A bucket gives its burst at once, then its rate.
func TestBucket(t *testing.T) {
	now := time.Unix(1000, 0)
	l := newLimits(now)
	c := proto.Frame{Type: proto.TypeControl, Payload: []byte("x")}
	for i := 0; i < controlBurst; i++ {
		if !l.allow(c, now) {
			t.Fatalf("control frame %d refused within the burst", i)
		}
	}
	if l.allow(c, now) {
		t.Fatal("control frames past the burst allowed")
	}
	if !l.allow(c, now.Add(100*time.Millisecond)) { // 20 a second: 2 more after 0.1 s
		t.Fatal("the rate did not refill")
	}
	d := proto.Frame{Type: proto.TypeData, Payload: make([]byte, 300)}
	l = newLimits(now)
	for i := 0; i < 120*60; i++ { // a minute of a game's frames, 120 a second
		if !l.allow(d, now.Add(time.Duration(i)*time.Second/120)) {
			t.Fatalf("a game's ordinary rate refused at frame %d", i)
		}
	}
}

// A player flooding control frames is disconnected; the others stay.
func TestFloodDisconnectsTheSender(t *testing.T) {
	s := newTestServer(t)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go s.serve(ln)
	a, _ := dial(t, ln.Addr().String(), proto.Hello{Op: "create", Room: "flood", Name: "A", Game: "10"})
	b, _ := dial(t, ln.Addr().String(), proto.Hello{Op: "join", Room: "flood", Name: "B", Game: "10"})
	nextStatus(t, a)
	proto.WriteFrame(a, proto.Frame{Type: proto.TypeBegin})
	nextStatus(t, a)
	nextStatus(t, b)
	s.mu.Lock()
	epoch := s.rooms["flood"].epoch
	s.mu.Unlock()
	for i := 0; i < 500; i++ {
		if proto.WriteFrame(b, proto.Frame{Type: proto.TypeControl, Epoch: epoch, Payload: []byte("pos=2:1")}) != nil {
			break
		}
	}
	b.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		f, err := proto.ReadFrame(b.br)
		if err != nil {
			t.Fatal("the flooder was not told why")
		}
		if f.Type == proto.TypeError {
			if !strings.HasPrefix(string(f.Payload), "flood\n") {
				t.Fatalf("error %q", f.Payload)
			}
			break
		}
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		s.mu.Lock()
		r := s.rooms["flood"]
		n := len(r.members)
		owner := r.owner
		s.mu.Unlock()
		if n == 1 && owner != nil && owner.name == "A" {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("the flooder is still in the room, or the owner left")
}

// One address opens at most maxRoomsPerIP rooms.
func TestRoomsPerIP(t *testing.T) {
	s := newTestServer(t)
	s.maxRoomsPerIP = 2
	for i, want := range []string{"", "", "too_many_rooms"} {
		c := fakeClient("A")
		c.ip = "203.0.113.7"
		err := s.join(c, proto.Hello{Op: "create", Room: string(rune('a' + i))})
		if got := code(err); got != want {
			t.Fatalf("room %d: %q, want %q", i+1, got, want)
		}
	}
	other := fakeClient("B")
	other.ip = "203.0.113.8"
	if err := s.join(other, proto.Hello{Op: "create", Room: "z"}); err != nil {
		t.Fatalf("another address: %v", err)
	}
}

// A room whose creator waits alone, or whose players never race, is closed;
// a room whose games exchange data is not.
func TestIdleRooms(t *testing.T) {
	s := newTestServer(t)
	clk := &clock{t: time.Unix(1000, 0)}
	s.now = clk.now
	alone, pairA, pairB, racerA, racerB := fakeClient("alone"), fakeClient("A"), fakeClient("B"), fakeClient("C"), fakeClient("D")
	s.join(alone, proto.Hello{Op: "create", Room: "alone"})
	s.join(pairA, proto.Hello{Op: "create", Room: "pair"})
	s.join(pairB, proto.Hello{Op: "join", Room: "pair"})
	s.join(racerA, proto.Hello{Op: "create", Room: "race"})
	s.join(racerB, proto.Hello{Op: "join", Room: "race"})
	s.begin(racerA)
	_, epoch := lastStatus(t, racerA)
	data := func() {
		s.mu.Lock()
		s.forwardLocked(racerA, proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: []byte{1}})
		s.mu.Unlock()
	}
	open := func(code string) bool {
		s.mu.Lock()
		defer s.mu.Unlock()
		return s.rooms[code] != nil
	}
	clk.add(29 * time.Minute)
	data()
	s.janitor()
	if !open("alone") || !open("pair") || !open("race") {
		t.Fatal("closed too soon")
	}
	clk.add(2 * time.Minute)
	data()
	s.janitor()
	if open("alone") || !open("pair") {
		t.Fatal("the creator alone for 31 min: its room should be closed, the pair's not")
	}
	if !gotError(alone, "idle") {
		t.Error("the creator was not told why")
	}
	for i := 0; i < 3; i++ { // 3 x 30 min more: 121 min without a race in "pair"
		clk.add(30 * time.Minute)
		data()
		s.janitor()
	}
	if open("pair") || !open("race") {
		t.Fatalf("after 121 min: pair open %v (want closed), race open %v (want open)", open("pair"), open("race"))
	}
}

// Failed attempts from one address are counted a minute at a time.
func TestJoinTries(t *testing.T) {
	s := newTestServer(t)
	clk := &clock{t: time.Unix(1000, 0)}
	s.now = clk.now
	s.joinTriesPerMin = 3
	ip := "198.51.100.1"
	miss := proto.Errf("not_found", "no such room")
	s.failed(ip, proto.Errf("full", "a full room is not a guess"))
	for i := 0; i < 3; i++ {
		if s.tooManyTries(ip) {
			t.Fatalf("slowed down after %d tries", i)
		}
		s.failed(ip, miss)
	}
	if !s.tooManyTries(ip) || s.tooManyTries("198.51.100.2") {
		t.Fatal("not slowed down after 3 tries, or another address was")
	}
	clk.add(61 * time.Second)
	if s.tooManyTries(ip) {
		t.Fatal("still slowed down a minute later")
	}
}

// A datagram address is taken only from the player's own IP, and forgotten
// when the player leaves.
func TestUDPHelloOwnAddress(t *testing.T) {
	s := newTestServer(t)
	uc, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer uc.Close()
	s.udpConn = uc
	go s.serveUDP()
	a := fakeClient("A")
	a.udp, a.ip = true, "127.0.0.2" // its TCP connection came from another address
	b := fakeClient("B")
	b.udp, b.ip = true, "127.0.0.1"
	s.join(a, proto.Hello{Op: "create", Room: "u", UDP: true})
	s.join(b, proto.Hello{Op: "join", Room: "u", UDP: true})
	send := func(key string) {
		c, err := net.DialUDP("udp", nil, uc.LocalAddr().(*net.UDPAddr))
		if err != nil {
			t.Fatal(err)
		}
		defer c.Close()
		c.Write(append([]byte{'h'}, key...))
	}
	send(a.key) // from 127.0.0.1: not A's address
	send(b.key) // B's own
	time.Sleep(200 * time.Millisecond)
	s.mu.Lock()
	aAddr, bAddr, n := a.udpAddr, b.udpAddr, len(s.udpByAddr)
	s.mu.Unlock()
	if aAddr != nil || bAddr == nil || n != 1 {
		t.Fatalf("A's address %v (want none), B's %v, %d known", aAddr, bAddr, n)
	}
	s.leave(b)
	s.mu.Lock()
	n = len(s.udpByAddr)
	s.mu.Unlock()
	if n != 0 {
		t.Fatalf("%d addresses kept after B left", n)
	}
}

func code(err error) string {
	if err == nil {
		return ""
	}
	if e, ok := err.(*proto.Error); ok {
		return e.Code
	}
	return err.Error()
}

func gotError(c *client, code string) bool {
	for {
		select {
		case f := <-c.out:
			if f.Type == proto.TypeError && strings.HasPrefix(string(f.Payload), code+"\n") {
				return true
			}
		default:
			return false
		}
	}
}
