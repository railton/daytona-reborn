// Package proto is the wire format between the players (the game's Online
// mode, or the daytona-link helper) and daytona-relay (the server).
//
// The client opens one TCP connection and sends a single text line, values
// percent-encoded:
//
//	DAYTONA-RELAY 1 op=create room=k7f3q9xz players=3 name=Railton pass=segredo\n
//
// op: create (the room must not exist), join (it must exist; players is then
// not needed) or auto (create or join; the daytona-link helper).
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
//	name=Railton
//	name=Jo%C3%A3o
//
// eeprom: the race settings every player's game applies (offset:value in the
// I/O board's settings EEPROM, hexadecimal); names in ring order.
package proto

import (
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
	Version    = 1
	MaxPlayers = 8
	MinPlayers = 2
	MaxPayload = 16 * 1024
	MaxName    = 20

	TypeData   byte = 'D' // game bytes: client -> server (from its game), server -> client (for its game)
	TypeStatus byte = 'S' // server -> client: room state, see Status
	TypePing   byte = 'P' // keepalive, both directions
	TypeError  byte = 'E' // server -> client: fatal error text, then the server closes
)

var roomRe = regexp.MustCompile(`^[a-z0-9_-]{1,32}$`)

// Error is a refusal with a code: an E frame's payload is "code\ntext", and
// the game shows its own text for the code, in the player's language.
// Codes: protocol, version, room, players, password, not_found, exists,
// full, size, server_full.
type Error struct{ Code, Text string }

func (e *Error) Error() string { return e.Text }

func Errf(code, format string, a ...any) error { return &Error{code, fmt.Sprintf(format, a...)} }

func ErrorPayload(err error) []byte {
	code := "other"
	var e *Error
	if errors.As(err, &e) {
		code = e.Code
	}
	return []byte(code + "\n" + err.Error())
}

// ErrorText: the text of an E frame's payload, without its code.
func ErrorText(payload []byte) string {
	if _, text, ok := strings.Cut(string(payload), "\n"); ok {
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
	Op       string // create, join, auto
	Room     string
	Players  int // create and auto
	Name     string
	Password string
}

func (h Hello) Line() string {
	var b strings.Builder
	fmt.Fprintf(&b, "%s %d op=%s room=%s", Magic, Version, h.Op, url.PathEscape(h.Room))
	if h.Players > 0 {
		fmt.Fprintf(&b, " players=%d", h.Players)
	}
	if h.Name != "" {
		fmt.Fprintf(&b, " name=%s", url.PathEscape(h.Name))
	}
	if h.Password != "" {
		fmt.Fprintf(&b, " pass=%s", url.PathEscape(h.Password))
	}
	return b.String() + "\n"
}

func ParseHello(line string) (Hello, error) {
	parts := strings.Fields(line)
	if len(parts) < 3 || parts[0] != Magic {
		return Hello{}, Errf("protocol", "unknown protocol")
	}
	if v, _ := strconv.Atoi(parts[1]); v != Version {
		return Hello{}, Errf("version", "client version %s is not compatible with this server (version %d)", parts[1], Version)
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
		}
	}
	switch h.Op {
	case "create", "join", "auto":
	default:
		return Hello{}, Errf("protocol", "unknown operation")
	}
	room, err := NormalizeRoom(h.Room)
	if err != nil {
		return Hello{}, err
	}
	h.Room = room
	if h.Op != "join" && (h.Players < MinPlayers || h.Players > MaxPlayers) {
		return Hello{}, Errf("players", "players must be %d to %d", MinPlayers, MaxPlayers)
	}
	h.Name = CleanName(h.Name)
	return h, nil
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
	Size     int  // players the room waits for
	Position int  // this client's place in the ring, 1-based (1 = MASTER)
	Ready    bool // the ring is complete: open the game's connections
	EEPROM   []Setting
	Names    []string // ring order
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
		}
	}
	return s, nil
}
