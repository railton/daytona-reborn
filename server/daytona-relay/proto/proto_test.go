package proto

import (
	"bufio"
	"bytes"
	"errors"
	"reflect"
	"strings"
	"testing"
)

func TestHelloRoundTrip(t *testing.T) {
	in := Hello{Op: "create", Room: "k7f3q9xz", Name: "João da Silva", Password: "a b%c", Game: "6",
		Settings: "difficulty:hard,cabinet:deluxe"}
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
		"DAYTONA-RELAY 2 op=join room=x", // the version before room settings from the game
		"DAYTONA-RELAY 3 op=steal room=x",
		"DAYTONA-RELAY 3 op=auto room=x players=9",
		"DAYTONA-RELAY 3 op=auto room=x players=1",
		"DAYTONA-RELAY 3 op=join room=bad%20room",
	} {
		if _, err := ParseHello(line); err == nil {
			t.Errorf("accepted %q", line)
		}
	}
	if h, err := ParseHello("DAYTONA-RELAY 3 op=join room=ABC"); err != nil || h.Room != "abc" {
		t.Errorf("join without players: %+v %v", h, err)
	}
	if h, err := ParseHello("DAYTONA-RELAY 3 op=create room=abc"); err != nil || h.Players != 0 {
		t.Errorf("create without players: %+v %v", h, err)
	}
	if h, err := ParseHello("DAYTONA-RELAY 3 op=info pass=x room=y set=a:b"); err != nil || h.Op != "info" || h.Room != "" || h.Settings != "" || h.Password != "x" {
		t.Errorf("info: %+v %v", h, err)
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
	in := Status{Joined: 3, Size: 2, Position: 2, Ready: true,
		EEPROM: []Setting{{0x1a, 1}, {0x20, 3}}, Names: []string{"Railton", "João"}, AnyStart: true,
		Owner: true, Racing: true, Waiting: []string{"Ana Maria"}, Me: 2}
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

func TestReadLine(t *testing.T) {
	r := bufio.NewReader(strings.NewReader("short\n" + strings.Repeat("x", 600) + "\n"))
	if l, err := ReadLine(r, 512); err != nil || l != "short" {
		t.Fatalf("%q %v", l, err)
	}
	if _, err := ReadLine(r, 512); err == nil {
		t.Fatal("accepted a 600-byte line")
	}
}
