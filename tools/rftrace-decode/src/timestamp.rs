//! Timestamp reconstruction. The tracer stamps packets from a 16-bit counter running at
//! 2 MHz (bit clock / 12), i.e. 0.5 us per tick and a 32.768 ms range before the counter
//! wraps. Each timestamped packet carries a 16-bit snapshot; this module unwraps those
//! snapshots into a monotonically increasing full tick count.

/// Running device-time reconstructor. Feed the 16-bit per-packet snapshot; get unwrapped ticks.
#[derive(Clone, Copy, Debug, Default)]
pub struct TsState {
    pub last: u64, // full unwrapped tick count
    have: bool,
}

impl TsState {
    pub fn new() -> Self {
        Self::default()
    }

    /// Merge a 16-bit counter snapshot with the running upper bits. Rollover rule: if the
    /// merged value lands more than 2000 ticks behind the last one, the counter wrapped, so
    /// add 0x10000. The 2000-tick (1 ms) slack tolerates slightly out-of-order stamps from
    /// interleaved channels without declaring a false rollover.
    pub fn reconstruct(&mut self, delta16: u16) -> u64 {
        if !self.have {
            self.have = true;
            self.last = delta16 as u64;
            return self.last;
        }
        let upper = self.last & !0xFFFF;
        let mut val = upper | delta16 as u64;
        if val + 2000 < self.last {
            val += 0x1_0000; // rolled past the 16-bit boundary
        }
        self.last = val;
        val
    }

    /// If a packet carried no timestamp, reuse the last value.
    pub fn hold(&self) -> u64 {
        self.last
    }
}

/// Convert 0.5 us ticks to microseconds. Parts whose tracer runs at 48 MHz (e.g. CC2745)
/// tick at 0.25 us, so `divide_by_2` halves again to read as wall-clock.
pub fn ticks_to_us(ticks: u64, divide_by_2: bool) -> f64 {
    let us = ticks as f64 / 2.0;
    if divide_by_2 {
        us / 2.0
    } else {
        us
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_snapshot_seeds() {
        let mut ts = TsState::new();
        assert_eq!(ts.reconstruct(0x1234), 0x1234);
    }

    #[test]
    fn rollover_unwraps() {
        let mut ts = TsState::new();
        assert_eq!(ts.reconstruct(0xFFF0), 0xFFF0);
        assert_eq!(ts.reconstruct(0x0005), 0x1_0005);
        // and keeps counting across a second wrap
        assert_eq!(ts.reconstruct(0xFFFF), 0x1_FFFF);
        assert_eq!(ts.reconstruct(0x0001), 0x2_0001);
    }

    #[test]
    fn small_backstep_is_jitter_not_rollover() {
        // Interleaved channels can stamp slightly out of order; up to 2000 ticks
        // behind must NOT unwrap.
        let mut ts = TsState::new();
        ts.reconstruct(0x1000);
        assert_eq!(ts.reconstruct(0x0FFF), 0x0FFF);
        // Just beyond the slack does unwrap.
        let mut ts2 = TsState::new();
        ts2.reconstruct(0x1000);
        assert_eq!(ts2.reconstruct(0x0800), 0x1_0800);
    }

    #[test]
    fn hold_reuses_last() {
        let mut ts = TsState::new();
        ts.reconstruct(0x42);
        assert_eq!(ts.hold(), 0x42);
    }

    #[test]
    fn tick_conversion() {
        assert_eq!(ticks_to_us(4, false), 2.0); // 0.5 us/tick
        assert_eq!(ticks_to_us(4, true), 1.0); // 0.25 us/tick on 48 MHz parts
    }
}
