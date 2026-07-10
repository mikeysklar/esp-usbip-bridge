BOARD ?= p4-function-ev
PORT ?=

.PHONY: build flash clean jscheck

# Syntax-check the embedded browser JS (main/web_app.js).
# Uses node if available; otherwise silently skips.
jscheck:
	@command -v node >/dev/null 2>&1 && node --check main/web_app.js \
		&& echo "jscheck: OK" \
		|| { echo "jscheck: node not found, skipping"; }

build:
	./scripts/build-board.sh $(BOARD) build $(PORT)

flash:
	./scripts/build-board.sh $(BOARD) flash $(PORT)

clean:
	./scripts/build-board.sh $(BOARD) clean
