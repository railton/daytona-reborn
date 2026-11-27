package main

import (
	"net"
	"reflect"
	"testing"
)

func TestLanAddresses(t *testing.T) {
	addrs := []ifaceAddr{
		{"lo", net.ParseIP("127.0.0.1")},
		{"eth0", net.ParseIP("192.168.0.10")},
		{"wlan0", net.ParseIP("10.0.0.5")},
		{"docker0", net.ParseIP("172.17.0.1")},
		{"br-1a2b", net.ParseIP("172.18.0.1")},
		{"eth1", net.ParseIP("8.8.8.8")},          // public: not a local network's
		{"eth0", net.ParseIP("fe80::1")},          // IPv6
		{"Ethernet 2", net.ParseIP("172.16.5.4")}, // Windows names its adapters so
	}
	got := lanAddresses(addrs, 7777)
	want := []string{"192.168.0.10:7777", "10.0.0.5:7777", "172.16.5.4:7777"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}
