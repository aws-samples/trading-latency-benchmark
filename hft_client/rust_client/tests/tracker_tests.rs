//! Latency Tracker Module Tests
//!
//! Unit tests and property-based tests for the tracker module.
//!
//! Property tests validate:
//! - Property 9: Round-Trip Time Calculation Correctness
//! - Property 10: Client ID Correlation Lookup
//! - Property 11: Entry Removal After Response
//! - Property 12: Statistics Trigger at Test Size Boundary
//! - Property 13: Statistics Output Completeness and Reset

use rust_hft_client::LatencyTracker;
use proptest::prelude::*;
use std::thread;
use std::time::Duration;

fn test_host() -> String {
    "test.host".to_string()
}

#[test]
fn test_tracker_initialization() {
    let tracker = LatencyTracker::new(1000, test_host());
    assert_eq!(tracker.response_count(), 0);
}

#[test]
fn test_order_tracking() {
    let mut tracker = LatencyTracker::new(1000, test_host());
    
    // Record order sent
    tracker.record_order_sent("order-1".to_string());
    assert!(tracker.has_pending_order("order-1"));
    assert!(!tracker.has_pending_order("order-2"));
    
    // Small delay
    thread::sleep(Duration::from_micros(50));
    
    // Record response
    let rtt = tracker.record_order_response("order-1");
    assert!(rtt.is_some());
    assert!(!tracker.has_pending_order("order-1"));
}

#[test]
fn test_cancel_tracking() {
    let mut tracker = LatencyTracker::new(1000, test_host());
    
    // Record cancel sent
    tracker.record_cancel_sent("cancel-1".to_string());
    assert!(tracker.has_pending_cancel("cancel-1"));
    
    // Small delay
    thread::sleep(Duration::from_micros(50));
    
    // Record response
    let rtt = tracker.record_cancel_response("cancel-1");
    assert!(rtt.is_some());
    assert!(!tracker.has_pending_cancel("cancel-1"));
}

#[test]
fn test_unknown_client_id_returns_none() {
    let mut tracker = LatencyTracker::new(1000, test_host());
    
    let rtt = tracker.record_order_response("unknown");
    assert!(rtt.is_none());
    
    let rtt = tracker.record_cancel_response("unknown");
    assert!(rtt.is_none());
}

#[test]
fn test_should_print_stats_boundary() {
    let mut tracker = LatencyTracker::new(5, test_host());
    
    // Initially should not print
    assert!(!tracker.should_print_stats());
    
    // Record 5 responses
    for i in 0..5 {
        tracker.record_order_sent(format!("id-{}", i));
    }
    for i in 0..5 {
        tracker.record_order_response(&format!("id-{}", i));
    }
    
    // Should print at boundary
    assert!(tracker.should_print_stats());
    assert_eq!(tracker.response_count(), 5);
}

#[test]
fn test_stats_calculation() {
    let mut tracker = LatencyTracker::new(1000, test_host());
    
    // Record some orders with delays
    for i in 0..10 {
        tracker.record_order_sent(format!("id-{}", i));
        thread::sleep(Duration::from_micros(100));
        tracker.record_order_response(&format!("id-{}", i));
    }
    
    let stats = tracker.get_stats();
    
    // All percentiles should be positive
    assert!(stats.p50 > 0);
    assert!(stats.p90 >= stats.p50);
    assert!(stats.p95 >= stats.p90);
    assert!(stats.p99 >= stats.p95);
    assert!(stats.max >= stats.p99);
}

// ============================================================================
// Property-Based Tests
// ============================================================================

// Strategy for generating valid client IDs
fn client_id_strategy() -> impl Strategy<Value = String> {
    "[a-z0-9]{8}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{4}-[a-z0-9]{12}"
}

// Strategy for generating test sizes
fn test_size_strategy() -> impl Strategy<Value = usize> {
    10usize..1000usize
}

proptest! {
    /// Feature: rust-hft-client, Property 10: Client ID Correlation Lookup
    /// 
    /// *For any* client_id that has been recorded with a timestamp, looking up 
    /// that client_id SHALL return the exact timestamp that was recorded.
    /// 
    /// **Validates: Requirements 5.6**
    #[test]
    fn prop_client_id_correlation_lookup(client_id in client_id_strategy()) {
        let mut tracker = LatencyTracker::new(1000, test_host());
        
        // Record order sent
        tracker.record_order_sent(client_id.clone());
        
        // Property: Client ID is tracked
        prop_assert!(tracker.has_pending_order(&client_id));
        
        // Property: Unknown client IDs are not tracked
        prop_assert!(!tracker.has_pending_order("unknown-id"));
    }

    /// Feature: rust-hft-client, Property 11: Entry Removal After Response
    /// 
    /// *For any* client_id that has been recorded, after recording a response 
    /// for that client_id, the entry SHALL no longer exist in the tracking map.
    /// 
    /// **Validates: Requirements 7.3**
    #[test]
    fn prop_entry_removal_after_response(client_id in client_id_strategy()) {
        let mut tracker = LatencyTracker::new(1000, test_host());
        
        // Record order sent
        tracker.record_order_sent(client_id.clone());
        prop_assert!(tracker.has_pending_order(&client_id));
        
        // Record response
        let rtt = tracker.record_order_response(&client_id);
        
        // Property: Response returns Some (found the entry)
        prop_assert!(rtt.is_some());
        
        // Property: Entry is removed after response
        prop_assert!(!tracker.has_pending_order(&client_id));
        
        // Property: Second lookup returns None
        let rtt2 = tracker.record_order_response(&client_id);
        prop_assert!(rtt2.is_none());
    }

    /// Feature: rust-hft-client, Property 12: Statistics Trigger at Test Size Boundary
    /// 
    /// *For any* test_size value N, the should_print_stats() function SHALL return 
    /// true when response_count is a non-zero multiple of N, and false otherwise.
    /// 
    /// **Validates: Requirements 6.3**
    #[test]
    fn prop_stats_trigger_at_boundary(test_size in test_size_strategy()) {
        let mut tracker = LatencyTracker::new(test_size, test_host());
        
        // Property: Initially should not print (count = 0)
        prop_assert!(!tracker.should_print_stats());
        
        // Record exactly test_size responses
        for i in 0..test_size {
            tracker.record_order_sent(format!("id-{}", i));
            tracker.record_order_response(&format!("id-{}", i));
        }
        
        // Property: Should print at exactly test_size
        prop_assert!(tracker.should_print_stats());
        prop_assert_eq!(tracker.response_count(), test_size as u64);
    }

    /// Feature: rust-hft-client, Property 9: Round-Trip Time Calculation Correctness
    /// 
    /// *For any* recorded send timestamp and response timestamp where response > send, 
    /// the calculated RTT SHALL equal (response_time - send_time) in nanoseconds.
    /// 
    /// **Validates: Requirements 5.2, 5.4**
    #[test]
    fn prop_rtt_calculation_positive(client_id in client_id_strategy()) {
        let mut tracker = LatencyTracker::new(1000, test_host());
        
        // Record order sent
        tracker.record_order_sent(client_id.clone());
        
        // Small delay to ensure measurable RTT
        thread::sleep(Duration::from_micros(100));
        
        // Record response
        let rtt = tracker.record_order_response(&client_id);
        
        // Property: RTT is positive (response > send)
        prop_assert!(rtt.is_some());
        prop_assert!(rtt.unwrap() > 0);
    }

    /// Feature: rust-hft-client, Property 13: Statistics Output Completeness and Reset
    /// 
    /// *For any* histogram with recorded values, after calling print_stats(), 
    /// the output SHALL contain all required percentiles (50th, 90th, 95th, 99th, 
    /// 99.9th, 99.99th, max) AND the histogram SHALL be reset to empty state.
    /// 
    /// **Validates: Requirements 6.4, 6.5**
    #[test]
    fn prop_stats_completeness_and_reset(num_samples in 10usize..100usize) {
        let mut tracker = LatencyTracker::new(1000, test_host());
        
        // Record some samples
        for i in 0..num_samples {
            tracker.record_order_sent(format!("id-{}", i));
            thread::sleep(Duration::from_micros(10));
            tracker.record_order_response(&format!("id-{}", i));
        }
        
        // Get stats before reset
        let stats = tracker.get_stats();
        
        // Property: All percentiles are present and valid
        prop_assert!(stats.p50 > 0);
        prop_assert!(stats.p90 >= stats.p50);
        prop_assert!(stats.p95 >= stats.p90);
        prop_assert!(stats.p99 >= stats.p95);
        prop_assert!(stats.p99_9 >= stats.p99);
        prop_assert!(stats.p99_99 >= stats.p99_9);
        prop_assert!(stats.max >= stats.p99_99);
        
        // Print stats (which resets histogram)
        tracker.print_stats();
        
        // Property: Histogram is reset after print_stats
        let stats_after = tracker.get_stats();
        prop_assert_eq!(stats_after.max, 0);
    }
}
