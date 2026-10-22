# Shortcuts for the Swift package commands in the README.
#   make        build bluebox (release) and build/Blue Box.app
#   make test   run the tests
#   make clean  remove the build products and build/Blue Box.app
#   make release VERSION=1.2
#               build, sign and zip a release (scripts/release.sh); with
#               NOTARY_PROFILE set in the environment, also notarize it and
#               update releases/appcast.xml

.PHONY: all test clean release

all:
	sh scripts/make-app.sh

test:
	swift test

clean:
	swift package clean
	rm -rf build

release:
	@if [ -z "$(VERSION)" ]; then \
		echo "usage: make release VERSION=<version>   (notarizes when NOTARY_PROFILE is set)" >&2; \
		exit 2; \
	fi
	sh scripts/release.sh "$(VERSION)"
