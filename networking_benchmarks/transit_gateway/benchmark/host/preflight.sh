#!/bin/bash
# preflight.sh - clock and NIC readiness check, run ON a benchmark instance.
#
# Modes:
#   preflight.sh [--max-offset-us N]   full check (static + gate). Exit 1 on failure.
#   preflight.sh --static              checks that do not change between runs: PHC driver, PTP
#                                      device, hardware RX stamping, exactly one refclock
#   preflight.sh --gate [--max-offset-us N]
#                                      per-run checks: PHC is the selected source, then force a
#                                      clock step and require System time within the threshold
#   preflight.sh --clock               read-only snapshot of the chrony state (no step)
#   preflight.sh --context             read-only record of the host and NIC state in effect
#
# Output is key=value lines, then PASS or one FAIL:<code> line per failed check.
#
# The gate is chrony's "System time" (how far the system clock is from the PHC-disciplined
# time right now), not "Last offset" (the previous measurement) or "RMS offset" (a long
# average): only System time bounds the error of a timestamp taken now.
#
# Test seams: SYSROOT prefixes /sys paths; chronyc, ethtool and ip come from PATH;
# PREFLIGHT_SETTLE is the seconds to wait after the step (default 1); IFACE overrides detection.
set -u

MAX_OFFSET_US=10
MODE=full
while [ $# -gt 0 ]; do
  case "$1" in
    --max-offset-us) MAX_OFFSET_US="$2"; shift 2 ;;
    --static) MODE=static; shift ;;
    --gate) MODE=gate; shift ;;
    --clock) MODE=clock; shift ;;
    --context) MODE=context; shift ;;
    -h|--help) sed -n 2,22p "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

SYSROOT="${SYSROOT:-}"
SETTLE="${PREFLIGHT_SETTLE:-1}"
FAILS=""
fail() { FAILS="$FAILS FAIL:$1"; }

# chrony "System time : 0.000000062 seconds slow of NTP time" -> signed microseconds
system_time_us() {
  chronyc tracking 2>/dev/null | awk '/^System time/ {
    v = $4 + 0; if ($6 == "slow") v = -v; printf "%.3f", v * 1e6; found = 1 }
    END { if (!found) print "unreadable" }'
}

# Run context: what the numbers were measured under. Every line is key=value, one line per key.
if [ "$MODE" = context ]; then
  ctx() { printf '%s=%s\n' "$1" "$2"; }
  sysfs() { cat "$SYSROOT$1" 2>/dev/null | tr '\n' ' ' | sed 's/ *$//'; }
  IF="${IFACE:-$(ip -o -4 route show to default 2>/dev/null | awk '{print $5}' | head -1)}"
  IF="${IF:-eth0}"
  ctx kernel "$(uname -r)"
  ctx cpu_model "$(grep -m1 'model name' "$SYSROOT/proc/cpuinfo" 2>/dev/null | sed 's/^[^:]*: *//')"
  ctx cpu_count "$(grep -c '^processor' "$SYSROOT/proc/cpuinfo" 2>/dev/null)"
  ctx iface "$IF"
  ctx ena_driver "$(ethtool -i "$IF" 2>/dev/null | awk -F': *' '/^(driver|version)/ {printf "%s%s", sep, $2; sep=" "}')"
  ctx ena_phc_enable "$(sysfs /sys/module/ena/parameters/phc_enable)"
  ctx mtu "$(sysfs "/sys/class/net/$IF/mtu")"
  ctx coalescing "$(ethtool -c "$IF" 2>/dev/null | awk '/^Adaptive RX/ {a=$3} /^rx-usecs:/ {r=$2} /^tx-usecs:/ {t=$2} END {printf "adaptive_rx=%s rx_usecs=%s tx_usecs=%s", a, r, t}')"
  ctx channels "$(ethtool -l "$IF" 2>/dev/null | awk '/Current hardware/ {c=1} c && /^Combined/ {print $2; exit}')"
  ctx gro "$(ethtool -k "$IF" 2>/dev/null | awk -F': *' '/^generic-receive-offload/ {print $2}' | cut -d' ' -f1)"
  ctx napi_defer_hard_irqs "$(sysfs "/sys/class/net/$IF/napi_defer_hard_irqs")"
  ctx gro_flush_timeout "$(sysfs "/sys/class/net/$IF/gro_flush_timeout")"
  ctx irqbalance "$(systemctl is-active irqbalance 2>/dev/null || echo unknown)"
  ctx cpuidle_driver "$(sysfs /sys/devices/system/cpu/cpuidle/current_driver)"
  ctx cpuidle_states "$(for d in "$SYSROOT"/sys/devices/system/cpu/cpu0/cpuidle/state*; do [ -d "$d" ] && printf '%s:%sus ' "$(cat "$d/name")" "$(cat "$d/latency")"; done | sed 's/ *$//')"
  ctx cpufreq_governor "$(sysfs /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
  ctx cmdline_tuning "$(tr ' ' '\n' < "$SYSROOT/proc/cmdline" 2>/dev/null | grep -E '^(isolcpus|nohz_full|rcu_nocbs|nosmt|mitigations|intel_idle|processor|idle|tsc|clocksource)' | tr '\n' ' ' | sed 's/ *$//')"
  ctx busy_poll "$(sysctl -n net.core.busy_poll 2>/dev/null)/$(sysctl -n net.core.busy_read 2>/dev/null)"
  ctx rmem_max "$(sysctl -n net.core.rmem_max 2>/dev/null)"
  ctx thp "$(sed 's/.*\[\(.*\)\].*/\1/' "$SYSROOT/sys/kernel/mm/transparent_hugepage/enabled" 2>/dev/null)"
  ctx clocksource "$(sysfs /sys/devices/system/clocksource/clocksource0/current_clocksource)"
  ctx chrony_refclock "$(cat "$SYSROOT"/etc/chrony.conf "$SYSROOT"/etc/chrony.d/*.conf 2>/dev/null | grep -E '^[[:space:]]*refclock' | tr -s ' ' | tr '\n' ';' | sed 's/;$//')"
  ctx chrony_reference "$(chronyc tracking 2>/dev/null | awk -F': *' '/^Reference ID/ {print $2}')"
  ctx chrony_system_time_us "$(system_time_us)"
  exit 0
fi

if [ "$MODE" = clock ]; then
  echo "system_time_us=$(system_time_us)"
  chronyc tracking 2>/dev/null | awk -F': *' '/^Reference ID/ {print "reference=" $2} /^Leap status/ {print "leap=" $2}'
  exit 0
fi

if [ "$MODE" != gate ]; then
# 1. ENA PHC support loaded
PHC_ENABLE=$(cat "$SYSROOT/sys/module/ena/parameters/phc_enable" 2>/dev/null || echo 0)
echo "phc_enable=$PHC_ENABLE"
[ "$PHC_ENABLE" = "1" ] || fail phc_not_enabled

# 2. The ENA PTP device exists
PTP_DEV=""
for f in "$SYSROOT"/sys/class/ptp/*/clock_name; do
  [ -f "$f" ] || continue
  if grep -q ena-ptp "$f"; then PTP_DEV="/dev/$(basename "$(dirname "$f")")"; break; fi
done
echo "ptp_device=${PTP_DEV:-none}"
[ -n "$PTP_DEV" ] || fail no_ena_ptp_device

# 3. The NIC advertises hardware receive stamping
IFACE="${IFACE:-$(ip -o -4 route show to default 2>/dev/null | awk '{print $5}' | head -1)}"
echo "iface=${IFACE:-unknown}"
if ethtool -T "${IFACE:-eth0}" 2>&1 | grep -q hardware-receive; then
  echo "hw_receive=yes"
else
  echo "hw_receive=no"; fail no_hw_receive
fi
fi

# 4. Exactly one PHC refclock (static), and it is the selected source (gate)
SOURCES=$(chronyc sources 2>&1)
if [ "$MODE" != gate ]; then
  REFCLOCKS=$(echo "$SOURCES" | grep -c '^#')
  echo "refclocks=$REFCLOCKS"
  [ "$REFCLOCKS" = "1" ] || fail refclock_count_not_1
fi
if [ "$MODE" != static ]; then
  SELECTED=$(echo "$SOURCES" | awk '/^[#^=][*]/ {print $2; exit}')
  echo "selected=${SELECTED:-none}"
  echo "$SOURCES" | grep -q '^#\*' || fail phc_not_selected
fi

if [ "$MODE" != static ]; then
# 5. Step away any residual offset, then require the clock to be close
chronyc makestep >/dev/null 2>&1 || fail makestep_failed
sleep "$SETTLE"
OFFSET_US=$(system_time_us)
echo "system_time_us=$OFFSET_US"
echo "max_offset_us=$MAX_OFFSET_US"
if [ "$OFFSET_US" = "unreadable" ]; then
  fail clock_unreadable
elif ! awk -v v="$OFFSET_US" -v m="$MAX_OFFSET_US" 'BEGIN { if (v < 0) v = -v; exit !(v <= m) }'; then
  fail "clock_offset_exceeds_${MAX_OFFSET_US}us"
fi
chronyc tracking 2>/dev/null | awk -F': *' '/^Leap status/ {print "leap=" $2}'
fi

if [ -z "$FAILS" ]; then
  echo PASS
  exit 0
fi
for f in $FAILS; do echo "$f"; done
exit 1
