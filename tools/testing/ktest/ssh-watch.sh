#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2018 Intel Corporation. All rights reserved.
#
# Marc.Herbert@intel.com

# set -x
set -e

# This is ktest's default SUCCESS_LINE, make it look like the tty it
# expects.
SUCCESS_LINE='login:'

die()
{
    >&2 printf "ERROR: "
    >&2 printf "$@"
    exit 1
}

usage()
{
cat <<EOF

This script keeps trying to ssh and prints a fake "${SUCCESS_LINE}"
prompt when it does so ktest (or something else) knows when the system
successfully (re)booted. It's meant to keep running forever in a
forgotten terminal.

Usage:
  - mkfifo ./monitor-ktest.fifo
  - $0 [root@]target.local ./monitor-ktest.fifo
  - In your ktest.conf file:
       CONSOLE = cat ./monitor-ktest.fifo

To stop hit Ctrl-C *twice* in a row.

If you were used to letting ktest time out then you most likely want to
add REBOOT_ON_SUCCESS=0 and REBOOT_ON_ERROR=0 to your ktest config file.
EOF
  exit 1
}

fifo_readers()
{
    lsof +E "$1" | grep 'r[[:blank:]]*FIFO'
}

main()
{
    test  -n "$2"  || usage
    local remote=$1
    local fifo="$2"

    test -p "$fifo" || die '%s is not a pipe\n' "$fifo"

    # lsof doesn't show pipes until they have both reader(s) and writer(s)
    if lsof +E "$fifo" | grep '^'; then
        >&2 printf 'WARNING: zombie(s) are still using %s!\n' "$fifo"
    fi

    # In theory we don't need the outer "while" loop and inner subshell
    # because the inner "while" loop never writes to the $fifo itself
    # hence it will never see a failure when the reader has gone and
    # closed the other of that named pipe. Only the ssh child process
    # uses the $fifo pipe and experiences closures and failures.  In
    # practice let's be paranoid and play it safe.
    local npipe=0
    while true; do

        >&2 printf '%s open number %d\n' "$fifo" $((npipe=npipe+1))
        local nssh=1
        ( while true; do
              local d
              d=$(date --rfc-3339=seconds)

              if fifo_readers "$fifo" | tail -n +2 | grep -q '^'; then
                  >&2 lsof +E "$fifo"
                  >&2 printf 'ERROR: more than one %s reader!\n' "$fifo"
                  # exiting closes the pipe
                  >&2 printf '  suspending for forensics\n'
                  sleep 100000; exit 1
              fi

              # We could have a ping retry loop first to save a few
              # seconds but ICMP is sometimes filtered. Can of worms.

              >&2 printf '%s open number %d, ssh %s attempt number %d\n' \
                  "$fifo" $((npipe)) "$remote" $((nssh=nssh+1))
              d=$(date --rfc-3339=seconds)

              # This dirt cheap set -x logging can be used to match logs
              # from both sides.
              stdbuf --output=0 ssh -oServerAliveInterval=20 "$remote" \
                    "set -x; printf %s\\\n '$0 $nssh $d ${SUCCESS_LINE}';
                    sleep 1000000" || true
              # 1. Don't hog the CPU in case something unexpected happens
              # 2. Give double Ctrl-C a local chance
              # 3. Less spam when the system is down
              sleep 4
          done > "$fifo"
        )
        sleep 1 # same as 1. above
        >&2 printf 'pipe was probably closed on %s, re-opening it\n' "$0"

    done

}

main "$@"
