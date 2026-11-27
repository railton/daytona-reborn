package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"net"
	"reflect"
	"strings"
	"testing"
	"time"

	"daytona-relay/linkcodec"
	"daytona-relay/proto"
	"daytona-relay/settings"
)

func envRace(t *testing.T, env map[string]string) settings.Race {
	t.Helper()
	r, err := settings.FromEnv(func(k string) string { return env[k] })
	if err != nil {
		t.Fatal(err)
	}
	return r
}

// fakeClient: a member without a connection, its frames queued in out.
func fakeClient(name string) *client {
	return &client{name: name, addr: "test", out: make(chan proto.Frame, 64), done: make(chan struct{})}
}

type clock struct{ t time.Time }

func (c *clock) now() time.Time          { return c.t }
func (c *clock) add(d time.Duration)     { c.t = c.t.Add(d) }
func newTestServer(t *testing.T) *server { return newServer(envRace(t, nil), "", 100, 1000, 16) }

// dial joins a room over TCP as a game does and returns its first status.
type player struct {
	net.Conn
	br *bufio.Reader
}

func dial(t *testing.T, addr string, h proto.Hello) (*player, proto.Status) {
	t.Helper()
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	if _, err := conn.Write([]byte(h.Line())); err != nil {
		t.Fatal(err)
	}
	p := &player{conn, bufio.NewReader(conn)}
	return p, nextStatus(t, p)
}

func nextStatus(t *testing.T, p *player) proto.Status {
	t.Helper()
	p.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		f, err := proto.ReadFrame(p.br)
		if err != nil {
			t.Fatal(err)
		}
		switch f.Type {
		case proto.TypeStatus:
			st, err := proto.DecodeStatus(f.Payload)
			if err != nil {
				t.Fatal(err)
			}
			return st
		case proto.TypeError:
			t.Fatalf("refused: %s", f.Payload)
		}
	}
}

// lastStatus: the newest status queued for a fake client.
func lastStatus(t *testing.T, c *client) (proto.Status, uint32) {
	t.Helper()
	var st proto.Status
	var epoch uint32
	found := false
	for {
		select {
		case f := <-c.out:
			if f.Type == proto.TypeStatus {
				st, _ = proto.DecodeStatus(f.Payload)
				epoch, found = f.Epoch, true
			}
		default:
			if !found {
				t.Fatalf("%s: no status", c.name)
			}
			return st, epoch
		}
	}
}

// The owner starts races; who joins during one waits for the next; a new
// Begin takes everyone; a racer leaving breaks the ring; the owner's role
// passes on.
func TestBeginQueueAndOwner(t *testing.T) {
	s := newTestServer(t)
	a, b, c := fakeClient("A"), fakeClient("B"), fakeClient("C")
	if err := s.join(a, proto.Hello{Op: "create", Game: "9", Room: "r"}); err != nil {
		t.Fatal(err)
	}
	s.begin(a) // alone: nothing
	if st, _ := lastStatus(t, a); st.Racing || !st.Owner {
		t.Fatalf("alone %+v", st)
	}
	s.join(b, proto.Hello{Op: "join", Game: "9", Room: "r"})
	s.begin(b) // not the owner: nothing
	if st, _ := lastStatus(t, b); st.Racing {
		t.Fatal("a non-owner started a race")
	}
	s.begin(a)
	sa, ea := lastStatus(t, a)
	sb, eb := lastStatus(t, b)
	if !sa.Ready || sa.Position != 1 || !sb.Ready || sb.Position != 2 || ea != eb {
		t.Fatalf("race of 2: %+v %+v", sa, sb)
	}
	// C joins during the race: waits; the racers' ring (epoch) is unchanged
	s.join(c, proto.Hello{Op: "join", Game: "9", Room: "r"})
	sc, _ := lastStatus(t, c)
	sa, ea2 := lastStatus(t, a)
	if sc.Ready || sc.Position != 0 || !sc.Racing || ea2 != ea || !sa.Ready ||
		!reflect.DeepEqual(sa.Waiting, []string{"C"}) || !reflect.DeepEqual(sa.Names, []string{"A", "B"}) {
		t.Fatalf("joining during a race: C %+v, A %+v (epoch %d -> %d)", sc, sa, ea, ea2)
	}
	// a new Begin takes everyone, in a new ring
	s.begin(a)
	sc, ec := lastStatus(t, c)
	if !sc.Ready || sc.Position != 3 || sc.Size != 3 || ec == ea {
		t.Fatalf("second race: C %+v epoch %d", sc, ec)
	}
	lastStatus(t, a)
	lastStatus(t, b)
	// B leaves mid-race: the ring breaks, back to the room
	s.leave(b)
	sa, ea3 := lastStatus(t, a)
	if sa.Ready || sa.Racing || ea3 == ec || !reflect.DeepEqual(sa.Names, []string{"A", "C"}) {
		t.Fatalf("after a racer left: %+v", sa)
	}
	// the owner leaves: C owns the room now
	s.leave(a)
	if sc, _ := lastStatus(t, c); !sc.Owner || sc.Joined != 1 {
		t.Fatalf("owner left: %+v", sc)
	}
}

// A room's creator chooses its settings over the server's defaults; another
// room on the same server keeps the defaults.
func TestRoomSettingsInStatus(t *testing.T) {
	env := envRace(t, map[string]string{"DAYTONA_DIFFICULTY": "easy"})
	s := newServer(env, "", 100, 1000, 16)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go s.serve(ln)

	want := env
	want.Difficulty, want.GameMode, want.FreePlay, want.Start, want.Country = "hardest", "endurance", "off", "anyone", "jpn"
	a, st := dial(t, ln.Addr().String(), proto.Hello{Op: "create", Room: "chosen", Name: "A", Game: "3",
		Settings: "difficulty:hardest,game_mode:endurance,free_play:off,start:anyone,country:jpn"})
	if st.Size != 0 || st.Joined != 1 || st.Position != 0 || st.Ready || !st.Owner || st.Racing {
		t.Errorf("first status %+v", st)
	}
	if !reflect.DeepEqual(st.EEPROM, want.EEPROM()) || !st.AnyStart {
		t.Errorf("chosen room: eeprom %v anyStart %v, want %v true", st.EEPROM, st.AnyStart, want.EEPROM())
	}
	// a joiner's set= is ignored: the room keeps its creator's
	b, st := dial(t, ln.Addr().String(), proto.Hello{Op: "join", Room: "chosen", Name: "B", Game: "3", Settings: "difficulty:easy"})
	if st.Ready || st.Owner || !reflect.DeepEqual(st.Names, []string{"A", "B"}) || !reflect.DeepEqual(st.EEPROM, want.EEPROM()) {
		t.Errorf("second status %+v", st)
	}
	if st := nextStatus(t, a); st.Ready || st.Joined != 2 || !st.AnyStart {
		t.Errorf("first player's update %+v", st)
	}
	// only the owner starts: B's Begin is ignored, A's forms the ring
	proto.WriteFrame(b, proto.Frame{Type: proto.TypeBegin})
	proto.WriteFrame(a, proto.Frame{Type: proto.TypeBegin})
	if st := nextStatus(t, a); !st.Ready || st.Position != 1 || st.Size != 2 || !st.Racing {
		t.Errorf("owner after Begin %+v", st)
	}
	if st := nextStatus(t, b); !st.Ready || st.Position != 2 || !reflect.DeepEqual(st.EEPROM, want.EEPROM()) {
		t.Errorf("second after Begin %+v", st)
	}

	_, st = dial(t, ln.Addr().String(), proto.Hello{Op: "create", Room: "plain", Name: "C", Game: "3"})
	if !reflect.DeepEqual(st.EEPROM, env.EEPROM()) || st.AnyStart {
		t.Errorf("default room: eeprom %v anyStart %v, want %v false", st.EEPROM, st.AnyStart, env.EEPROM())
	}
}

// op=info answers the defaults (after the password check) and closes.
func TestInfo(t *testing.T) {
	env := envRace(t, map[string]string{"DAYTONA_CABINET": "deluxe"})
	s := newServer(env, "pw", 100, 1000, 16)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go s.serve(ln)
	ask := func(pass string) proto.Frame {
		conn, err := net.Dial("tcp", ln.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		defer conn.Close()
		conn.Write([]byte(proto.Hello{Op: "info", Password: pass, Game: "6"}.Line()))
		conn.SetReadDeadline(time.Now().Add(5 * time.Second))
		f, err := proto.ReadFrame(bufio.NewReader(conn))
		if err != nil {
			t.Fatal(err)
		}
		return f
	}
	if f := ask("pw"); f.Type != proto.TypeInfo || string(f.Payload) != env.Info() {
		t.Errorf("info: %c %q", f.Type, f.Payload)
	}
	if f := ask("nope"); f.Type != proto.TypeError || !strings.HasPrefix(string(f.Payload), "password\n") {
		t.Errorf("wrong password: %c %q", f.Type, f.Payload)
	}
	if len(s.rooms) != 0 {
		t.Errorf("info opened a room")
	}
}

func TestCreateRules(t *testing.T) {
	s := newServer(envRace(t, nil), "", 2, 1000, 16)
	var e *proto.Error
	if err := s.join(fakeClient("A"), proto.Hello{Op: "create", Game: "9", Room: "bad", Settings: "difficulty:insane"}); !errors.As(err, &e) || e.Code != "settings" {
		t.Errorf("invalid setting: %v", err)
	}
	if len(s.rooms) != 0 {
		t.Errorf("a refused room stayed")
	}
	if err := s.join(fakeClient("A"), proto.Hello{Op: "create", Room: "one", Game: "3"}); err != nil {
		t.Fatal(err)
	}
	if err := s.join(fakeClient("A"), proto.Hello{Op: "create", Room: "one", Game: "3"}); !errors.As(err, &e) || e.Code != "exists" {
		t.Errorf("create over a room: %v", err)
	}
	if err := s.join(fakeClient("B"), proto.Hello{Op: "join", Room: "one", Game: "2"}); !errors.As(err, &e) || e.Code != "game_version" ||
		!strings.HasSuffix(string(proto.ErrorPayload(err)), "\nroom_online=3") {
		t.Errorf("another game version: %v", err)
	}
	for i := 1; i < proto.MaxPlayers; i++ {
		if err := s.join(fakeClient("B"), proto.Hello{Op: "join", Room: "one", Game: "3"}); err != nil {
			t.Fatalf("player %d: %v", i+1, err)
		}
	}
	if err := s.join(fakeClient("I"), proto.Hello{Op: "join", Room: "one", Game: "3"}); !errors.As(err, &e) || e.Code != "full" {
		t.Errorf("ninth in a room: %v", err)
	}
	if err := s.join(fakeClient("C"), proto.Hello{Op: "create", Game: "9", Room: "two"}); err != nil {
		t.Fatal(err)
	}
	if err := s.join(fakeClient("D"), proto.Hello{Op: "create", Game: "9", Room: "three"}); !errors.As(err, &e) || e.Code != "server_full" {
		t.Errorf("third room with DAYTONA_MAX_ROOMS=2: %v", err)
	}
}

// Pongs set a player's latency, smoothed; the room hears of it once it moves.
func TestLatency(t *testing.T) {
	s := newTestServer(t)
	a, b := fakeClient("A"), fakeClient("B")
	s.join(a, proto.Hello{Op: "create", Room: "lat", Game: "7"})
	s.join(b, proto.Hello{Op: "join", Room: "lat", Game: "7"})
	lastStatus(t, a)
	lastStatus(t, b)
	if st, _ := func() (proto.Status, uint32) {
		s.mu.Lock()
		s.notify(s.rooms["lat"])
		s.mu.Unlock()
		return lastStatus(t, a)
	}(); !reflect.DeepEqual(st.Latency, []int{-1, -1}) {
		t.Errorf("before any pong: %v", st.Latency)
	}
	a.rtt.Store(20_000)
	b.rtt.Store(45_400)
	s.refreshLatency()
	if st, _ := lastStatus(t, b); !reflect.DeepEqual(st.Latency, []int{20, 45}) {
		t.Errorf("measured: %v", st.Latency)
	}
	lastStatus(t, a)
	b.rtt.Store(46_000) // jitter: no new status
	s.refreshLatency()
	select {
	case f := <-a.out:
		t.Errorf("a status for 1 ms of jitter: %c", f.Type)
	default:
	}
	b.rtt.Store(60_000)
	s.refreshLatency()
	if st, _ := lastStatus(t, a); !reflect.DeepEqual(st.Latency, []int{20, 60}) {
		t.Errorf("moved: %v", st.Latency)
	}
}

// A pong over TCP: the stamp of the server's ping, sent back.
func TestPongOverTCP(t *testing.T) {
	s := newTestServer(t)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go s.serve(ln)
	p, _ := dial(t, ln.Addr().String(), proto.Hello{Op: "create", Room: "pong", Name: "A", Game: "7"})
	stamp := make([]byte, 8)
	binary.BigEndian.PutUint64(stamp, uint64(time.Now().Add(-30*time.Millisecond).UnixNano()))
	proto.WriteFrame(p, proto.Frame{Type: proto.TypePong, Payload: stamp})
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		s.mu.Lock()
		var ms int
		for _, m := range s.rooms["pong"].members {
			ms = m.latencyMs()
		}
		s.mu.Unlock()
		if ms >= 30 && ms < 1000 {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Error("the pong set no latency")
}

// Star mode: the block a player gets holds every other car's latest own
// slot, not the copy passed along the ring.
func TestStarCompose(t *testing.T) {
	s := newTestServer(t)
	s.star = true
	a, b, c := fakeClient("A"), fakeClient("B"), fakeClient("C")
	s.join(a, proto.Hello{Op: "create", Room: "star", Game: "8"})
	s.join(b, proto.Hello{Op: "join", Room: "star", Game: "8"})
	s.join(c, proto.Hello{Op: "join", Room: "star", Game: "8"})
	s.begin(a)
	_, epoch := lastStatus(t, a)
	lastStatus(t, b)
	lastStatus(t, c)
	// what each player's game sends: an id, its own slot filled with its letter, the rest as given
	block := func(own byte, rest byte) []byte {
		f := make([]byte, 1+8*linkcodec.Slot)
		for i := 0; i < linkcodec.Slot; i++ {
			f[1+i] = own
		}
		for i := linkcodec.Slot; i < 8*linkcodec.Slot; i++ {
			f[1+i] = rest
		}
		return f
	}
	var encB, encC linkcodec.Codec
	s.forward(b, proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: encB.Encode(nil, block('B', 0))})
	<-c.out // B's block goes to C
	// C's block carries old data of B in its slot 1 ('o'); A must get B's latest ('B')
	s.forward(c, proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: encC.Encode(nil, block('C', 'o'))})
	got := <-a.out
	var dec linkcodec.Codec
	frame, err := dec.Decode(got.Payload)
	if err != nil {
		t.Fatal(err)
	}
	slot := func(k int) byte { return frame[1+k*linkcodec.Slot] }
	if slot(0) != 'C' || slot(1) != 'B' || slot(2) != 'o' {
		t.Errorf("A got slots %c %c %c, want C B o", slot(0), slot(1), slot(2))
	}
}

// Star mode: a racer whose connection drops keeps its place; back with its
// key it takes it again, in the same ring (epoch).
func TestRejoin(t *testing.T) {
	s := newTestServer(t)
	s.star = true
	a, b := fakeClient("A"), fakeClient("B")
	s.join(a, proto.Hello{Op: "create", Room: "rj", Game: "9"})
	s.join(b, proto.Hello{Op: "join", Room: "rj", Game: "9"})
	s.begin(a)
	_, epoch := lastStatus(t, a)
	st, _ := lastStatus(t, b)
	if st.Key == "" || !st.Resume {
		t.Fatalf("status without key or resume: %+v", st)
	}
	s.leave(b) // the connection dropped
	s.mu.Lock()
	inRing := s.rooms["rj"].inRing(b)
	s.mu.Unlock()
	if inRing != 2 || !b.away {
		t.Fatalf("dropped racer not kept: place %d away %v", inRing, b.away)
	}
	var e *proto.Error
	if err := s.rejoin(fakeClient("X"), proto.Hello{Op: "rejoin", Game: "9", Room: "rj", Key: "wrong"}); !errors.As(err, &e) || e.Code != "not_found" {
		t.Errorf("rejoin with a wrong key: %v", err)
	}
	b2 := fakeClient("?")
	if err := s.rejoin(b2, proto.Hello{Op: "rejoin", Game: "9", Room: "rj", Key: st.Key}); err != nil {
		t.Fatal(err)
	}
	st2, epoch2 := lastStatus(t, b2)
	if epoch2 != epoch || st2.Position != 2 || !st2.Ready || b2.name != "B" {
		t.Errorf("back: epoch %d (was %d) %+v name %q", epoch2, epoch, st2, b2.name)
	}
	// without star mode the ring breaks at once, as before
	p := newTestServer(t)
	c, d := fakeClient("C"), fakeClient("D")
	p.join(c, proto.Hello{Op: "create", Room: "plain", Game: "9"})
	p.join(d, proto.Hello{Op: "join", Room: "plain", Game: "9"})
	p.begin(c)
	p.leave(d)
	if p.rooms["plain"].ring != nil {
		t.Error("ring kept without star mode")
	}
}

// Numbered data (udp=1): each number once, in order, whichever path brings
// it first; the next player gets them numbered from 0.
func TestNumberedInOrderOnce(t *testing.T) {
	s := newTestServer(t)
	a, b := fakeClient("A"), fakeClient("B")
	a.udp, b.udp = true, true
	s.join(a, proto.Hello{Op: "create", Room: "num", Game: "9", UDP: true})
	s.join(b, proto.Hello{Op: "join", Room: "num", Game: "9", UDP: true})
	s.begin(a)
	_, epoch := lastStatus(t, a)
	lastStatus(t, b)
	s.mu.Lock()
	for _, seq := range []uint32{1, 0, 0, 2, 1} { // out of order and twice, as TCP and UDP race
		s.receiveNumbered(a, epoch, seq, []byte{byte('x' + seq)}, false)
	}
	s.mu.Unlock()
	for want := uint32(0); want < 3; want++ {
		f := <-b.out
		if got := binary.BigEndian.Uint32(f.Payload); got != want || f.Payload[4] != byte('x'+want) {
			t.Fatalf("message %d: seq %d payload %q", want, got, f.Payload[4:])
		}
	}
	select {
	case f := <-b.out:
		t.Errorf("a duplicate went through: %v", f.Payload)
	default:
	}
}

// The race log's numbers: latency min/avg/max, the queue's longest, kept
// across a player's new connection (rejoin).
func TestRaceStats(t *testing.T) {
	var st raceStats
	for _, us := range []int64{220_000, 180_000, 260_000} {
		st.latency(us)
	}
	st.queued(3)
	st.queued(1)
	st.in.Add(5)
	if st.rttMin.Load() != 180_000 || st.rttMax.Load() != 260_000 || st.rttSum.Load()/st.rttN.Load() != 220_000 || st.queueMax.Load() != 3 {
		t.Fatalf("min %d max %d avg %d queue %d", st.rttMin.Load(), st.rttMax.Load(), st.rttSum.Load()/st.rttN.Load(), st.queueMax.Load())
	}
	var back raceStats
	back.copyFrom(&st)
	if back.in.Load() != 5 || back.rttN.Load() != 3 {
		t.Fatal("a rejoin lost the race's numbers")
	}
	st.reset()
	if st.in.Load() != 0 || st.rttN.Load() != 0 {
		t.Fatal("reset left numbers")
	}
}

// op=info's game_online: the newest game online version a race started with.
func TestNewestGame(t *testing.T) {
	s := newTestServer(t)
	if strings.Contains(s.info(), "game_online=") {
		t.Fatalf("game_online before any race: %q", s.info())
	}
	a, b := fakeClient("A"), fakeClient("B")
	if err := s.join(a, proto.Hello{Op: "create", Room: "new", Game: "12"}); err != nil {
		t.Fatal(err)
	}
	if strings.Contains(s.info(), "game_online=") {
		t.Fatalf("game_online from a room alone: %q", s.info())
	}
	if err := s.join(b, proto.Hello{Op: "join", Room: "new", Game: "12"}); err != nil {
		t.Fatal(err)
	}
	s.begin(a)
	if !strings.HasSuffix(s.info(), "game_online=12\n") {
		t.Fatalf("after a race: %q", s.info())
	}
	c, d := fakeClient("C"), fakeClient("D")
	s.join(c, proto.Hello{Op: "create", Room: "old", Game: "11"})
	s.join(d, proto.Hello{Op: "join", Room: "old", Game: "11"})
	s.begin(c)
	if !strings.HasSuffix(s.info(), "game_online=12\n") {
		t.Fatalf("an older race lowered it: %q", s.info())
	}
}

// Star mode: a car's slots reach another player in order, one per block,
// though they arrive at its own rate and leave at the sender's.
func TestStarHistory(t *testing.T) {
	s := newTestServer(t)
	s.star = true
	a, b, c := fakeClient("A"), fakeClient("B"), fakeClient("C")
	s.join(a, proto.Hello{Op: "create", Room: "hist", Game: "8"})
	s.join(b, proto.Hello{Op: "join", Room: "hist", Game: "8"})
	s.join(c, proto.Hello{Op: "join", Room: "hist", Game: "8"})
	s.begin(a)
	_, epoch := lastStatus(t, a)
	lastStatus(t, b)
	lastStatus(t, c)
	// ring A, B, C: B's blocks go to C, C's to A; A's slot 1 holds B's data, put by the relay
	block := func(tick byte) []byte {
		f := make([]byte, 1+8*linkcodec.Slot)
		for i := 0; i < linkcodec.Slot; i++ {
			f[1+i] = tick
		}
		return f
	}
	var encB, encC, dec linkcodec.Codec
	sendB := func(tick byte) {
		s.forward(b, proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: encB.Encode(nil, block(tick))})
		<-c.out
	}
	// what A gets in B's slot when C sends
	sendC := func() byte {
		s.forward(c, proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: encC.Encode(nil, block(99))})
		got := <-a.out
		frame, err := dec.Decode(got.Payload)
		if err != nil {
			t.Fatal(err)
		}
		return frame[1+linkcodec.Slot+tickOffset]
	}
	sendB(1)
	if got := sendC(); got != 1 {
		t.Fatalf("first: %d, want 1", got)
	}
	sendB(2)
	sendB(3) // two of B's between two of C's
	if got := sendC(); got != 2 {
		t.Fatalf("after two: %d, want 2 (in order)", got)
	}
	if got := sendC(); got != 3 {
		t.Fatalf("then: %d, want 3", got)
	}
	if got := sendC(); got != 3 {
		t.Fatalf("none new: %d, want 3 again", got)
	}
	for tick := byte(4); tick <= 9; tick++ { // six behind: only historyKeep kept behind the newest
		sendB(tick)
	}
	if got := sendC(); got != 9-defaultHistoryKeep {
		t.Fatalf("far behind: %d, want %d", got, 9-defaultHistoryKeep)
	}
	// off (DAYTONA_STAR_HISTORY=0): the latest, as before
	s.historyKeep = 0
	sendB(10)
	sendB(11)
	if got := sendC(); got != 11 {
		t.Fatalf("history off: %d, want the latest, 11", got)
	}
}

// The game's rooms need the game's online version, on joining and on coming
// back into a race (op=rejoin).
func TestVersionAlways(t *testing.T) {
	s := newTestServer(t)
	s.star = true
	var e *proto.Error
	if err := s.join(fakeClient("A"), proto.Hello{Op: "create", Room: "nov"}); !errors.As(err, &e) || e.Code != "game_version" {
		t.Errorf("create without v: %v", err)
	}
	a, b := fakeClient("A"), fakeClient("B")
	s.join(a, proto.Hello{Op: "create", Room: "rv", Game: "11"})
	s.join(b, proto.Hello{Op: "join", Room: "rv", Game: "11"})
	s.begin(a)
	lastStatus(t, a)
	st, _ := lastStatus(t, b)
	s.leave(b)
	if err := s.rejoin(fakeClient("B"), proto.Hello{Op: "rejoin", Room: "rv", Key: st.Key, Game: "10"}); !errors.As(err, &e) ||
		e.Code != "game_version" || !strings.HasSuffix(string(proto.ErrorPayload(err)), "\nroom_online=11") {
		t.Errorf("rejoin with another version: %v", err)
	}
	if err := s.rejoin(fakeClient("B"), proto.Hello{Op: "rejoin", Room: "rv", Key: st.Key, Game: "11"}); err != nil {
		t.Errorf("rejoin with the room's version: %v", err)
	}
}

// The own pace (pace.go): B late, C gets its blocks from the server once a
// frame, with A's newest data and B's standing; B's late blocks are then
// taken in but not sent, so C gets one block a frame, never a burst.
func TestStarPace(t *testing.T) {
	s := newTestServer(t)
	s.star, s.paceAfter = true, defaultPaceAfter
	clk := &clock{t: time.Unix(1000, 0)}
	s.now = clk.now
	a, b, c := fakeClient("A"), fakeClient("B"), fakeClient("C")
	s.join(a, proto.Hello{Op: "create", Room: "pace", Game: "11"})
	s.join(b, proto.Hello{Op: "join", Room: "pace", Game: "11"})
	s.join(c, proto.Hello{Op: "join", Room: "pace", Game: "11"})
	s.begin(a)
	_, epoch := lastStatus(t, a)
	lastStatus(t, b)
	lastStatus(t, c)
	block := func(tick byte) []byte {
		f := make([]byte, 1+8*linkcodec.Slot)
		for i := 0; i < linkcodec.Slot; i++ {
			f[1+i] = tick
		}
		return f
	}
	var encA, encB, encC, dec linkcodec.Codec
	s.forward(c, proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: encC.Encode(nil, block(0))}) // every player in the race's mode
	<-a.out
	sendA := func(tick byte) { // A's blocks go to B (or are taken in for the server's own, owed to B)
		s.forward(a, proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: encA.Encode(nil, block(tick))})
		select {
		case <-b.out:
		default:
		}
	}
	sendB := func(tick byte) {
		s.forward(b, proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: encB.Encode(nil, block(tick))})
	}
	// C's block: B's slot (0) and A's (1)
	got := func() (byte, byte, bool) {
		select {
		case f := <-c.out:
			frame, err := dec.Decode(f.Payload)
			if err != nil {
				t.Fatal(err)
			}
			return frame[1+tickOffset], frame[1+linkcodec.Slot+tickOffset], true
		default:
			return 0, 0, false
		}
	}
	tick := byte(0)
	for i := 0; i < paceSteady; i++ { // a steady race: one block each a frame
		tick++
		sendA(tick)
		sendB(tick)
		if _, _, ok := got(); !ok {
			t.Fatalf("block %d not passed on", i)
		}
		clk.add(framePeriod)
		s.paceTick()
		if _, _, ok := got(); ok {
			t.Fatalf("block %d: the server sent one with B on time", i)
		}
	}
	last := tick
	// B stalls: A goes on; nothing from the server before paceAfter
	tick++
	sendA(tick)
	clk.add(defaultPaceAfter - framePeriod - time.Millisecond)
	s.paceTick()
	if _, _, ok := got(); ok {
		t.Fatal("sent before paceAfter")
	}
	clk.add(2 * time.Millisecond)
	s.paceTick()
	bs, as, ok := got()
	if !ok || bs != last || as != tick {
		t.Fatalf("own pace: %v, B %d A %d, want B %d (standing) A %d (newest)", ok, bs, as, last, tick)
	}
	tick++
	sendA(tick)
	clk.add(framePeriod)
	s.paceTick()
	if bs, as, ok = got(); !ok || bs != last || as != tick {
		t.Fatalf("a frame later: %v, B %d A %d", ok, bs, as)
	}
	s.paceTick() // the same moment: not twice
	if _, _, ok = got(); ok {
		t.Fatal("two in one frame")
	}
	// B's two late blocks: taken in, not sent; its third goes on
	sendB(last + 1)
	sendB(last + 2)
	if _, _, ok = got(); ok {
		t.Fatal("a late block sent after the server's own")
	}
	sendB(last + 3)
	if bs, _, ok = got(); !ok || bs != last+1 { // B's own in order too: the one after its last sent
		t.Fatalf("after the owed ones: %v, B %d, want %d", ok, bs, last+1)
	}
	if n := c.race.paced.Load(); n != 2 {
		t.Errorf("paced count %d, want 2", n)
	}
	// B silent again: the server keeps the pace while B's place is kept
	// (paceActive), then stops; a frame of A's in another size (the race
	// ending) stops it at once
	sent := func(frames int) int64 {
		before := c.race.paced.Load()
		for i := 0; i < frames; i++ {
			clk.add(framePeriod)
			s.paceTick()
			got()
			for _, m := range []*client{a, b} { // A silent too: B gets the server's blocks as well
				select {
				case <-m.out:
				default:
				}
			}
		}
		return c.race.paced.Load() - before
	}
	if n := sent(300); n < 295 {
		t.Errorf("a 5 s stall: %d sent by the server, want about 298", n)
	}
	if n := sent(int(paceActive/framePeriod) + 60); n == 0 || n > int64(paceActive/framePeriod) {
		t.Errorf("past paceActive: %d sent", n)
	}
	if n := sent(60); n != 0 {
		t.Errorf("B gone: %d sent, want 0", n)
	}
	sendB(last + 4) // B back, at its pace: what was owed is cleared
	got()
	clk.add(framePeriod)
	sendB(last + 5)
	// the server's own blocks took B's backlog in order meanwhile (last+3);
	// B's first one back (last+4) is taken in for what was owed, then sent in order
	if bs, _, ok = got(); !ok || bs != last+4 {
		t.Fatalf("B back: %v, B %d, want %d", ok, bs, last+4)
	}
	for i := 0; i < paceSteady; i++ {
		clk.add(framePeriod)
		tick++
		sendA(tick)
		sendB(last + 6 + byte(i))
		got()
	}
	s.forward(a, proto.Frame{Type: proto.TypeData, Epoch: epoch, Payload: encA.Encode(nil, make([]byte, 1+4*linkcodec.Slot))})
	select {
	case <-b.out:
	default:
	}
	if n := sent(10); n != 0 {
		t.Errorf("A's blocks in another size: %d sent by the server, want 0", n)
	}
}
