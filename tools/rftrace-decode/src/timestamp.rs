//! Timestamp reconstruction (§11.5). 16-bit counter @ 2 MHz (0.5 us/tick), 32 ms rollover.

/// Running device-time reconstructor. Feed the 16-bit per-packet delta; get unwrapped ticks.
#[derive(Clone, Copy, Debug, Default)]
pub struct TsState {
    pub last: u64, // full unwrapped tick count
    have: bool,
}

impl TsState {
    pub fn new() -> Self {
        Self::default()
    }

    /// Merge a 16-bit counter snapshot with the running upper bits, handling rollover
    /// (add 0x10000 if the new value is >2000 ticks behind — matches `PacketParser.cpp`).
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

/// Convert 0.5 us ticks to microseconds (÷2), with the optional Agama/48 MHz extra ÷2.
pub fn ticks_to_us(ticks: u64, divide_by_2: bool) -> f64 {
    let us = ticks as f64 / 2.0;
    if divide_by_2 {
        us / 2.0
    } else {
        us
    }
}
