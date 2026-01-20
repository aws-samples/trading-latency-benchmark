//! Latency Tracker Module
//!
//! Handles latency tracking and histogram-based statistics reporting.
//! Uses HdrHistogram for precise latency measurements with percentile calculations.

use hdrhistogram::Histogram;
use hdrhistogram::serialization::interval_log::IntervalLogWriterBuilder;
use hdrhistogram::serialization::V2DeflateSerializer;
use std::collections::HashMap;
use std::fs::{self, OpenOptions};
use std::time::{Instant, SystemTime};

/// Latency statistics structure
#[derive(Debug, Clone)]
pub struct LatencyStats {
    pub p50: u64,
    pub p90: u64,
    pub p95: u64,
    pub p99: u64,
    pub p99_9: u64,
    pub p99_99: u64,
    pub max: u64,
}

/// Latency tracker for measuring round-trip times
pub struct LatencyTracker {
    /// Maps client_id -> timestamp when order was sent
    order_sent_times: HashMap<String, Instant>,
    /// Maps client_id -> timestamp when cancel was sent
    cancel_sent_times: HashMap<String, Instant>,
    /// Histogram for latency measurements (in nanoseconds)
    histogram: Histogram<u64>,
    /// Count of responses received
    response_count: u64,
    /// Number of responses before printing statistics
    test_size: usize,
    /// Host used as directory name for histogram log files
    host: String,
    /// Timestamp when the current histogram interval started
    interval_start: Instant,
}

impl LatencyTracker {
    /// Create a new LatencyTracker with pre-allocated capacity
    ///
    /// Histogram is configured with:
    /// - Minimum value: 1 nanosecond
    /// - Maximum value: 3,600,000,000,000 nanoseconds (1 hour)
    /// - 3 significant figures precision
    pub fn new(test_size: usize, host: String) -> Self {
        // Pre-allocate HashMap capacity based on expected concurrent orders
        let capacity = std::cmp::min(test_size, 1000);

        Self {
            order_sent_times: HashMap::with_capacity(capacity),
            cancel_sent_times: HashMap::with_capacity(capacity),
            histogram: Histogram::new_with_bounds(1, 3_600_000_000_000, 3)
                .expect("Failed to create histogram"),
            response_count: 0,
            test_size,
            host,
            interval_start: Instant::now(),
        }
    }

    /// Record when an order was sent
    pub fn record_order_sent(&mut self, client_id: String) {
        self.order_sent_times.insert(client_id, Instant::now());
    }

    /// Record when a cancel was sent
    pub fn record_cancel_sent(&mut self, client_id: String) {
        self.cancel_sent_times.insert(client_id, Instant::now());
    }

    /// Record when an order response (BOOKED) was received
    /// Returns the RTT in nanoseconds if the client_id was found
    pub fn record_order_response(&mut self, client_id: &str) -> Option<u64> {
        if let Some(sent_time) = self.order_sent_times.remove(client_id) {
            let rtt = self.calculate_rtt(sent_time);
            self.record_latency(rtt);
            Some(rtt)
        } else {
            log::warn!("Received order response for unknown client_id: {}", client_id);
            None
        }
    }

    /// Record when a cancel response (DONE) was received
    /// Returns the RTT in nanoseconds if the client_id was found
    pub fn record_cancel_response(&mut self, client_id: &str) -> Option<u64> {
        if let Some(sent_time) = self.cancel_sent_times.remove(client_id) {
            let rtt = self.calculate_rtt(sent_time);
            self.record_latency(rtt);
            Some(rtt)
        } else {
            log::warn!("Received cancel response for unknown client_id: {}", client_id);
            None
        }
    }

    /// Check if statistics should be printed (every test_size responses)
    pub fn should_print_stats(&self) -> bool {
        self.response_count > 0 && self.response_count % (self.test_size as u64) == 0
    }

    /// Check if the test has reached the configured test_size
    pub fn is_test_complete(&self) -> bool {
        self.response_count >= self.test_size as u64
    }

    /// Print statistics, save histogram to file, and reset
    pub fn print_stats(&mut self) {
        let stats = self.get_stats();
        
        println!("{{");
        println!("  \"latency_ns\": {{");
        println!("    \"p50\": {},", stats.p50);
        println!("    \"p90\": {},", stats.p90);
        println!("    \"p95\": {},", stats.p95);
        println!("    \"p99\": {},", stats.p99);
        println!("    \"p99.9\": {},", stats.p99_9);
        println!("    \"p99.99\": {},", stats.p99_99);
        println!("    \"max\": {}", stats.max);
        println!("  }},");
        println!("  \"count\": {}", self.histogram.len());
        println!("}}");

        log::info!(
            "Latency stats (ns): p50={}, p90={}, p95={}, p99={}, p99.9={}, p99.99={}, max={}",
            stats.p50, stats.p90, stats.p95, stats.p99, stats.p99_9, stats.p99_99, stats.max
        );

        // Save histogram to .hlog file
        self.save_histogram_to_file();

        // Reset histogram for next interval
        self.histogram.reset();
        self.interval_start = Instant::now();
    }

    /// Save the current histogram to an HdrHistogram .hlog file.
    /// Uses the host as directory name (dots replaced with underscores)
    /// and appends a "rust" suffix to distinguish from other client implementations.
    fn save_histogram_to_file(&self) {
        let folder = format!("./{}", self.host.replace('.', "_"));
        if let Err(e) = fs::create_dir_all(&folder) {
            log::error!("Failed to create histogram log directory '{}': {}", folder, e);
            return;
        }

        let path = format!("{}/histogram_rust.hlog", folder);
        let mut file = match OpenOptions::new().create(true).append(true).open(&path) {
            Ok(f) => f,
            Err(e) => {
                log::error!("Failed to open histogram log file '{}': {}", path, e);
                return;
            }
        };

        let now = SystemTime::now();
        let interval_elapsed = self.interval_start.elapsed();
        let start_time = now - interval_elapsed;

        let mut serializer = V2DeflateSerializer::new();
        let mut builder = IntervalLogWriterBuilder::new();
        builder
            .add_comment("[Logged with Rust HFT Client 0.0.1]")
            .with_base_time(start_time)
            .with_start_time(start_time);

        let mut log_writer = match builder.begin_log_with(&mut file, &mut serializer) {
            Ok(w) => w,
            Err(e) => {
                log::error!("Failed to create interval log writer: {}", e);
                return;
            }
        };

        // start_timestamp is duration since base_time (zero since base_time == start_time)
        if let Err(e) = log_writer.write_histogram(
            &self.histogram,
            std::time::Duration::ZERO,
            interval_elapsed,
            hdrhistogram::serialization::interval_log::Tag::new("rust"),
        ) {
            log::error!("Failed to write histogram to log: {}", e);
            return;
        }

        log::info!("Histogram saved to {}", path);
    }

    /// Get current latency statistics
    pub fn get_stats(&self) -> LatencyStats {
        LatencyStats {
            p50: self.histogram.value_at_quantile(0.50),
            p90: self.histogram.value_at_quantile(0.90),
            p95: self.histogram.value_at_quantile(0.95),
            p99: self.histogram.value_at_quantile(0.99),
            p99_9: self.histogram.value_at_quantile(0.999),
            p99_99: self.histogram.value_at_quantile(0.9999),
            max: self.histogram.max(),
        }
    }

    /// Calculate round-trip time in nanoseconds
    fn calculate_rtt(&self, sent_time: Instant) -> u64 {
        sent_time.elapsed().as_nanos() as u64
    }

    /// Record a latency value in the histogram
    fn record_latency(&mut self, rtt_ns: u64) {
        // Clamp value to histogram bounds
        let clamped = rtt_ns.clamp(1, 3_600_000_000_000);
        if let Err(e) = self.histogram.record(clamped) {
            log::warn!("Failed to record latency {}: {}", rtt_ns, e);
        }
        self.response_count += 1;
    }

    /// Get the current response count
    pub fn response_count(&self) -> u64 {
        self.response_count
    }

    /// Check if an order is pending for the given client_id
    pub fn has_pending_order(&self, client_id: &str) -> bool {
        self.order_sent_times.contains_key(client_id)
    }

    /// Check if a cancel is pending for the given client_id
    pub fn has_pending_cancel(&self, client_id: &str) -> bool {
        self.cancel_sent_times.contains_key(client_id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::thread;
    use std::time::Duration;

    fn test_host() -> String {
        "test.host".to_string()
    }

    #[test]
    fn test_new_tracker() {
        let tracker = LatencyTracker::new(1000, test_host());
        assert_eq!(tracker.response_count(), 0);
        assert_eq!(tracker.test_size, 1000);
    }

    #[test]
    fn test_record_order_sent() {
        let mut tracker = LatencyTracker::new(1000, test_host());
        tracker.record_order_sent("test-id".to_string());
        assert!(tracker.has_pending_order("test-id"));
    }

    #[test]
    fn test_record_cancel_sent() {
        let mut tracker = LatencyTracker::new(1000, test_host());
        tracker.record_cancel_sent("test-id".to_string());
        assert!(tracker.has_pending_cancel("test-id"));
    }

    #[test]
    fn test_record_order_response() {
        let mut tracker = LatencyTracker::new(1000, test_host());
        tracker.record_order_sent("test-id".to_string());
        
        // Small delay to ensure measurable RTT
        thread::sleep(Duration::from_micros(100));
        
        let rtt = tracker.record_order_response("test-id");
        assert!(rtt.is_some());
        assert!(rtt.unwrap() >= 100_000); // 100µs in nanoseconds
        assert!(!tracker.has_pending_order("test-id"));
    }

    #[test]
    fn test_record_unknown_client_id() {
        let mut tracker = LatencyTracker::new(1000, test_host());
        let rtt = tracker.record_order_response("unknown-id");
        assert!(rtt.is_none());
    }

    #[test]
    fn test_should_print_stats() {
        let mut tracker = LatencyTracker::new(10, test_host());
        
        // Initially should not print
        assert!(!tracker.should_print_stats());
        
        // Record 10 responses
        for i in 0..10 {
            tracker.record_order_sent(format!("id-{}", i));
        }
        for i in 0..10 {
            tracker.record_order_response(&format!("id-{}", i));
        }
        
        // Should print after test_size responses
        assert!(tracker.should_print_stats());
    }

    #[test]
    fn test_get_stats() {
        let mut tracker = LatencyTracker::new(1000, test_host());
        
        // Record some latencies directly via the histogram
        for _ in 0..100 {
            tracker.histogram.record(1_000_000).unwrap(); // 1ms in nanoseconds
        }
        
        let stats = tracker.get_stats();
        assert!(stats.p50 > 0);
        assert!(stats.max >= stats.p99);
    }
}
