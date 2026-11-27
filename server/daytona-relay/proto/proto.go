// Package proto is the wire format between the players (the game's Online
// mode, or the daytona-link helper) and daytona-relay (the server).
//
// The client opens one TCP connection and sends a single text line, values
// percent-encoded:
//
//	DAYTONA-RELAY 3 op=create room=k7f3q9xz name=Alex pass=segredo set=difficulty:hard,cabinet:twin\n
//
// op: info (no room: the server answers one Info frame, its race settings'
// defaults, then closes; a wrong password is refused as for any op), create
// (the room must not exist; its creator owns it, and set= overrides the
// server's race settings for it), join (it must exist) or auto (create or
// join, with players=N: the daytona-link helper's fixed-size rooms, which
// start on their own once N have joined).
//
// A room takes up to MaxPlayers. Its owner (the creator; the next in line
// when the owner leaves) starts a
// race with a Begin frame: everyone in the room at that moment forms the
// ring, the owner first (the MASTER). Who joins later waits in the room for
// the next race; a new Begin forms a new ring with everyone.
//
// After that both sides exchange frames:
//
//	type (1 byte) | epoch (4 bytes, big endian) | length (2 bytes, big endian) | payload
//
// Epoch numbers the room's ring: it changes whenever someone joins or leaves,
// so data sent for an old ring is dropped instead of reaching the wrong game.
//
// Status payload (server -> client), text lines:
//
//	joined=2 size=3 pos=1 ready=0
//	eeprom=19:00,1a:01,1b:01,1c:00,1d:00,20:01
//	name=Alex
//	name=Jo%C3%A3o
//
// eeprom: the race settings every player's game applies (offset:value in the
// I/O board's settings EEPROM, hexadecimal); names in ring order.
package proto

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"unicode"
)

const (
	Magic      = "DAYTONA-RELAY"
	Version    = 5
	MaxPlayers = 8
	MinPlayers = 2
	MaxPayload = 16 * 1024
	MaxName    = 20

	TypeData    byte = 'D' // game bytes: client -> server (from its game), server -> client (for its game)
	TypeStatus  byte = 'S' // server -> client: room state, see Status
	TypePing    byte = 'P' // keepalive, both directions
	TypeError   byte = 'E' // server -> client: fatal error text, then the server closes
	TypeControl byte = 'C' // client -> server -> every other member: the games' own coordination (course choice)
	TypeBegin   byte = 'G' // client -> server: the room's owner starts a race with everyone in the room
	TypeInfo    byte = 'I' // server -> client, answering op=info: "key=value" lines, the race settings' defaults
	TypePong    byte = 'O' // client -> server: a ping's payload (the server's time stamp) sent straight back
)

var roomRe = regexp.MustCompile(`^[a-z0-9_-]{1,32}$`)

// Error is a refusal with a code: an E frame's payload is "code\ntext",
// then any "key=value" lines of Detail, and the game shows its own text for
// the code, in the player's language (a game that does not know a detail
// line skips it: it reads the code only).
// Codes: protocol, version, room, players, password, not_found, exists,
// full, size, server_full, game_version, too_many, settings.
// Details: version: server_protocol=N (this server's Version); game_version:
// room_online=V (the room's game online version), so that the game can say
// which side has to update.
type Error struct {
	Code, Text string
	Detail     []string
}

func (e *Error) Error() string { return e.Text }

func Errf(code, format string, a ...any) error {
	return &Error{Code: code, Text: fmt.Sprintf(format, a...)}
}

// ErrDetail: Errf with "key=value" detail lines.
func ErrDetail(code string, detail []string, format string, a ...any) error {
	return &Error{Code: code, Text: fmt.Sprintf(format, a...), Detail: detail}
}

func ErrorPayload(err error) []byte {
	code := "other"
	var e *Error
	if errors.As(err, &e) {
		code = e.Code
	}
	out := code + "\n" + err.Error()
	if e != nil {
		for _, d := range e.Detail {
			out += "\n" + d
		}
	}
	return []byte(out)
}

// ErrorText: the text of an E frame's payload, without its code or details.
func ErrorText(payload []byte) string {
	if _, text, ok := strings.Cut(string(payload), "\n"); ok {
		text, _, _ = strings.Cut(text, "\n")
		return text
	}
	return string(payload)
}

// NormalizeRoom lowercases a room code and checks it.
func NormalizeRoom(s string) (string, error) {
	r := strings.ToLower(strings.TrimSpace(s))
	if !roomRe.MatchString(r) {
		return "", Errf("room", "invalid room code: use 1 to 32 letters, digits, - or _")
	}
	return r, nil
}

// CleanName keeps a player's name printable and short.
func CleanName(s string) string {
	var b strings.Builder
	n := 0
	for _, r := range strings.TrimSpace(s) {
		if n >= MaxName {
			break
		}
		if unicode.IsControl(r) || r == unicode.ReplacementChar {
			continue
		}
		b.WriteRune(r)
		n++
	}
	return strings.TrimSpace(b.String())
}

type Hello struct {
	Op       string // info, create, join, auto
	Room     string // empty for info
	Players  int    // auto only: the helper's fixed room size
	Name     string
	Password string
	Game     string // the game's online version ("v"); a room only takes one; empty: the daytona-link helper
	Settings string // create only: "key:value,..." over the server's race settings (package settings)
	Key      string // rejoin only: the key the player's status gave (Status.Key)
	UDP      bool   // udp=1: this player's game data is numbered (4-byte sequence before each D payload) and also comes and goes by UDP
}

func (h Hello) Line() string {
	var b strings.Builder
	fmt.Fprintf(&b, "%s %d op=%s", Magic, Version, h.Op)
	if h.Room != "" {
		fmt.Fprintf(&b, " room=%s", url.PathEscape(h.Room))
	}
	if h.Players > 0 {
		fmt.Fprintf(&b, " players=%d", h.Players)
	}
	if h.Name != "" {
		fmt.Fprintf(&b, " name=%s", url.PathEscape(h.Name))
	}
	if h.Password != "" {
		fmt.Fprintf(&b, " pass=%s", url.PathEscape(h.Password))
	}
	if h.Game != "" {
		fmt.Fprintf(&b, " v=%s", url.PathEscape(h.Game))
	}
	if h.Settings != "" {
		fmt.Fprintf(&b, " set=%s", url.PathEscape(h.Settings))
	}
	if h.Key != "" {
		fmt.Fprintf(&b, " key=%s", url.PathEscape(h.Key))
	}
	if h.UDP {
		b.WriteString(" udp=1")
	}
	return b.String() + "\n"
}

func ParseHello(line string) (Hello, error) {
	parts := strings.Fields(line)
	if len(parts) < 3 || parts[0] != Magic {
		return Hello{}, Errf("protocol", "unknown protocol")
	}
	if v, _ := strconv.Atoi(parts[1]); v != Version {
		return Hello{}, ErrDetail("version", []string{"server_protocol=" + strconv.Itoa(Version)},
			"client version %s is not compatible with this server (version %d)", parts[1], Version)
	}
	var h Hello
	for _, kv := range parts[2:] {
		k, v, ok := strings.Cut(kv, "=")
		if !ok {
			continue
		}
		val, err := url.PathUnescape(v)
		if err != nil {
			return Hello{}, Errf("protocol", "invalid field %s", k)
		}
		switch k {
		case "op":
			h.Op = val
		case "room":
			h.Room = val
		case "players":
			h.Players, _ = strconv.Atoi(val)
		case "name":
			h.Name = val
		case "pass":
			h.Password = val
		case "v":
			h.Game = val
		case "set":
			h.Settings = val
		case "key":
			h.Key = val
		case "udp":
			h.UDP = val == "1"
		}
	}
	switch h.Op {
	case "info":
		h.Room, h.Players, h.Settings = "", 0, ""
		return h, nil
	case "create", "join", "auto", "rejoin":
	default:
		return Hello{}, Errf("protocol", "unknown operation")
	}
	room, err := NormalizeRoom(h.Room)
	if err != nil {
		return Hello{}, err
	}
	h.Room = room
	if h.Op == "auto" && (h.Players < MinPlayers || h.Players > MaxPlayers) {
		return Hello{}, Errf("players", "players must be %d to %d", MinPlayers, MaxPlayers)
	}
	h.Name = CleanName(h.Name)
	return h, nil
}

// MaxHello is the longest greeting line the server reads.
const MaxHello = 512

// ReadLine reads one line of at most max bytes (without the newline); a
// longer one is an error as soon as it passes max, not when it ends.
func ReadLine(r *bufio.Reader, max int) (string, error) {
	var b []byte
	for {
		c, err := r.ReadByte()
		if err != nil {
			return "", err
		}
		if c == '\n' {
			return string(b), nil
		}
		if len(b) >= max {
			return "", errors.New("line too long")
		}
		b = append(b, c)
	}
}

type Frame struct {
	Type    byte
	Epoch   uint32
	Payload []byte
}

const headerSize = 7

func WriteFrame(w io.Writer, f Frame) error {
	if len(f.Payload) > MaxPayload {
		return errors.New("payload too large")
	}
	buf := make([]byte, headerSize+len(f.Payload))
	buf[0] = f.Type
	binary.BigEndian.PutUint32(buf[1:5], f.Epoch)
	binary.BigEndian.PutUint16(buf[5:7], uint16(len(f.Payload)))
	copy(buf[headerSize:], f.Payload)
	_, err := w.Write(buf)
	return err
}

func ReadFrame(r io.Reader) (Frame, error) {
	var h [headerSize]byte
	if _, err := io.ReadFull(r, h[:]); err != nil {
		return Frame{}, err
	}
	n := int(binary.BigEndian.Uint16(h[5:7]))
	if n > MaxPayload {
		return Frame{}, errors.New("payload too large")
	}
	f := Frame{Type: h[0], Epoch: binary.BigEndian.Uint32(h[1:5])}
	if n > 0 {
		f.Payload = make([]byte, n)
		if _, err := io.ReadFull(r, f.Payload); err != nil {
			return Frame{}, err
		}
	}
	return f, nil
}

// Setting: one byte of the race settings EEPROM.
type Setting struct {
	Offset, Value byte
}

// Status is what the server tells every member when the room changes.
type Status struct {
	Joined   int  // players in the room now
	Size     int  // players in the race's ring; 0 while no race is formed
	Position int  // this client's place in the ring, 1-based (1 = MASTER); 0: not in it
	Ready    bool // this client is in a formed ring: open the game's connections
	EEPROM   []Setting
	// Names: the ring's players in ring order while a race is formed (car
	// 1, 2...); else everyone in the room, in the order a race would take.
	Names []string
	// AnyStart: any player may start a race (the others join it); else only
	// the MASTER, the first in the ring.
	AnyStart bool
	Owner    bool     // this client may start a race (Begin)
	Racing   bool     // a race's ring is formed
	Waiting  []string // in the room but not in the race: the next one
	Me       int      // this client's line in Names, 1-based; 0: in Waiting
	// Latency to the server, round trip in ms, as Names and Waiting; -1: not measured yet.
	Latency, WaitingLatency []int
	// Key: this player's, to come back into its race after a drop (op=rejoin);
	// Resume: the server keeps a dropped racer's place for a while (star mode).
	Key    string
	Resume bool
}

func (s Status) Encode() []byte {
	ready := 0
	if s.Ready {
		ready = 1
	}
	var b strings.Builder
	fmt.Fprintf(&b, "joined=%d size=%d pos=%d ready=%d\n", s.Joined, s.Size, s.Position, ready)
	b.WriteString("eeprom=")
	for i, e := range s.EEPROM {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, "%02x:%02x", e.Offset, e.Value)
	}
	b.WriteByte('\n')
	for _, n := range s.Names {
		fmt.Fprintf(&b, "name=%s\n", url.PathEscape(n))
	}
	if s.AnyStart {
		b.WriteString("start=anyone\n")
	} else {
		b.WriteString("start=master\n")
	}
	if s.Owner {
		b.WriteString("owner=1\n")
	}
	if s.Racing {
		b.WriteString("racing=1\n")
	}
	if s.Me > 0 {
		fmt.Fprintf(&b, "me=%d\n", s.Me)
	}
	for _, n := range s.Waiting {
		fmt.Fprintf(&b, "waiting=%s\n", url.PathEscape(n))
	}
	writeInts := func(key string, v []int) {
		if len(v) == 0 {
			return
		}
		b.WriteString(key + "=")
		for i, n := range v {
			if i > 0 {
				b.WriteByte(',')
			}
			if n >= 0 {
				b.WriteString(strconv.Itoa(n))
			}
		}
		b.WriteByte('\n')
	}
	writeInts("lat", s.Latency)
	writeInts("wlat", s.WaitingLatency)
	if s.Key != "" {
		fmt.Fprintf(&b, "key=%s\n", s.Key)
	}
	if s.Resume {
		b.WriteString("resume=1\n")
	}
	return []byte(b.String())
}

func DecodeStatus(p []byte) (Status, error) {
	var s Status
	lines := strings.Split(strings.TrimRight(string(p), "\n"), "\n")
	var ready int
	if _, err := fmt.Sscanf(lines[0], "joined=%d size=%d pos=%d ready=%d", &s.Joined, &s.Size, &s.Position, &ready); err != nil {
		return s, err
	}
	s.Ready = ready == 1
	for _, l := range lines[1:] {
		k, v, _ := strings.Cut(l, "=")
		switch k {
		case "eeprom":
			for _, pair := range strings.Split(v, ",") {
				o, val, ok := strings.Cut(pair, ":")
				if !ok {
					continue
				}
				oi, err1 := strconv.ParseUint(o, 16, 8)
				vi, err2 := strconv.ParseUint(val, 16, 8)
				if err1 == nil && err2 == nil {
					s.EEPROM = append(s.EEPROM, Setting{byte(oi), byte(vi)})
				}
			}
		case "name":
			n, _ := url.PathUnescape(v)
			s.Names = append(s.Names, n)
		case "start":
			s.AnyStart = v == "anyone"
		case "owner":
			s.Owner = v == "1"
		case "racing":
			s.Racing = v == "1"
		case "me":
			s.Me, _ = strconv.Atoi(v)
		case "key":
			s.Key = v
		case "resume":
			s.Resume = v == "1"
		case "lat", "wlat":
			var list []int
			for _, x := range strings.Split(v, ",") {
				n, err := strconv.Atoi(x)
				if err != nil {
					n = -1
				}
				list = append(list, n)
			}
			if k == "lat" {
				s.Latency = list
			} else {
				s.WaitingLatency = list
			}
		case "waiting":
			n, _ := url.PathUnescape(v)
			s.Waiting = append(s.Waiting, n)
		}
	}
	return s, nil
}
