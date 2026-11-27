package main

// The addresses to give the other players on a local network, shown when
// the server starts (a player running it on their own computer reads the
// one to send their friends there). Inside a container (Docker) the
// computer's own addresses are not seen: nothing is shown.

import (
	"fmt"
	"net"
	"os"
	"strings"
)

type ifaceAddr struct {
	name string
	ip   net.IP
}

// lanAddresses: this computer's private IPv4 addresses with the port, not
// the loopback nor the virtual bridges (Docker's, VirtualBox's, libvirt's).
func lanAddresses(addrs []ifaceAddr, port int) []string {
	var out []string
	for _, a := range addrs {
		ip := a.ip.To4()
		if ip == nil || ip.IsLoopback() || !ip.IsPrivate() {
			continue
		}
		virtual := false
		for _, prefix := range []string{"docker", "br-", "veth", "virbr", "vboxnet", "vmnet", "lxc", "cni", "flannel", "podman"} {
			if strings.HasPrefix(a.name, prefix) {
				virtual = true
			}
		}
		if !virtual {
			out = append(out, fmt.Sprintf("%s:%d", ip, port))
		}
	}
	return out
}

func localAddresses() []ifaceAddr {
	var out []ifaceAddr
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	for _, ifc := range ifaces {
		if ifc.Flags&net.FlagUp == 0 {
			continue
		}
		addrs, err := ifc.Addrs()
		if err != nil {
			continue
		}
		for _, a := range addrs {
			if n, ok := a.(*net.IPNet); ok {
				out = append(out, ifaceAddr{ifc.Name, n.IP})
			}
		}
	}
	return out
}

func inContainer() bool {
	_, err := os.Stat("/.dockerenv")
	return err == nil
}
