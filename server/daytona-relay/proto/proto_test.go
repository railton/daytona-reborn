package proto

import (
	"bytes"
	"errors"
	"reflect"
	"strings"
	"testing"
)

func TestHelloRoundTrip(t *testing.T) {
	in := Hello{Op: "create", Room: "k7f3q9xz", Players: 3, Name: "João da Silva", Password: "a b%c"}
	out, err := ParseHello(in.Line())
	if err != nil {
		t.Fatal(err)
	}
	if out != in {
		t.Fatalf("got %+v want %+v", out, in)
	}
}

func TestHelloRejects(t *testing.T) {
	for _, line := range []string{
		"HTTP/1.1 GET /",
		"DAYTONA-RELAY 2 op=join room=x",
		"DAYTONA-RELAY 1 op=steal room=x",
		"DAYTONA-RELAY 1 op=create room=x players=9",
		"DAYTONA-RELAY 1 op=create room=x players=1",
		"DAYTONA-RELAY 1 op=join room=bad%20room",
	} {
		if _, err := ParseHello(line); err == nil {
			t.Errorf("accepted %q", line)
		}
	}
	if h, err := ParseHello("DAYTONA-RELAY 1 op=join room=ABC"); err != nil || h.Room != "abc" {
		t.Errorf("join without players: %+v %v", h, err)
	}
}

func TestCleanName(t *testing.T) {
	if got := CleanName("  Ana\x00\n Maria  "); got != "Ana Maria" {
		t.Errorf("got %q", got)
	}
	if got := CleanName("abcdefghijklmnopqrstuvwxyz"); len([]rune(got)) != MaxName {
		t.Errorf("not cut: %q", got)
	}
}

func TestStatusRoundTrip(t *testing.T) {
	in := Status{Joined: 2, Size: 3, Position: 2, Ready: false,
		EEPROM: []Setting{{0x1a, 1}, {0x20, 3}}, Names: []string{"Railton", "João"}}
	out, err := DecodeStatus(in.Encode())
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(in, out) {
		t.Fatalf("got %+v want %+v", out, in)
	}
}

func TestFrameRoundTrip(t *testing.T) {
	var b bytes.Buffer
	in := Frame{Type: TypeData, Epoch: 0x01020304, Payload: []byte{1, 2, 3}}
	if err := WriteFrame(&b, in); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(b.Bytes(), []byte{'D', 1, 2, 3, 4, 0, 3, 1, 2, 3}) {
		t.Fatalf("wire % x", b.Bytes())
	}
	out, err := ReadFrame(&b)
	if err != nil || !reflect.DeepEqual(in, out) {
		t.Fatalf("got %+v %v", out, err)
	}
}

func TestErrorPayload(t *testing.T) {
	_, err := ParseHello("DAYTONA-RELAY 9 op=join room=x")
	p := ErrorPayload(err)
	if !strings.HasPrefix(string(p), "version\n") || ErrorText(p) != err.Error() {
		t.Fatalf("payload %q", p)
	}
	if string(ErrorPayload(errors.New("x"))) != "other\nx" {
		t.Fatal("uncoded error")
	}
}
