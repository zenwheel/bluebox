#!/bin/sh
# M9's exit check: Open Transport applications connect over TCP/IP and over
# AppleTalk, headless, with nothing leaving this host.
#
#  - TCP/IP: Fetch 4.0.3 (PowerPC, OTInetClientLib) logs in to an FTP
#    server on 127.0.0.1:2121 (pyftpdlib, run through uv) and lists a
#    folder.
#  - AppleTalk: two copies of the Shadow Warrior demo, set up for an
#    AppleTalk game by its SW Net Startup, find each other through NBP
#    (Open Transport's mapper) and exchange DDP datagrams (ddp endpoints)
#    on Blue Box's loopback AppleTalk (--appletalk local).
#
# The applications are not part of the repository: give the Fetch 4.0.3
# application (or the Fetch disk image it is on) and the Shadow Warrior
# Demo folder.  Every run has its HOME in the scratch folder.
#
# usage: sh scripts/ot-exit-check.sh <bluebox> <Fetch 4.0.3> <Shadow Warrior Demo folder> <scratch folder>
set -eu

[ $# -eq 4 ] || {
	echo "usage: $0 <bluebox> <Fetch 4.0.3> <Shadow Warrior Demo folder> <scratch folder>" >&2
	exit 2
}
bluebox=$1
fetch=$2
sw=$3
scratch=$4
mkdir -p "$scratch"
scratch=$(cd "$scratch" && pwd)
status=0

# Steps for --keys that do nothing (the pointer moved to a corner), 5
# million instructions apart: a program that waits before its window
# appears uses them up, and the steps after them come once it is there.
filler() {
	i=0
	k=""
	while [ "$i" -lt "$1" ]; do
		k="${k}wait:5000000,move:1000/700,"
		i=$((i + 1))
	done
	printf '%s' "$k"
}

# ---- TCP/IP: Fetch and an FTP server on the loopback interface

ftp="$scratch/ftp"
mkdir -p "$ftp/pub" "$scratch/home-fetch"
printf 'hello from the host\n' >"$ftp/pub/hello.txt"
cat >"$scratch/ftpd.py" <<'EOF'
import sys
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer
auth = DummyAuthorizer()
auth.add_anonymous(sys.argv[1])
FTPHandler.authorizer = auth
FTPServer(("127.0.0.1", int(sys.argv[2])), FTPHandler).serve_forever()
EOF
uv run -q --with pyftpdlib python -I "$scratch/ftpd.py" "$ftp" 2121 >"$scratch/ftpd.log" 2>&1 &
ftpd=$!
trap 'kill "$ftpd" 2>/dev/null || true' EXIT INT TERM
tries=0
until nc -z 127.0.0.1 2121 2>/dev/null; do
	tries=$((tries + 1))
	[ "$tries" -lt 60 ] || {
		echo "FAIL: the FTP server did not start (see $scratch/ftpd.log)"
		exit 1
	}
	sleep 1
done

set -- --app "Fetch 4.0.3"
case $fetch in
*.img | *.dmg | *.image) ;;
*) set -- ;;
esac
# Past the welcome window ("Try"), then a URL with the port in New
# Connection's host field, the user and the password.
keys="$(filler 30)click:202/210,$(filler 20)click:300/152,text:ftp://127.0.0.1:2121/pub/,tab,text:anonymous,tab,text:bluebox@localhost,return,$(filler 100)"
HOME="$scratch/home-fetch" "$bluebox" run "$fetch" "$@" --ppc --headless --realtime --max 1000000000 \
	--log traps --keys "$keys" --screenshot "$scratch/fetch.png" 2>&1 |
	grep -a 'OT: connect' >"$scratch/fetch.log" || true
if grep -q '127.0.0.1:2121' "$scratch/fetch.log" && grep -q "USER 'anonymous' logged in" "$scratch/ftpd.log" &&
	grep -q 'CWD .*/pub 250' "$scratch/ftpd.log"; then
	echo "ok: Fetch 4.0.3 connected over TCP/IP, logged in and opened pub ($scratch/fetch.png)"
else
	echo "FAIL: Fetch 4.0.3 over TCP/IP (see $scratch/fetch.log, $scratch/ftpd.log)"
	status=1
fi
kill "$ftpd" 2>/dev/null || true

# ---- AppleTalk: two Shadow Warriors

for who in a b; do
	rm -rf "${scratch:?}/sw-$who"
	cp -R "$sw" "$scratch/sw-$who"
	mkdir -p "$scratch/home-$who"
done
# Not a network game until SW Net Startup says so.
config="$scratch/sw-a/SW Demo Files/SW Config"
LC_ALL=C sed -e 's/NetGame = 1/NetGame = 0/' -e 's/CommType = [0-9]/CommType = 0/' "$config" >"$scratch/config" && cp "$scratch/config" "$config"
# SW Net Startup writes SW Config for an AppleTalk game (Connection Type
# AppleTalk, two players by default) when Start is clicked, and would
# launch the game.
HOME="$scratch/home-a" "$bluebox" run "$scratch/sw-a/SW Net Startup (Demo)" --ppc --headless --max 900000000 \
	--keys "$(filler 60)click:87/113,$(filler 4)click:247/373" >"$scratch/startup.log" 2>&1 || true
if ! LC_ALL=C grep -q 'NetGame = 1' "$config" || ! LC_ALL=C grep -q 'CommType = 4' "$config"; then
	echo "FAIL: SW Net Startup wrote no AppleTalk game (see $scratch/startup.log)"
	exit 1
fi
LC_ALL=C sed 's/PlayerName = "[^"]*"/PlayerName = "Wang"/' "$config" >"$scratch/sw-b/SW Demo Files/SW Config"

for who in a b; do
	HOME="$scratch/home-$who" "$bluebox" run "$scratch/sw-$who/Shadow Warrior (Demo)" --ppc --headless --realtime \
		--appletalk local --depth 8 --max 3000000000 --log traps --screenshot "$scratch/sw-$who.png" 2>&1 |
		grep -a -E 'OT: (NBP|notifier)' >"$scratch/sw-$who.log" &
done
wait
for who in a b; do
	found=$(grep -c 'NBP lookup found 2' "$scratch/sw-$who.log" || true)
	got=$(grep -c 'event 00000004' "$scratch/sw-$who.log" || true) # T_DATA
	if [ "$found" -gt 0 ] && [ "$got" -gt 0 ]; then
		echo "ok: Shadow Warrior $who found the other through NBP and was told of datagrams $got times ($scratch/sw-$who.png)"
	else
		echo "FAIL: Shadow Warrior $who over AppleTalk (lookups finding both: $found, T_DATA: $got)"
		status=1
	fi
done
exit "$status"
