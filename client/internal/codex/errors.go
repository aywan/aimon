package codex

import "errors"

// ErrNotInitialized is returned when limits have not been fetched yet.
var ErrNotInitialized = errors.New("codex limits not initialized")
