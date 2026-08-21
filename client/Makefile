APP := aiboard-client.app
BIN := $(APP)/Contents/MacOS/aiboard-client
JSON ?=
JSON_FILE ?= examples/all_slots.json
HOLD ?=
HOOK ?=

.PHONY: build send status run hook

build:
	./build_macos.sh

# Usage (run this from inside client/, not the project root — the root
# Makefile also has a "run" target that flashes the firmware instead):
#   make send
#   make send JSON='{"slot_1":{"type":"gauge","value":50,"label":"codex"}}'
#   make send HOLD=15s
#   make run
#   make hook HOOK=SessionStart
#
# Do not put "#RRGGBB" in an unquoted Makefile JSON= default: '#' starts a
# Make comment and truncates the payload (unexpected end of JSON input).
send: build
ifeq ($(JSON),)
	exec $(BIN) send --json "$$(tr -d '\n' < $(JSON_FILE))" $(if $(HOLD),--hold $(HOLD))
else
	exec $(BIN) send --json '$(JSON)' $(if $(HOLD),--hold $(HOLD))
endif

status: build
	exec $(BIN) status

# Poll Codex every minute onto slots 1–3; slot 4 is hook status text.
# Also listens on a Unix socket for `hook` commands. Ctrl+C to stop.
run: build
	exec $(BIN) run

# Deliver a hook to a running `make run` daemon, e.g.:
#   make hook HOOK=SessionStart
#   make hook HOOK=Stop
hook: build
	@test -n "$(HOOK)" || (echo "usage: make hook HOOK=SessionStart" >&2; exit 1)
	exec $(BIN) hook '$(HOOK)'
