#!/bin/bash
# Prepare the engine mode on the R2T2: a ticket secret shared with afu-remote and the
# control socket. Existing direct access stays as it is (ticket_only stays 0).
#   R2T2_HOST=dk5den@44.149.24.154 R2T2_KEY=~/.ssh/r2t2 R2T2_SUDO=... tools/engine-setup-r2t2.sh [group]
# group: may read the secret and use the socket; the group of the account afu-remote will
# run as later (default: primary group of the SSH user).
set -euo pipefail
HOST=${R2T2_HOST:-dk5den@44.149.24.154}
KEY=${R2T2_KEY:-$HOME/.ssh/r2t2}
SUDO_PW=${R2T2_SUDO:?set R2T2_SUDO to the sudo password of the R2T2 account}
GROUP=${1:-}

ssh -o BatchMode=yes -i "$KEY" "$HOST" bash -s <<EOF
set -e
G=${GROUP:-\$(id -gn)}
s() { echo '$SUDO_PW' | sudo -S -p "" "\$@"; }
if ! s test -s /etc/r2t2sdr-ticket.key; then
  umask 077; head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > /tmp/ticket.key
  s install -m 640 -o root -g \$G /tmp/ticket.key /etc/r2t2sdr-ticket.key
  rm -f /tmp/ticket.key
fi
s sed -i '/^ticket_secret\|^ticket_device\|^ticket_only\|^control_socket\|^control_group/d' /etc/r2t2sdr.conf
# sudo reads the password from stdin, so the lines go through a file
printf '%s\n' '' '# engine mode for afu-remote' 'ticket_secret = /etc/r2t2sdr-ticket.key' \
  'ticket_device = sdr' 'ticket_only = 0' 'control_socket = /run/r2t2sdr.sock' "control_group = \$G" \
  > /tmp/r2t2sdr-engine.conf
s sh -c 'cat /tmp/r2t2sdr-engine.conf >> /etc/r2t2sdr.conf'
rm -f /tmp/r2t2sdr-engine.conf
grep -E '^(ticket|control)_' /etc/r2t2sdr.conf
s systemctl restart r2t2sdr
sleep 2
systemctl is-active r2t2sdr
ls -l /etc/r2t2sdr-ticket.key /run/r2t2sdr.sock
EOF
