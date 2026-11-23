// Package invite: the invite code the game's Online mode decodes, made by the
// web interface. The same format as the game's app::online::encode_invite
// (src/app/online.cpp):
//
//	"DTN1" + Crockford base32 of (server "\n" room "\n" password, then the
//	CRC-16/XMODEM of those bytes, big endian), in groups of 5 after dashes.
//
// Decoding takes any case, with or without dashes and spaces, O for 0 and
// I or L for 1; a code whose check does not match is refused.
package invite

import (
	"errors"
	"strconv"
	"strings"

	"daytona-relay/proto"
)

const (
	alphabet    = "0123456789ABCDEFGHJKMNPQRSTVWXYZ" // Crockford: no I L O U
	prefix      = "DTN1"
	DefaultPort = 7777
)

// Invite is what a friend needs to join: the server, the room, the server's
// password.
type Invite struct {
	Server   string // host:port
	Room     string
	Password string
}

var ErrInvalid = errors.New("invalid invite code")

func base32(b []byte) string {
	var out strings.Builder
	var buf uint32
	bits := 0
	for _, c := range b {
		buf = buf<<8 | uint32(c)
		bits += 8
		for bits >= 5 {
			out.WriteByte(alphabet[(buf>>(bits-5))&31])
			bits -= 5
		}
	}
	if bits > 0 {
		out.WriteByte(alphabet[(buf<<(5-bits))&31])
	}
	return out.String()
}

func symbol(c byte) int {
	if c >= 'a' && c <= 'z' {
		c -= 'a' - 'A'
	}
	switch c {
	case 'O':
		c = '0'
	case 'I', 'L':
		c = '1'
	}
	return strings.IndexByte(alphabet, c)
}

func unbase32(s string) ([]byte, bool) {
	var out []byte
	var buf uint32
	bits := 0
	for i := 0; i < len(s); i++ {
		v := symbol(s[i])
		if v < 0 {
			return nil, false
		}
		buf = buf<<5 | uint32(v)
		bits += 5
		if bits >= 8 {
			out = append(out, byte(buf>>(bits-8)))
			bits -= 8
		}
	}
	return out, true
}

// crc16 is CRC-16/XMODEM (polynomial 0x1021, initial 0).
func crc16(b []byte) uint16 {
	var v uint16
	for _, c := range b {
		v ^= uint16(c) << 8
		for i := 0; i < 8; i++ {
			if v&0x8000 != 0 {
				v = v<<1 ^ 0x1021
			} else {
				v <<= 1
			}
		}
	}
	return v
}

// Encode makes the code; the fields are used as given (see NormalizeServer).
func Encode(in Invite) string {
	b := []byte(in.Server + "\n" + in.Room + "\n" + in.Password)
	sum := crc16(b)
	b = append(b, byte(sum>>8), byte(sum))
	body := base32(b)
	var out strings.Builder
	out.WriteString(prefix)
	for i := 0; i < len(body); i += 5 {
		out.WriteByte('-')
		out.WriteString(body[i:min(i+5, len(body))])
	}
	return out.String()
}

// Decode reads a code as the game does, and refuses one the game would.
func Decode(code string) (Invite, error) {
	var text []byte
	for i := 0; i < len(code); i++ {
		c := code[i]
		if c >= '0' && c <= '9' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' {
			text = append(text, c)
		}
	}
	if len(text) <= 4 || strings.ToUpper(string(text[:4])) != prefix {
		return Invite{}, ErrInvalid
	}
	b, ok := unbase32(string(text[4:]))
	if !ok || len(b) < 2 {
		return Invite{}, ErrInvalid
	}
	sum := uint16(b[len(b)-2])<<8 | uint16(b[len(b)-1])
	b = b[:len(b)-2]
	if crc16(b) != sum {
		return Invite{}, ErrInvalid
	}
	parts := strings.SplitN(string(b), "\n", 3)
	if len(parts) != 3 {
		return Invite{}, ErrInvalid
	}
	in := Invite{Server: parts[0], Room: parts[1], Password: parts[2]}
	if _, ok := NormalizeServer(in.Server); !ok {
		return Invite{}, ErrInvalid
	}
	if r, err := proto.NormalizeRoom(in.Room); err != nil || r != in.Room {
		return Invite{}, ErrInvalid
	}
	return in, nil
}

// NormalizeServer: host or host:port (IPv6 in brackets, or bare) to
// host:port, DefaultPort when none, as the game's normalize_server.
func NormalizeServer(s string) (string, bool) {
	s = strings.Join(strings.Fields(s), "")
	if s == "" || strings.ContainsAny(s, "\n\x00") {
		return "", false
	}
	host, port := s, ""
	switch n := strings.Count(s, ":"); {
	case s[0] == '[':
		end := strings.IndexByte(s, ']')
		if end < 0 {
			return "", false
		}
		host = s[:end+1]
		if rest := s[end+1:]; rest != "" {
			if rest[0] != ':' {
				return "", false
			}
			port = rest[1:]
		}
	case n == 1:
		host, port, _ = strings.Cut(s, ":")
	case n > 1:
		host = "[" + s + "]"
	}
	if host == "" || host == "[]" {
		return "", false
	}
	p := DefaultPort
	if port != "" {
		if len(port) > 5 || strings.Trim(port, "0123456789") != "" {
			return "", false
		}
		p, _ = strconv.Atoi(port)
		if p < 1 || p > 65535 {
			return "", false
		}
	}
	return host + ":" + strconv.Itoa(p), true
}
