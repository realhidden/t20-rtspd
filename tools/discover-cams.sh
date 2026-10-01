#!/bin/bash
# Discover cameras on the local network.
#
# Two signatures matter here:
#
#   T20 (ours)  -- telnet on 23 and the t20rtspd snapshot/status server on
#                  8080. MAC OUI e4:aa:ec. These can be patched remotely.
#   TP-Link     -- RTSP on 554 behind HTTP auth. NOT ours; cannot run our
#                  firmware, but listed so they are not mistaken for T20s.
#
# Usage: ./discover-cams.sh [cidr-prefix]
#   default scans 192.168.0.1-192.168.3.254; override with an argument, e.g.
#   ./discover-cams.sh 10.0 to scan a different /22.

set -u
BASE="${1:-192.168}"

probe() {
	ip="$1"
	t=0; h=0; r=0
	nc -z -G 1 -w 1 "$ip" 23   2>/dev/null && t=1
	nc -z -G 1 -w 1 "$ip" 8080 2>/dev/null && h=1
	nc -z -G 1 -w 1 "$ip" 554  2>/dev/null && r=1
	# Only the 23+8080 pair is a patchable T20. Report 554 alone as a
	# possible third-party camera so it can be triaged by hand.
	if { [ "$t" = 1 ] && [ "$h" = 1 ]; } || [ "$r" = 1 ]; then
		echo "$ip t20=$((t && h)) telnet=$t http8080=$h rtsp554=$r"
	fi
}
export -f probe

echo "Scanning ${BASE}.0.1 - ${BASE}.3.254 (telnet/8080/554)..."
for a in 0 1 2 3; do
	seq 1 254 | xargs -P 48 -I{} bash -c "probe ${BASE}.$a.{}"
done | sort -V

echo
echo "MAC addresses (populated by the scan above; these hosts ignore ICMP):"
for a in 0 1 2 3; do
	seq 1 254 | xargs -P 48 -I{} bash -c "nc -z -G 1 -w 1 ${BASE}.$a.{} 554 2>/dev/null; nc -z -G 1 -w 1 ${BASE}.$a.{} 23 2>/dev/null"
done
arp -a | grep -E "192\.168\.[0-3]\." | grep -v incomplete | sort -u
