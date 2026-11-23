// daytona-link: runs next to the game on each player's computer. It joins a
// room on the daytona-relay server and, once the room is full, bridges the
// game's link-play connections to it:
//
//   - the game's "next cabinet" is this program (127.0.0.1:15200 by default):
//     what the game sends goes to the server, which passes it to the next
//     player;
//   - this program connects to the game's own port (15112 by default) and
//     delivers what the previous player sent.
//
// Only outgoing connections leave the computer: no router port to open.
//
//	daytona-link -server vps.example.com:7777 -room race1 -players 3
package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"daytona-relay/proto"
)

const (
	pingEvery   = 10 * time.Second
	idleTimeout = 30 * time.Second
	retryServer = 3 * time.Second
	retryGame   = 500 * time.Millisecond
)

type options struct {
	server    string
	room      string
	players   int
	password  string
	name      string
	gamePort  int // the launcher's "This cabinet's port"
	localPort int // where the launcher's "Next cabinet" points
}

// session: the bridge for one complete ring (one epoch). Stopped when the
// room changes or the server connection drops.
type session struct {
	epoch  uint32
	opts   options
	toSrv  chan<- proto.Frame
	inbox  chan []byte // bytes from the previous player, for the game
	stopCh chan struct{}
	once   sync.Once
	mu     sync.Mutex
	conns  []net.Conn
	ln     net.Listener
}

func startSession(epoch uint32, opts options, toSrv chan<- proto.Frame) (*session, error) {
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", opts.localPort))
	if err != nil {
		return nil, fmt.Errorf("cannot use local port %d (already in use?): %v", opts.localPort, err)
	}
	s := &session{epoch: epoch, opts: opts, toSrv: toSrv, inbox: make(chan []byte, 4096), stopCh: make(chan struct{}), ln: ln}
	go s.fromGame()
	go s.toGame()
	return s, nil
}

func (s *session) track(c net.Conn) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	select {
	case <-s.stopCh:
		c.Close()
		return false
	default:
	}
	s.conns = append(s.conns, c)
	return true
}

func (s *session) stop() {
	s.once.Do(func() {
		close(s.stopCh)
		s.ln.Close()
		s.mu.Lock()
		for _, c := range s.conns {
			c.Close()
		}
		s.mu.Unlock()
	})
}

func (s *session) stopped() bool {
	select {
	case <-s.stopCh:
		return true
	default:
		return false
	}
}

// fromGame: the game connects here as to its next cabinet; its bytes go to
// the server.
func (s *session) fromGame() {
	for {
		c, err := s.ln.Accept()
		if err != nil {
			return // listener closed: session over
		}
		if tc, ok := c.(*net.TCPConn); ok {
			tc.SetNoDelay(true)
		}
		if !s.track(c) {
			return
		}
		log.Printf("game connected (sending)")
		buf := make([]byte, proto.MaxPayload)
		for {
			n, err := c.Read(buf)
			if n > 0 {
				p := make([]byte, n)
				copy(p, buf[:n])
				select {
				case s.toSrv <- proto.Frame{Type: proto.TypeData, Epoch: s.epoch, Payload: p}:
				case <-s.stopCh:
					return
				}
			}
			if err != nil {
				break
			}
		}
		c.Close()
		if s.stopped() {
			return
		}
		log.Printf("the game closed its sending connection; waiting for it again")
	}
}

// toGame: connects to the game's own port and writes what the previous
// player sent. Bytes that arrive before the game is up wait in the inbox.
func (s *session) toGame() {
	addr := fmt.Sprintf("127.0.0.1:%d", s.opts.gamePort)
	warned := false
	for !s.stopped() {
		c, err := net.DialTimeout("tcp", addr, 2*time.Second)
		if err != nil {
			if !warned {
				log.Printf("waiting for the game to listen on port %d (tick \"Link to other cabinets\" in the launcher)", s.opts.gamePort)
				warned = true
			}
			select {
			case <-time.After(retryGame):
			case <-s.stopCh:
				return
			}
			continue
		}
		if tc, ok := c.(*net.TCPConn); ok {
			tc.SetNoDelay(true)
		}
		if !s.track(c) {
			return
		}
		log.Printf("game connected (receiving)")
		// the game never sends on this connection; reading tells us when it closes
		closed := make(chan struct{})
		go func() { io.Copy(io.Discard, c); close(closed) }()
	write:
		for {
			select {
			case p := <-s.inbox:
				c.SetWriteDeadline(time.Now().Add(5 * time.Second))
				if _, err := c.Write(p); err != nil {
					break write
				}
			case <-closed:
				break write
			case <-s.stopCh:
				c.Close()
				return
			}
		}
		c.Close()
		if s.stopped() {
			return
		}
		log.Printf("the game closed its receiving connection; connecting again")
		warned = false
	}
}

func (s *session) deliver(p []byte) {
	select {
	case s.inbox <- p:
	case <-s.stopCh:
	default:
		log.Printf("the game is not reading what arrives; dropping it (start the game with link play on)")
	}
}

// run: one connection to the server, until it drops. Returns a fatal error
// (wrong password, room full...) or nil to reconnect.
func run(opts options) (fatal error) {
	conn, err := net.DialTimeout("tcp", opts.server, 10*time.Second)
	if err != nil {
		log.Printf("cannot connect to server %s: %v", opts.server, err)
		return nil
	}
	defer conn.Close()
	if tc, ok := conn.(*net.TCPConn); ok {
		tc.SetNoDelay(true)
		tc.SetKeepAlive(true)
	}
	if _, err := io.WriteString(conn, proto.Hello{Op: "auto", Room: opts.room, Players: opts.players, Name: opts.name, Password: opts.password}.Line()); err != nil {
		return nil
	}
	log.Printf("connected to server %s, room %q (%d players)", opts.server, opts.room, opts.players)

	toSrv := make(chan proto.Frame, 1024)
	done := make(chan struct{})
	defer close(done)
	go func() { // the only writer to conn
		bw := bufio.NewWriterSize(conn, 32*1024)
		ping := time.NewTicker(pingEvery)
		defer ping.Stop()
		for {
			var f proto.Frame
			select {
			case f = <-toSrv:
			case <-ping.C:
				f = proto.Frame{Type: proto.TypePing}
			case <-done:
				return
			}
			conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if proto.WriteFrame(bw, f) != nil {
				conn.Close()
				return
			}
			for n := len(toSrv); n > 0; n-- {
				if proto.WriteFrame(bw, <-toSrv) != nil {
					conn.Close()
					return
				}
			}
			if bw.Flush() != nil {
				conn.Close()
				return
			}
		}
	}()

	var cur *session
	defer func() {
		if cur != nil {
			cur.stop()
		}
	}()
	br := bufio.NewReaderSize(conn, 32*1024)
	for {
		conn.SetReadDeadline(time.Now().Add(idleTimeout))
		f, err := proto.ReadFrame(br)
		if err != nil {
			log.Printf("connection to the server lost: %v", err)
			return nil
		}
		switch f.Type {
		case proto.TypeError:
			return fmt.Errorf("server: %s", proto.ErrorText(f.Payload))
		case proto.TypeStatus:
			st, err := proto.DecodeStatus(f.Payload)
			if err != nil {
				continue
			}
			if cur != nil {
				cur.stop()
				cur = nil
				log.Printf("the room changed: the game's connections are closed (if the race dropped, Reset in the launcher)")
			}
			names := make([]string, len(st.Names))
			for i, n := range st.Names {
				if n == "" {
					n = fmt.Sprintf("Player %d", i+1)
				}
				names[i] = n
			}
			log.Printf("players: %s", strings.Join(names, ", "))
			if !st.Ready {
				log.Printf("room %q: %d of %d players; waiting for the others...", opts.room, st.Joined, st.Size)
				continue
			}
			s, err := startSession(f.Epoch, opts, toSrv)
			if err != nil {
				return err
			}
			cur = s
			role := "SLAVE"
			if st.Position == 1 {
				role = "MASTER"
			}
			log.Printf("room %q full: %d players. In the test menu (F2) > GAME SYSTEM set LINK ID %s and CAR NUMBER %d. Start the game.", opts.room, st.Size, role, st.Position)
		case proto.TypeData:
			if cur != nil && f.Epoch == cur.epoch {
				cur.deliver(f.Payload)
			}
		case proto.TypePing:
		}
	}
}

func ask(r *bufio.Reader, prompt string) string {
	fmt.Print(prompt)
	s, _ := r.ReadString('\n')
	return strings.TrimSpace(s)
}

func main() {
	var o options
	flag.StringVar(&o.server, "server", "", "daytona-relay server address (host:port)")
	flag.StringVar(&o.room, "room", "", "room code (letters, digits, - or _)")
	flag.IntVar(&o.players, "players", 0, "players in the room (2 to 8)")
	flag.StringVar(&o.password, "password", "", "the server's password, if it has one")
	flag.StringVar(&o.name, "name", "", "your name, shown to the other players")
	flag.IntVar(&o.gamePort, "game-port", 15112, "the launcher's \"This cabinet's port\"")
	flag.IntVar(&o.localPort, "local-port", 15200, "local port the launcher's \"Next cabinet\" points to")
	flag.Parse()
	log.SetFlags(log.Ltime)

	// double-clicked on Windows: ask instead of failing
	in := bufio.NewReader(os.Stdin)
	if o.server == "" {
		o.server = ask(in, "Server (host:port): ")
	}
	if o.room == "" {
		o.room = ask(in, "Room code: ")
	}
	for o.players < proto.MinPlayers || o.players > proto.MaxPlayers {
		n, _ := strconv.Atoi(ask(in, "Players in the room (2 to 8): "))
		o.players = n
	}
	room, err := proto.NormalizeRoom(o.room)
	if err != nil {
		log.Fatal(err)
	}
	o.room = room
	if _, _, err := net.SplitHostPort(o.server); err != nil {
		log.Fatalf("invalid server %q: use host:port", o.server)
	}

	fmt.Printf("\nIn the game's launcher (Game tab):\n")
	fmt.Printf("  - tick \"Link to other cabinets\"\n")
	fmt.Printf("  - This cabinet's port: %d\n", o.gamePort)
	fmt.Printf("  - Next cabinet:        127.0.0.1:%d\n", o.localPort)
	fmt.Printf("Each player's LINK ID and CAR NUMBER show when the room is full.\n")
	fmt.Printf("(The game's Online mode sets them on its own.)\n\n")

	for {
		if err := run(o); err != nil {
			log.Printf("%v", err)
			if len(os.Args) == 1 { // keep the window open on Windows
				ask(in, "Press Enter to quit.")
			}
			os.Exit(1)
		}
		time.Sleep(retryServer)
		log.Printf("connecting to the server again...")
	}
}
