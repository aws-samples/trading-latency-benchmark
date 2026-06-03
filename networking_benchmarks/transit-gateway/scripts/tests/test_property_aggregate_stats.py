"""Property-based test for aggregate statistics computation correctness.

Feature: tgw-multicast-benchmark, Property 2: Aggregate statistics computation correctness

**Validates: Requirements 4.2**
"""

import sys
import os

# Add scripts/ to path so we can import collect_results
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from hypothesis import given, settings
from hypothesis import strategies as st

from collect_results import compute_aggregate_statistics


@st.composite
def subscriber_metrics(draw):
    """Generate a valid per-subscriber metrics dict.

    Constraints:
      - min <= median <= p95 <= p99 <= max  (ordered latency chain)
      - min <= mean <= max
      - total_received <= total_expected
      - packet_loss_count = total_expected - total_received
      - All latency values >= 0
    """
    # Generate 5 sorted latency values for the ordered chain: min, median, p95, p99, max
    # Use min_value=0.001 to avoid subnormal floats that round to 0.0 at 3 decimal places,
    # which would break ordering invariants after the function's round(..., 3) calls.
    # Real latency values in microseconds are always well above this threshold.
    latencies = sorted(
        draw(
            st.lists(
                st.floats(min_value=0.001, max_value=1e9, allow_nan=False, allow_infinity=False),
                min_size=5,
                max_size=5,
            )
        )
    )
    min_lat = latencies[0]
    median_lat = latencies[1]
    p95_lat = latencies[2]
    p99_lat = latencies[3]
    max_lat = latencies[4]

    # mean must be between min and max
    mean_lat = draw(
        st.floats(min_value=min_lat, max_value=max_lat, allow_nan=False, allow_infinity=False)
    )

    total_expected = draw(st.integers(min_value=1, max_value=100_000))
    total_received = draw(st.integers(min_value=0, max_value=total_expected))
    packet_loss_count = total_expected - total_received

    return {
        "min_latency_us": min_lat,
        "max_latency_us": max_lat,
        "mean_latency_us": mean_lat,
        "median_latency_us": median_lat,
        "p95_latency_us": p95_lat,
        "p99_latency_us": p99_lat,
        "total_received": total_received,
        "total_expected": total_expected,
        "packet_loss_count": packet_loss_count,
    }


@given(
    per_sub=st.lists(subscriber_metrics(), min_size=1, max_size=10),
)
@settings(max_examples=100, deadline=None)
def test_aggregate_statistics_correctness(per_sub):
    """Property 2: Aggregate statistics computation correctness.

    Feature: tgw-multicast-benchmark, Property 2: Aggregate statistics computation correctness

    **Validates: Requirements 4.2**

    For any non-empty collection of per-subscriber result sets, the computed
    aggregate statistics must satisfy ordering and summation invariants.
    """
    agg = compute_aggregate_statistics(per_sub)

    # --- Ordering invariants ---
    assert agg["min_latency_us"] <= agg["median_latency_us"] <= agg["max_latency_us"], (
        f"min <= median <= max violated: {agg['min_latency_us']} <= {agg['median_latency_us']} <= {agg['max_latency_us']}"
    )
    assert agg["min_latency_us"] <= agg["mean_latency_us"] <= agg["max_latency_us"], (
        f"min <= mean <= max violated: {agg['min_latency_us']} <= {agg['mean_latency_us']} <= {agg['max_latency_us']}"
    )
    assert agg["p95_latency_us"] <= agg["p99_latency_us"] <= agg["max_latency_us"], (
        f"p95 <= p99 <= max violated: {agg['p95_latency_us']} <= {agg['p99_latency_us']} <= {agg['max_latency_us']}"
    )

    # --- Cross-subscriber aggregation invariants ---
    # min is floored to 3 decimal places, max is ceiled to 3 decimal places
    import math

    expected_min = math.floor(min(m["min_latency_us"] for m in per_sub) * 1000) / 1000
    expected_max = math.ceil(max(m["max_latency_us"] for m in per_sub) * 1000) / 1000
    assert agg["min_latency_us"] == expected_min, (
        f"aggregate min must equal floor-rounded min of per-subscriber mins: {agg['min_latency_us']} != {expected_min}"
    )
    assert agg["max_latency_us"] == expected_max, (
        f"aggregate max must equal ceil-rounded max of per-subscriber maxes: {agg['max_latency_us']} != {expected_max}"
    )

    # --- Summation invariants ---
    assert agg["total_received"] == sum(m["total_received"] for m in per_sub), (
        "aggregate total_received must equal sum of per-subscriber total_received"
    )
