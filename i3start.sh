#!/bin/bash

# Set the port number.
PORT=${1:-9900}

rm -f shutdown.txt

# Set limits
ulimit -c unlimited

while true; do
    # If you want to have logs in a different directory,
    #   change the 'set logfile' line to reflect the directory name.
    INDEX=1000
    while [ -e "./logs/$INDEX.log" ]; do
        ((INDEX++))
    done
    LOGFILE="./logs/$INDEX.log"

    # Check if already running using ss (or netstat if preferred)
    if ss -tuln | grep -q ":$PORT "; then
        echo "Port $PORT is already in use."
        exit 0
    fi

    # Record starting time
    echo "$(date) :: Intermud-3 router startup initiated..." > "$LOGFILE"

    # Run I3 Router.
    ./i3router "$PORT" >> "$LOGFILE" 2>&1

    # Check for clean shutdown
    if [ -f "shutdown.txt" ]; then
        rm -f shutdown.txt
        exit 0
    fi

    # Wait before restarting
    sleep 5
done
