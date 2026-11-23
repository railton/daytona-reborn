package invite

import (
	"strings"
	"testing"
)

// Codes made by the game's own encode_invite (src/app/online.cpp), compiled
// and run on these fields: the server must produce the same, byte for byte.
var gameVectors = []struct {
	in   Invite
	code string
}{
	{Invite{"vps.exemplo.com.br:7777", "k7f3q9xz", "s3nh@ forte"},
		"DTN1-ESR76-BK5F1-JPTW3-CDWQ6-6VVD5-SH74E-HQ6WV-KE2KB-6XK36-W9SF1-X0MWS-KDSM4-0836D-XS78S-FWCW"},
	{Invite{"127.0.0.1:17777", "race1", ""},
		"DTN1-64S3E-BHG5R-R2WC9-T64VK-EDSQ1-9S62R-V5645-BSD0"},
	{Invite{"[2001:db8::1]:7777", "a_b-c", "páss"},
		"DTN1-BCS30-C1H79-J64E1-T78RN-TEHQ6-WVKE2-K1BXH-2TRRA-E31T2-WVKXR-2G"},
}

func TestGameVectors(t *testing.T) {
	for _, v := range gameVectors {
		if got := Encode(v.in); got != v.code {
			t.Errorf("Encode(%q)\n got %s\nwant %s", v.in, got, v.code)
		}
		got, err := Decode(v.code)
		if err != nil || got != v.in {
			t.Errorf("Decode(%s) = %q, %v", v.code, got, err)
		}
	}
}

func TestDecodeLenient(t *testing.T) {
	v := gameVectors[0]
	messy := strings.ToLower(strings.ReplaceAll(v.code, "-", " "))
	if got, err := Decode("  " + messy + " "); err != nil || got != v.in {
		t.Errorf("lowercase with spaces: %q, %v", got, err)
	}
	// O for 0, I and L for 1
	alias := strings.NewReplacer("0", "O", "1", "l").Replace(v.code[4:])
	if got, err := Decode("DTN1" + alias); err != nil || got != v.in {
		t.Errorf("aliases: %q, %v", got, err)
	}
}

func TestDecodeRefuses(t *testing.T) {
	code := gameVectors[0].code
	typo := []byte(code)
	if typo[10] == 'A' {
		typo[10] = 'B'
	} else {
		typo[10] = 'A'
	}
	for _, bad := range []string{
		"", "DTN1-", "XXXX-ABCDE", "DTN2" + code[4:],
		code[:len(code)-7], // cut short
		string(typo),       // one wrong character
		"DTN1-UUUUU",       // U is not in the alphabet
		Encode(Invite{"host:7777", "Sala Ruim!", ""}), // the game refuses such a room
		Encode(Invite{"host:99999", "room", ""}),
		Encode(Invite{"", "room", ""}),
	} {
		if got, err := Decode(bad); err == nil {
			t.Errorf("Decode(%q) accepted: %q", bad, got)
		}
	}
}

func TestNormalizeServer(t *testing.T) {
	for in, want := range map[string]string{
		"vps.example.com":    "vps.example.com:7777",
		" 1.2.3.4 : 9000 ":   "1.2.3.4:9000",
		"[2001:db8::1]":      "[2001:db8::1]:7777",
		"[2001:db8::1]:8000": "[2001:db8::1]:8000",
		"2001:db8::1":        "[2001:db8::1]:7777",
		"host:65535":         "host:65535",
		"":                   "",
		":7777":              "",
		"host:0":             "",
		"host:123456":        "",
		"host:12a":           "",
		"[2001:db8::1]x":     "",
		"[2001:db8::1":       "",
	} {
		got, ok := NormalizeServer(in)
		if got != want || ok != (want != "") {
			t.Errorf("NormalizeServer(%q) = %q, %v; want %q", in, got, ok, want)
		}
	}
}
