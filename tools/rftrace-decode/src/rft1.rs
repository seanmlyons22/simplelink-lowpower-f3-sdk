//! RFT1 — the USB wire protocol emitted by the rftrace-pico firmware (a Raspberry Pi Pico that
//! deframes the RF-core tracer pin and streams the recovered 10-bit words over USB CDC).
//!
//! ```text
//! [ "RFT1" magic, once on connect ]  then a stream of little-endian u16 frames:
//!   0x0000..=0x03FF  Trace(word)     a 10-bit tracer word (NOPs already dropped by the Pico)
//!   0x8000..=0xBFFF  Overflow(n)     control: n words were dropped on a ring-full burst
//!   0xC000..=0xFFFF  Status(rate)    control: lock/health; rate = measured tracer bit rate
//!                                    in 2 kHz units (0 = no lock / line idle)
//! ```
//!
//! Bit 15 marks control frames; bit 14 splits them into overflow (clear) and status (set),
//! which is why the overflow payload is 14 bits. A decoder must ignore any control frame it
//! does not recognise (forward compatibility within the RFT1 control space).
//!
//! This is the decode side. The normative definition of the format is the `rftrace-wire` crate in
//! the rftrace-pico repo (which the firmware encodes with); it is vendored here — rather than
//! path-depended — so `tracedecode` keeps its zero-dependency, offline-buildable property. An
//! `xtask` drift guard in the Pico repo asserts that `rftrace_wire::Frame::to_bytes` round-trips
//! through this decoder for the whole trace/overflow/status space, so the copy can't silently
//! diverge.

/// Stream magic, emitted once when the USB link comes up. Also the format version tag.
pub const MAGIC: [u8; 4] = *b"RFT1";

/// Set on control frames; a 10-bit trace word never has it set.
const CONTROL_BIT: u16 = 0x8000;
/// Bit 14 selects the status half of the control space (clear = overflow).
const STATUS_BIT: u16 = 0x4000;
/// Payload mask for control frames (the overflow count / the status rate field).
const CONTROL_PAYLOAD_MASK: u16 = 0x3FFF;
/// A tracer word is 10 bits.
const TRACE_MASK: u16 = 0x03FF;

/// The [`Frame::Status`] rate field is in units of 2 kHz (nominal 24 MHz -> 12000).
pub const STATUS_RATE_UNIT_HZ: u32 = 2_000;

/// One decoded unit of the RFT1 stream.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Frame {
    /// A 10-bit tracer word (`0x001..=0x3FF`; NOP `0x000` is dropped by the firmware).
    Trace(u16),
    /// The firmware dropped `n` words because its RAM ring filled during a burst.
    Overflow(u16),
    /// Lock/health report: the measured tracer bit rate in [`STATUS_RATE_UNIT_HZ`] units,
    /// `0` = no lock (line idle / rate not measurable). Emitted ~1 Hz and on lock
    /// transitions; not a log record — surface it, don't count it as data.
    Status(u16),
}

impl Frame {
    /// Encode this frame as its little-endian 2-byte wire form (used by the synth/test fixtures).
    #[must_use]
    pub fn to_bytes(self) -> [u8; 2] {
        let v = match self {
            Frame::Trace(w) => w & TRACE_MASK,
            Frame::Overflow(n) => CONTROL_BIT | (n & CONTROL_PAYLOAD_MASK),
            Frame::Status(r) => CONTROL_BIT | STATUS_BIT | (r & CONTROL_PAYLOAD_MASK),
        };
        v.to_le_bytes()
    }

    /// Classify a raw little-endian u16 into a frame.
    #[must_use]
    pub fn from_u16(v: u16) -> Frame {
        if v & CONTROL_BIT == 0 {
            Frame::Trace(v & TRACE_MASK)
        } else if v & STATUS_BIT == 0 {
            Frame::Overflow(v & CONTROL_PAYLOAD_MASK)
        } else {
            Frame::Status(v & CONTROL_PAYLOAD_MASK)
        }
    }

    /// The measured bit rate carried by a [`Frame::Status`], in Hz (`0` = no lock).
    /// Returns `None` for non-status frames.
    #[must_use]
    pub fn status_hz(self) -> Option<u32> {
        match self {
            Frame::Status(r) => Some(u32::from(r) * STATUS_RATE_UNIT_HZ),
            _ => None,
        }
    }
}

/// Streaming byte -> frame decoder. Feed bytes as they arrive off USB; it first syncs on the
/// `RFT1` magic, then yields one [`Frame`] per completed little-endian u16. Bounded state, no
/// allocation.
#[derive(Clone, Debug, Default)]
pub struct Decoder {
    magic_matched: usize,
    lo: Option<u8>,
}

impl Decoder {
    /// A fresh decoder waiting for the magic.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// True once the `RFT1` magic has been seen and frames are being decoded.
    #[must_use]
    pub fn is_synced(&self) -> bool {
        self.magic_matched >= MAGIC.len()
    }

    /// Feed one byte. Returns `Some(frame)` when a byte completes a frame, else `None`
    /// (still syncing on the magic, or holding the low half of a u16).
    pub fn push(&mut self, b: u8) -> Option<Frame> {
        if self.magic_matched < MAGIC.len() {
            if b == MAGIC[self.magic_matched] {
                self.magic_matched += 1;
            } else {
                // `RFT1` has no repeated prefix, so a mismatch resets to matching the
                // first byte (handles a stray byte right before a valid magic).
                self.magic_matched = usize::from(b == MAGIC[0]);
            }
            return None;
        }
        match self.lo.take() {
            None => {
                self.lo = Some(b);
                None
            }
            Some(lo) => Some(Frame::from_u16(u16::from_le_bytes([lo, b]))),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn decode_all(bytes: &[u8]) -> Vec<Frame> {
        let mut d = Decoder::new();
        bytes.iter().filter_map(|&b| d.push(b)).collect()
    }

    #[test]
    fn trace_overflow_and_status_roundtrip() {
        for f in [
            Frame::Trace(0x001),
            Frame::Trace(0x3FF),
            Frame::Overflow(0),
            Frame::Overflow(42),
            Frame::Overflow(0x3FFF),
            Frame::Status(0),
            Frame::Status(12_000),
            Frame::Status(0x3FFF),
        ] {
            assert_eq!(Frame::from_u16(u16::from_le_bytes(f.to_bytes())), f);
        }
    }

    #[test]
    fn status_rate_units() {
        assert_eq!(Frame::Status(12_000).status_hz(), Some(24_000_000));
        assert_eq!(Frame::Status(0).status_hz(), Some(0)); // 0 = no lock
        assert_eq!(Frame::Trace(1).status_hz(), None);
        assert_eq!(Frame::Overflow(1).status_hz(), None);
    }

    #[test]
    fn trace_word_never_sets_control_bit() {
        for w in 0..=TRACE_MASK {
            assert_eq!(
                Frame::from_u16(u16::from_le_bytes(Frame::Trace(w).to_bytes())),
                Frame::Trace(w)
            );
        }
    }

    #[test]
    fn decoder_syncs_on_magic_then_yields_frames() {
        let mut bytes = Vec::from(MAGIC);
        for f in [Frame::Trace(0x2A), Frame::Overflow(7), Frame::Trace(0x3FF)] {
            bytes.extend_from_slice(&f.to_bytes());
        }
        assert_eq!(
            decode_all(&bytes),
            vec![Frame::Trace(0x2A), Frame::Overflow(7), Frame::Trace(0x3FF)]
        );
    }

    #[test]
    fn decoder_skips_junk_before_magic() {
        let mut bytes = vec![0x00, 0xFF, b'R', 0x99]; // false starts, incl. a lone 'R'
        bytes.extend_from_slice(&MAGIC);
        bytes.extend_from_slice(&Frame::Trace(0x123).to_bytes());
        assert_eq!(decode_all(&bytes), vec![Frame::Trace(0x123)]);
    }

    #[test]
    fn byte_at_a_time_matches_bulk() {
        let mut bytes = Vec::from(MAGIC);
        bytes.extend_from_slice(&Frame::Trace(0x0AB).to_bytes());
        assert_eq!(decode_all(&bytes), vec![Frame::Trace(0x0AB)]);
        assert!(!Decoder::new().is_synced());
    }
}
