package codex

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// FindBinary locates the codex executable.
func FindBinary() (string, error) {
	if configured := os.Getenv("CODEX_BINARY"); configured != "" {
		if _, err := os.Stat(configured); err == nil {
			return configured, nil
		}
	}

	if path, err := exec.LookPath("codex"); err == nil {
		return path, nil
	}

	home, _ := os.UserHomeDir()

	candidates := []string{
		"/Applications/ChatGPT.app/Contents/Resources/codex",
		filepath.Join(home, "Applications/ChatGPT.app/Contents/Resources/codex"),
		filepath.Join(home, ".codex/plugins/.plugin-appserver/codex"),
		"/opt/homebrew/bin/codex",
		"/usr/local/bin/codex",
	}

	for _, candidate := range candidates {
		info, err := os.Stat(candidate)
		if err == nil && !info.IsDir() && info.Mode()&0111 != 0 {
			return candidate, nil
		}
	}

	return "", fmt.Errorf("codex executable not found; set CODEX_BINARY to its absolute path")
}
