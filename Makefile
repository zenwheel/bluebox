# Shortcuts for the Swift package commands in the README.
#   make        build bluebox (release) and build/Blue Box.app
#   make test   run the tests
#   make clean  remove the build products and build/Blue Box.app

.PHONY: all test clean

all:
	sh scripts/make-app.sh

test:
	swift test

clean:
	swift package clean
	rm -rf build
