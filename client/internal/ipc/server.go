package ipc

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"strings"
	"sync"
	"time"

	"go.uber.org/zap"
)

// HookHandler is called when a `hook <name>` request arrives.
type HookHandler func(name string) error

// Server accepts hook (and future) commands from one-shot CLI processes.
type Server struct {
	log        *zap.SugaredLogger
	socketPath string
	onHook     HookHandler

	mu     sync.Mutex
	ln     net.Listener
	closed bool
}

// NewServer creates an IPC server. Call Serve to start accepting.
func NewServer(log *zap.SugaredLogger, socketPath string, onHook HookHandler) *Server {
	if log == nil {
		log = zap.NewNop().Sugar()
	}
	if socketPath == "" {
		socketPath = DefaultSocketPath
	}
	return &Server{
		log:        log.Named("ipc"),
		socketPath: socketPath,
		onHook:     onHook,
	}
}

// Serve listens until Close. Safe to call from a goroutine.
func (s *Server) Serve() error {
	_ = os.Remove(s.socketPath)

	ln, err := net.Listen("unix", s.socketPath)
	if err != nil {
		return fmt.Errorf("listen %s: %w", s.socketPath, err)
	}

	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		_ = ln.Close()
		_ = os.Remove(s.socketPath)
		return fmt.Errorf("server already closed")
	}
	s.ln = ln
	s.mu.Unlock()

	s.log.Infof("listening on %s", s.socketPath)

	for {
		conn, err := ln.Accept()
		if err != nil {
			s.mu.Lock()
			closed := s.closed
			s.mu.Unlock()
			if closed {
				return nil
			}
			return fmt.Errorf("accept: %w", err)
		}
		go s.handleConn(conn)
	}
}

// Close stops the listener and removes the socket file.
func (s *Server) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil
	}
	s.closed = true
	var err error
	if s.ln != nil {
		err = s.ln.Close()
	}
	_ = os.Remove(s.socketPath)
	return err
}

func (s *Server) handleConn(conn net.Conn) {
	defer conn.Close()

	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	scanner := bufio.NewScanner(conn)
	if !scanner.Scan() {
		return
	}

	req, err := ParseRequest(scanner.Text())
	if err != nil {
		fmt.Fprintln(conn, FormatError(err))
		return
	}

	if err := s.dispatch(req); err != nil {
		s.log.Warnf("request %q failed: %v", req, err)
		fmt.Fprintln(conn, FormatError(err))
		return
	}
	fmt.Fprintln(conn, FormatOK())
}

func (s *Server) dispatch(req Request) error {
	switch req.Cmd {
	case CmdHook:
		if len(req.Args) != 1 {
			return fmt.Errorf("usage: hook <name>")
		}
		if s.onHook == nil {
			return fmt.Errorf("hooks not accepted by this process")
		}
		name := req.Args[0]
		s.log.Infof("hook received: %s", name)
		return s.onHook(name)
	default:
		return fmt.Errorf("unknown command %q", req.Cmd)
	}
}

// SendRequest dials the daemon socket and sends one request line.
// Returns (false, nil) when no daemon is listening.
func SendRequest(socketPath string, req Request) (handled bool, err error) {
	if socketPath == "" {
		socketPath = DefaultSocketPath
	}

	conn, dialErr := net.DialTimeout("unix", socketPath, 500*time.Millisecond)
	if dialErr != nil {
		return false, nil
	}
	defer conn.Close()

	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := fmt.Fprintf(conn, "%s\n", req.String()); err != nil {
		return true, fmt.Errorf("write request: %w", err)
	}

	resp, readErr := bufio.NewReader(conn).ReadString('\n')
	resp = strings.TrimSpace(resp)
	if readErr != nil {
		return true, fmt.Errorf("daemon response: %w", readErr)
	}
	if strings.HasPrefix(resp, "ERROR") {
		return true, fmt.Errorf("%s", resp)
	}
	if resp != FormatOK() {
		return true, fmt.Errorf("unexpected daemon response: %q", resp)
	}
	return true, nil
}
