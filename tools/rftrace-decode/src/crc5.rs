//! CRC-5 for the tracer packet trailer.
//!
//! Two things live here:
//!  1. `Crc5` - the incremental MSB-first CRC-5 (poly 0x05, init 0x1F, no xorout) the wire
//!     actually uses. Both the packet assembler and the synth encoder share it so encode and
//!     decode stay self-consistent. It accumulates `word[7:0]` of the SOP, timestamp, header,
//!     and data words, then the top 3 bits (`word[7:5]`) of EOP; the transmitted CRC sits in
//!     `EOP[4:0]`.
//!  2. `crc5_usb` - a textbook reflected CRC-5/USB (init 0x1F, reflected poly, xorout 0x1F,
//!     check("123456789") = 0x19). It does NOT match the wire; it is kept only as a self-test
//!     anchor proving our 5-bit CRC arithmetic against a published check value.
//!
//! The MSB-first convention was confirmed empirically against the golden
//! `tx_burst_example.sal` capture: it validates 28 of 28 real packets, while the reflected
//! USB form validated only 1 of 28.

/// Textbook CRC-5/USB over whole bytes (reflected). `check("123456789") == 0x19`.
pub fn crc5_usb(data: &[u8]) -> u8 {
    let mut crc: u16 = 0x1f;
    for &b in data {
        crc ^= b as u16;
        for _ in 0..8 {
            if crc & 1 != 0 {
                crc = (crc >> 1) ^ 0x14; // reflect(0x05) within 5 bits
            } else {
                crc >>= 1;
            }
        }
    }
    ((crc ^ 0x1f) & 0x1f) as u8
}

/// Incremental MSB-first CRC-5 (poly 0x05, init 0x1F) used inside packet assembly + synth.
#[derive(Clone, Copy, Debug)]
pub struct Crc5 {
    reg: u8,
}

impl Default for Crc5 {
    fn default() -> Self {
        Crc5 { reg: 0x1f }
    }
}

impl Crc5 {
    pub fn new() -> Self {
        Self::default()
    }

    #[inline]
    fn update_bit(&mut self, bit: u8) {
        let fb = ((self.reg >> 4) & 1) ^ (bit & 1);
        self.reg = (self.reg << 1) & 0x1f;
        if fb != 0 {
            self.reg ^= 0x05;
        }
    }

    /// Feed a full byte, MSB first (used for SOP/TS/HDR/DATA `word[7:0]`).
    pub fn update_byte(&mut self, byte: u8) {
        for i in (0..8).rev() {
            self.update_bit((byte >> i) & 1);
        }
    }

    /// Feed the low `n` bits of `val`, MSB first (used for the 3 non-CRC MSBs of EOP).
    pub fn update_bits(&mut self, val: u8, n: u8) {
        for i in (0..n).rev() {
            self.update_bit((val >> i) & 1);
        }
    }

    pub fn value(&self) -> u8 {
        self.reg & 0x1f
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn usb_anchor() {
        assert_eq!(crc5_usb(b"123456789"), 0x19);
        assert_eq!(crc5_usb(b""), 0x00); // init ^ xorout with no data
    }

    #[test]
    fn wire_crc_differs_from_usb() {
        // The two conventions must not be interchangeable; if they ever agree on this
        // input, one of them has been edited into the other by mistake.
        let mut c = Crc5::new();
        for b in b"123456789" {
            c.update_byte(*b);
        }
        assert_ne!(c.value(), crc5_usb(b"123456789"));
    }

    #[test]
    fn incremental_matches_bitwise_reference() {
        // Reference: plain MSB-first polynomial division, poly 0x05, init 0x1F.
        fn reference(bits: &[u8]) -> u8 {
            let mut reg: u8 = 0x1f;
            for &bit in bits {
                let fb = ((reg >> 4) & 1) ^ (bit & 1);
                reg = (reg << 1) & 0x1f;
                if fb != 0 {
                    reg ^= 0x05;
                }
            }
            reg
        }
        let data = [0x53u8, 0x00, 0x2A, 0x05];
        let mut bits = Vec::new();
        for b in data {
            for i in (0..8).rev() {
                bits.push((b >> i) & 1);
            }
        }
        bits.push(0);
        bits.push(1);
        bits.push(1);
        let mut c = Crc5::new();
        for b in data {
            c.update_byte(b);
        }
        c.update_bits(0b011, 3);
        assert_eq!(c.value(), reference(&bits));
    }
}
