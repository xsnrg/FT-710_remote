#!/bin/sh
# Builds the RX gain/limiter plugin and its offline test, runs it. Usage: app/tests/test_rxlimit.sh [plugin.so]
# Without an argument it also tests a build without the final safety clamp, to show the
# limiter alone holds the -1 dBFS ceiling.
here=$(cd "$(dirname "$0")" && pwd); t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
src="$here/../netaudio/ft710_rxlimit.c"
cc -O2 -o "$t/test_rxlimit" "$here/test_rxlimit.c" -ldl -lm || exit 1
if [ -n "$1" ]; then "$t/test_rxlimit" "$1"; exit $?; fi
cc -O2 -shared -fPIC -o "$t/ft710_rxlimit.so" "$src" -lm || exit 1
cc -O2 -shared -fPIC -DFT710_NO_SAFETY_CLAMP -o "$t/noclamp.so" "$src" -lm || exit 1
echo "== plugin as shipped"; "$t/test_rxlimit" "$t/ft710_rxlimit.so"; a=$?
echo "== without the safety clamp (limiter only)"; "$t/test_rxlimit" "$t/noclamp.so"; b=$?
[ $a -eq 0 ] && [ $b -eq 0 ]
