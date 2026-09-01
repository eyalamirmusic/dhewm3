#!/bin/sh
#
# The Phase 1 regression gate: does the renderer still put out the same pixels?
#
#   ./regression/gate.sh record          record the reference demo (once)
#   ./regression/gate.sh capture LABEL   replay it and hash every frame
#   ./regression/gate.sh compare A B     diff two captures
#
# `record` needs a playable build and takes about half a minute; the demo it
# writes is the artifact everything afterwards is measured against, so record it
# once on a known-good build and leave it alone. `capture` is what you run
# before and after a change, and takes about twenty seconds.
#
# Everything lives under regression/work, which is fs_savepath *and*
# fs_configpath for these runs - your own config, saves and screenshots are
# never touched. Every run is given a fresh game directory, because a run that
# inherits the previous run's written config is comparing two configurations
# and calling it a renderer regression.
#
# fs_configpath matters more than it looks. dhewm3 keeps dhewm.cfg on a path of
# its own, so redirecting only fs_savepath leaves the real config both readable
# and writable: the first version of this script did exactly that, a capture
# taken with r_skipSpecular 1 archived that cvar into the user's config, and
# every run afterwards silently read it back. It cost an afternoon and a wrong
# conclusion about a commit. Pin both paths.
#
# GATE_TIMEOUT (default 300) bounds every engine run. Nothing here ever waits
# on the engine indefinitely: it is a windowed game being driven by a script,
# and the ways it can sit there are not all worth enumerating.
#
# GAME (default dhewm3) picks which binary to run, because a build tree holds
# both of them: dhewm3 is the SDL/GL build and dhewm3-eacp is the port. They are
# two renderers and their hashes are not comparable with each other - compare
# each build against itself, the way the README already says hashes are only
# comparable within one machine and GPU.
#
#   GAME=dhewm3-eacp BUILD=$PWD/cmake-build-eacp ./regression/gate.sh capture x
#
# The eacp build is driven by the display link, which stops when the panel
# sleeps (plan.md section 5, gap 13) - so hold the display awake for the run:
#
#   caffeinate -du ./regression/gate.sh capture x

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD:-$root/cmake-build-debug}
timeout=${GATE_TIMEOUT:-300}
work=$root/regression/work
gamedir=demo
game=${GAME:-dhewm3}

if [ -x "$build/neo/$game.app/Contents/MacOS/$game" ]; then
	exe=$build/neo/$game.app/Contents/MacOS/$game
elif [ -x "$build/neo/$game" ]; then
	exe=$build/neo/$game
else
	echo "no $game binary under $build - set BUILD to your build directory," >&2
	echo "or GAME to the binary you meant (dhewm3, dhewm3-eacp)" >&2
	exit 1
fi

# fs_basepath has to stay where demo00.pk4 is; only the write path moves.
basepath=$build/neo

engine_pid=

stop_engine() {
	[ -n "$engine_pid" ] || return 0
	kill "$engine_pid" 2>/dev/null || true
	wait "$engine_pid" 2>/dev/null || true
	engine_pid=
}
trap 'stop_engine' EXIT INT TERM

# Start the engine on a game directory holding nothing but the cfg it is about
# to run, so no run can inherit state - a written dhewm3.cfg above all - from
# the run before it.
run_engine() {	# run_engine <cfg> <logfile>
	rm -rf "$work/$gamedir"
	mkdir -p "$work/$gamedir"
	cp "$root/regression/$1" "$work/$gamedir/"
	[ -z "${keep_demo:-}" ] || {
		mkdir -p "$work/$gamedir/demos"
		cp "$keep_demo" "$work/$gamedir/demos/reference.demo"
	}
	"$exe" +set fs_basepath "$basepath" +set fs_savepath "$work" \
	       +set fs_configpath "$work" +exec "$1" > "$2" 2>&1 &
	engine_pid=$!
}

# Wait until <marker> exists, the engine exits, or the timeout runs out.
await() {	# await <marker> <logfile> <what>
	waited=0
	while [ ! -e "$1" ]; do
		if ! kill -0 "$engine_pid" 2>/dev/null; then
			echo "engine exited before $3, see $2" >&2
			exit 1
		fi
		if [ "$waited" -ge "$timeout" ]; then
			echo "timed out after ${timeout}s waiting for $3, see $2" >&2
			echo "(raise it with GATE_TIMEOUT=... if this machine is just slow)" >&2
			exit 1
		fi
		sleep 1
		waited=$((waited + 1))
	done
}

case ${1:-} in
record)
	rm -rf "$work"
	mkdir -p "$work"
	log=$work/record.log
	run_engine record.cfg "$log"

	# record.cfg ends in `quit`, so the demo landing on disk is the signal.
	await "$work/$gamedir/demos/reference.demo" "$log" "the demo to be recorded"
	stop_engine

	demo=$work/$gamedir/demos/reference.demo
	if [ ! -s "$demo" ]; then
		echo "recording produced an empty demo, see $log" >&2
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
	rm -rf "$frames"
	mkdir -p "$work"

	log=$work/capture-$label.log
	keep_demo=$demo run_engine capture.cfg "$log"

	# aviDemo has no way to say "and then quit", and the engine's stdout is
	# block-buffered into the log, so the line it prints when it finishes may
	# sit unflushed for as long as the process idles afterwards. EndAVICapture
	# writes the .roqParam file as its last act, so wait for that instead.
	await "$work/$gamedir/demos/reference/reference.roqParam" "$log" "the capture to finish"
	stop_engine

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
		# Report how much moved and where it starts. Dumping every frame name
		# buries the one number that tells you what kind of change this is:
		# a handful of frames is a bug in one thing, all of them is systemic.
		sort -k2 "$fa" > "$work/.cmp-a"
		sort -k2 "$fb" > "$work/.cmp-b"
		join -j 2 "$work/.cmp-a" "$work/.cmp-b" | awk '$2 != $3 { print $1 }' > "$work/.cmp-diff"
		rm -f "$work/.cmp-a" "$work/.cmp-b"
		total=$(grep -c . "$fa")
		moved=$(grep -c . "$work/.cmp-diff" || true)
		echo "DIFFERENT: $moved of $total frames moved between '$a' and '$b'"
		head -8 "$work/.cmp-diff" | sed 's/^/  /'
		[ "$moved" -le 8 ] || echo "  ... and $((moved - 8)) more (full list in $work/.cmp-diff)"
		echo "images: $work/frames-$a vs $work/frames-$b"
		exit 1
	fi
	;;

*)
	sed -n '3,12p' "$0" | sed 's|^#||;s|^ ||'
	exit 1
	;;
esac
