#!/bin/sh
#
# The Phase 1 regression gate: does the renderer still put out the same pixels?
#
#   ./regression/gate.sh record          record the reference demo (once)
#   ./regression/gate.sh capture LABEL   replay it and hash every frame
#   ./regression/gate.sh compare A B     diff two captures
#
# `record` needs a playable build and takes a couple of minutes; the demo it
# writes is the artifact everything afterwards is measured against, so record it
# once on a known-good build and leave it alone. `capture` is what you run
# before and after a change.
#
# Everything lives under regression/work, which is fs_savepath for these runs -
# your own config, saves and screenshots are never touched.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD:-$root/cmake-build-debug}
work=$root/regression/work
gamedir=demo

if [ -x "$build/neo/dhewm3.app/Contents/MacOS/dhewm3" ]; then
	exe=$build/neo/dhewm3.app/Contents/MacOS/dhewm3
elif [ -x "$build/neo/dhewm3" ]; then
	exe=$build/neo/dhewm3
else
	echo "no dhewm3 binary under $build - set BUILD to your build directory" >&2
	exit 1
fi

# fs_basepath has to stay where demo00.pk4 is; only the write path moves.
basepath=$build/neo

run_engine() {	# run_engine <cfg> <logfile>
	mkdir -p "$work/$gamedir"
	cp "$root/regression/$1" "$work/$gamedir/"
	"$exe" +set fs_basepath "$basepath" +set fs_savepath "$work" \
	       +exec "$1" > "$2" 2>&1 &
	engine_pid=$!
}

case ${1:-} in
record)
	rm -rf "$work"
	log=$work/record.log
	mkdir -p "$work"
	run_engine record.cfg "$log"
	wait "$engine_pid" || true
	demo=$work/$gamedir/demos/reference.demo
	if [ ! -s "$demo" ]; then
		echo "recording failed, see $log" >&2
		exit 1
	fi
	mv "$demo" "$root/regression/reference.demo"
	echo "recorded regression/reference.demo ($(wc -c < "$root/regression/reference.demo") bytes)"
	;;

capture)
	label=${2:?usage: gate.sh capture LABEL}
	demo=$root/regression/reference.demo
	[ -s "$demo" ] || { echo "no regression/reference.demo - run 'gate.sh record' first" >&2; exit 1; }

	frames=$work/frames-$label
	rm -rf "$frames" "$work/$gamedir/demos"
	mkdir -p "$work/$gamedir/demos"
	cp "$demo" "$work/$gamedir/demos/reference.demo"

	log=$work/capture-$label.log
	run_engine capture.cfg "$log"

	# aviDemo has no way to say "and then quit", so wait for the line it prints
	# when the capture is complete and stop the engine ourselves.
	waited=0
	while ! grep -q "^captured .* frames" "$log" 2>/dev/null; do
		if ! kill -0 "$engine_pid" 2>/dev/null; then
			echo "engine exited before finishing the capture, see $log" >&2
			exit 1
		fi
		[ "$waited" -lt 900 ] || { echo "timed out after 15 minutes, see $log" >&2; exit 1; }
		sleep 1
		waited=$((waited + 1))
	done
	kill "$engine_pid" 2>/dev/null || true
	wait "$engine_pid" 2>/dev/null || true

	mv "$work/$gamedir/demos/reference" "$frames"
	( cd "$frames" && ls *.tga | sort | xargs shasum -a 256 ) > "$root/regression/frames-$label.sha256"
	n=$(grep -c . "$root/regression/frames-$label.sha256")
	echo "captured $n frames -> regression/frames-$label.sha256 (images in $frames)"
	;;

compare)
	a=${2:?usage: gate.sh compare A B}
	b=${3:?usage: gate.sh compare A B}
	fa=$root/regression/frames-$a.sha256
	fb=$root/regression/frames-$b.sha256
	for f in "$fa" "$fb"; do
		[ -s "$f" ] || { echo "missing $f" >&2; exit 1; }
	done
	if cmp -s "$fa" "$fb"; then
		echo "identical: $(grep -c . "$fa") frames match between '$a' and '$b'"
	else
		echo "DIFFERENT frames between '$a' and '$b':"
		# Report the frame names whose hashes moved, not the hashes themselves.
		sort -k2 "$fa" > "$work/.cmp-a"
		sort -k2 "$fb" > "$work/.cmp-b"
		join -j 2 "$work/.cmp-a" "$work/.cmp-b" | awk '$2 != $3 { print "  " $1 }'
		rm -f "$work/.cmp-a" "$work/.cmp-b"
		echo "images: $work/frames-$a vs $work/frames-$b"
		exit 1
	fi
	;;

*)
	sed -n '2,17p' "$0" | sed 's|^#||;s|^ ||'
	exit 1
	;;
esac
