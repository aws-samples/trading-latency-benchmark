#!/bin/bash
# Monitor XDP/XSK stats and mark rows that changed since last sample
IFACE="${1:-enp40s0}"
INTERVAL="${2:-10}"
PREV_FILE=$(mktemp)

echo "Monitoring XDP stats on $IFACE every ${INTERVAL}s (Ctrl+C to stop)"
echo ""

# First sample
sudo ethtool -S "$IFACE" | egrep 'xsk|xdp' > "$PREV_FILE"

while true; do
    sleep "$INTERVAL"
    CURR_FILE=$(mktemp)
    sudo ethtool -S "$IFACE" | egrep 'xsk|xdp' > "$CURR_FILE"

    clear
    echo "=== XDP Stats: $IFACE (every ${INTERVAL}s) ==="
    echo ""

    while IFS= read -r line; do
        key=$(echo "$line" | awk -F: '{print $1}' | xargs)
        val=$(echo "$line" | awk -F: '{print $2}' | xargs)
        prev_val=$(grep "$key:" "$PREV_FILE" | awk -F: '{print $2}' | xargs)

        if [ "$val" != "$prev_val" ] && [ -n "$prev_val" ]; then
            delta=$((val - prev_val))
            printf "  * %-40s %12s  (+%s)\n" "$key:" "$val" "$delta"
        else
            printf "    %-40s %12s\n" "$key:" "$val"
        fi
    done < "$CURR_FILE"

    cp "$CURR_FILE" "$PREV_FILE"
    rm -f "$CURR_FILE"
done
