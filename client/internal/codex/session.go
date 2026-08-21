package codex

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"go.uber.org/zap"
)

const (
	defaultSocketName = "app-server.sock"
	socketWaitTimeout = 15 * time.Second
	rpcTimeout        = 15 * time.Second
)

// Session owns a long-lived codex app-server process and a WebSocket
// connection over its Unix socket.
type Session struct {
	log      *zap.SugaredLogger
	sockPath string
	binPath  string

	cmd  *exec.Cmd
	conn *websocket.Conn

	writeMu sync.Mutex
	nextID  atomic.Int64

	pendingMu sync.Mutex
	pending   map[int64]chan rpcMessage

	notifyMu sync.Mutex
	onLimits func(rateLimitsResult)

	readDone chan struct{}
	closed   atomic.Bool
}

// SessionOption configures a Session.
type SessionOption func(*Session)

func withSocketPath(path string) SessionOption {
	return func(s *Session) {
		s.sockPath = path
	}
}

// NewSession prepares a Session; call Start to launch app-server and connect.
func NewSession(log *zap.SugaredLogger, opts ...SessionOption) (*Session, error) {
	if log == nil {
		log = zap.NewNop().Sugar()
	}

	bin, err := FindBinary()
	if err != nil {
		return nil, err
	}

	sockPath, err := defaultSocketPath()
	if err != nil {
		return nil, err
	}

	s := &Session{
		log:      log.Named("session"),
		sockPath: sockPath,
		binPath:  bin,
		pending:  make(map[int64]chan rpcMessage),
		readDone: make(chan struct{}),
	}
	for _, opt := range opts {
		opt(s)
	}
	return s, nil
}

func defaultSocketPath() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("resolve home: %w", err)
	}
	dir := filepath.Join(home, ".codex", "aiboard-client")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", fmt.Errorf("create socket dir: %w", err)
	}
	return filepath.Join(dir, defaultSocketName), nil
}

// SetLimitsNotify registers a callback for account/rateLimits/updated.
func (s *Session) SetLimitsNotify(fn func(rateLimitsResult)) {
	s.notifyMu.Lock()
	defer s.notifyMu.Unlock()
	s.onLimits = fn
}

// Start launches app-server, connects over Unix WebSocket, and initializes.
func (s *Session) Start(ctx context.Context) error {
	if err := s.startProcess(); err != nil {
		return err
	}
	if err := s.waitForSocket(ctx); err != nil {
		s.killProcess()
		return err
	}
	if err := s.dialWS(ctx); err != nil {
		s.killProcess()
		return err
	}

	go s.readLoop()

	if err := s.initialize(ctx); err != nil {
		_ = s.Close()
		return err
	}
	return nil
}

func (s *Session) startProcess() error {
	_ = os.Remove(s.sockPath)

	if err := os.MkdirAll(filepath.Dir(s.sockPath), 0o700); err != nil {
		return fmt.Errorf("create socket dir: %w", err)
	}

	listenURL := "unix://" + s.sockPath
	s.cmd = exec.Command(s.binPath, "app-server", "--listen", listenURL)
	s.cmd.Stderr = os.Stderr
	s.log.Infof("starting app-server: %s --listen %s", s.binPath, listenURL)
	if err := s.cmd.Start(); err != nil {
		return fmt.Errorf("start app-server: %w", err)
	}
	go func() {
		err := s.cmd.Wait()
		if !s.closed.Load() {
			s.log.Warnf("app-server exited: %v", err)
		}
	}()
	return nil
}

func (s *Session) waitForSocket(ctx context.Context) error {
	deadline := time.Now().Add(socketWaitTimeout)
	if d, ok := ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}

	for {
		if fi, err := os.Stat(s.sockPath); err == nil && fi.Mode()&os.ModeSocket != 0 {
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("timed out waiting for socket %s", s.sockPath)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(50 * time.Millisecond):
		}
	}
}

func (s *Session) dialWS(ctx context.Context) error {
	dialer := websocket.Dialer{
		NetDialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
			var d net.Dialer
			return d.DialContext(ctx, "unix", s.sockPath)
		},
		HandshakeTimeout: 10 * time.Second,
	}

	header := http.Header{}
	conn, resp, err := dialer.DialContext(ctx, "ws://localhost/rpc", header)
	if err != nil {
		if resp != nil {
			return fmt.Errorf("websocket dial: %w (HTTP %s)", err, resp.Status)
		}
		return fmt.Errorf("websocket dial: %w", err)
	}
	s.conn = conn
	s.log.Infof("connected to app-server via %s", s.sockPath)
	return nil
}

func (s *Session) initialize(ctx context.Context) error {
	_, err := s.call(ctx, "initialize", map[string]any{
		"clientInfo": map[string]string{
			"name":    "aiboard-client",
			"title":   "AIBoard Client",
			"version": "1.0.0",
		},
	})
	if err != nil {
		return fmt.Errorf("initialize: %w", err)
	}

	if err := s.notify("initialized", map[string]any{}); err != nil {
		return fmt.Errorf("initialized: %w", err)
	}
	return nil
}

// ReadRateLimits requests account/rateLimits/read over the live connection.
func (s *Session) ReadRateLimits(ctx context.Context) (rateLimitsResult, error) {
	raw, err := s.call(ctx, "account/rateLimits/read", nil)
	if err != nil {
		return rateLimitsResult{}, err
	}
	var result rateLimitsResult
	if err := json.Unmarshal(raw, &result); err != nil {
		return rateLimitsResult{}, fmt.Errorf("parse rate limits: %w", err)
	}
	return result, nil
}

func (s *Session) call(ctx context.Context, method string, params any) (json.RawMessage, error) {
	if s.closed.Load() || s.conn == nil {
		return nil, fmt.Errorf("session closed")
	}

	id := s.nextID.Add(1)
	ch := make(chan rpcMessage, 1)

	s.pendingMu.Lock()
	s.pending[id] = ch
	s.pendingMu.Unlock()
	defer func() {
		s.pendingMu.Lock()
		delete(s.pending, id)
		s.pendingMu.Unlock()
	}()

	msg := map[string]any{
		"method": method,
		"id":     id,
	}
	if params != nil {
		msg["params"] = params
	}

	if err := s.writeJSON(msg); err != nil {
		return nil, err
	}

	timeout := rpcTimeout
	if deadline, ok := ctx.Deadline(); ok {
		if remaining := time.Until(deadline); remaining > 0 && remaining < timeout {
			timeout = remaining
		}
	}

	select {
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-time.After(timeout):
		return nil, fmt.Errorf("rpc %s timed out", method)
	case resp := <-ch:
		if len(resp.Error) > 0 {
			return nil, fmt.Errorf("app-server error: %s", resp.Error)
		}
		return resp.Result, nil
	}
}

func (s *Session) notify(method string, params any) error {
	msg := map[string]any{
		"method": method,
		"params": params,
	}
	return s.writeJSON(msg)
}

func (s *Session) writeJSON(v any) error {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	if s.conn == nil {
		return fmt.Errorf("not connected")
	}
	return s.conn.WriteJSON(v)
}

func (s *Session) readLoop() {
	defer close(s.readDone)

	for {
		_, data, err := s.conn.ReadMessage()
		if err != nil {
			if !s.closed.Load() {
				s.log.Warnf("websocket read: %v", err)
			}
			return
		}

		var msg rpcMessage
		if err := json.Unmarshal(data, &msg); err != nil {
			s.log.Debugf("skip non-json frame: %v", err)
			continue
		}

		if msg.ID != nil {
			id, err := msg.ID.Int64()
			if err == nil {
				s.pendingMu.Lock()
				ch := s.pending[id]
				s.pendingMu.Unlock()
				if ch != nil {
					select {
					case ch <- msg:
					default:
					}
				}
			}
			continue
		}

		if msg.Method == "account/rateLimits/updated" {
			var params rateLimitsResult
			if err := json.Unmarshal(msg.Params, &params); err != nil {
				s.log.Debugf("rateLimits/updated parse: %v", err)
				continue
			}
			s.notifyMu.Lock()
			fn := s.onLimits
			s.notifyMu.Unlock()
			if fn != nil {
				fn(params)
			}
		}
	}
}

// Close shuts down the WebSocket and app-server process.
func (s *Session) Close() error {
	if !s.closed.CompareAndSwap(false, true) {
		return nil
	}

	s.writeMu.Lock()
	if s.conn != nil {
		_ = s.conn.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""))
		_ = s.conn.Close()
		s.conn = nil
	}
	s.writeMu.Unlock()

	select {
	case <-s.readDone:
	case <-time.After(2 * time.Second):
	}

	s.killProcess()
	_ = os.Remove(s.sockPath)
	return nil
}

func (s *Session) killProcess() {
	if s.cmd == nil || s.cmd.Process == nil {
		return
	}
	_ = s.cmd.Process.Kill()
}
