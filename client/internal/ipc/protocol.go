// Package ipc provides a local Unix-socket protocol between one-shot CLI
// commands (e.g. `hook`) and a long-running `run` daemon.
package ipc

import (
	"fmt"
	"strings"
)

const DefaultSocketPath = "/tmp/aiboard-client.sock"

// Command kinds exchanged over the socket (one line each).
const (
	CmdHook = "hook"
)

// Request is a single line: "<cmd> <arg...>" (e.g. "hook SessionStart").
type Request struct {
	Cmd  string
	Args []string
}

// ParseRequest parses one protocol line.
func ParseRequest(line string) (Request, error) {
	line = strings.TrimSpace(line)
	if line == "" {
		return Request{}, fmt.Errorf("empty request")
	}
	parts := strings.Fields(line)
	return Request{Cmd: parts[0], Args: parts[1:]}, nil
}

func (r Request) String() string {
	if len(r.Args) == 0 {
		return r.Cmd
	}
	return r.Cmd + " " + strings.Join(r.Args, " ")
}

// FormatOK / FormatError are response lines.
func FormatOK() string { return "OK" }

func FormatError(err error) string {
	return "ERROR: " + err.Error()
}
