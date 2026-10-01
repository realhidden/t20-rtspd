#!/bin/bash
# Run a command on a Dafang camera over a raw telnet session driven by nc.
# (The macOS `telnet` binary is blocked by a local firewall rule; nc works.)
# Usage: cam.sh <ip> "command"
#
# The camera root password is read from the environment and has no default:
#   export CAM_PASS='...'
#   ./tools/cam.sh 192.168.x.y "command"
#
# Do not commit a password here. This repo is public.
IP="${1:?usage: cam.sh <ip> <cmd>}"
CMD="${2:?usage: cam.sh <ip> <cmd>}"
PW="${CAM_PASS:?CAM_PASS must be set (camera root password)}"

EXPFILE="$(mktemp)"
cat > "$EXPFILE" <<'EOF'
set timeout 60
log_user 1
spawn nc $env(CAM_IP) 23
expect {
  -re "ogin:" { send "root\r" }
  timeout { puts "\nNO LOGIN PROMPT"; exit 1 }
}
expect {
  -re "assword:" { send "$env(CAM_PW)\r" }
  timeout { puts "\nNO PASSWORD PROMPT"; exit 1 }
}
sleep 2
expect -re {[$#] $}
send -- "$env(REMOTE_CMD)\r"
sleep 3
expect -re {[$#] $}
send "\r"
sleep 1
send "exit\r"
expect eof
EOF

for attempt in 1 2 3 4 5 6; do
  if CAM_IP="$IP" CAM_PW="$PW" REMOTE_CMD="$CMD" expect -f "$EXPFILE" 2>/dev/null; then
    rm -f "$EXPFILE"; exit 0
  fi
  echo "(attempt $attempt: telnet session failed, retrying)" >&2
  sleep 5
done

rm -f "$EXPFILE"
echo "FAILED to run on $IP after 6 attempts" >&2
exit 1
