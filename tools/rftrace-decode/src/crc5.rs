//! CRC-5 for the tracer packet trailer (§11.4).
//!
//! Two things live here:
//!  1. `crc5_usb` — a textbook **CRC-5/USB** (init 0x1F, poly 0x05 reflected, xorout 0x1F,
//!     check = 0x19). Sanity anchor that our arithmetic is a real CRC-5/USB.
//!  2. `Crc5` — an incremental accumulator used by BOTH the synth encoder and the packet
//!     assembler so they are self-consistent. It feeds `word[7:0]` for SOP/TS/HDR/DATA and
//!     the top 3 bits of EOP, per the RTL description.
//!
//! TODO(fable): confirm the exact bit convention (MSB-first here) and residue behaviour
//! against the golden `tx_burst_example.sal` packets. The RTL uses a 256-entry LUT on
//! `data XOR (crcReg<<3)`; if the golden CRCs don't validate, switch `Crc5` to the
//! reflected form (`crc5_usb` shows that arithmetic).

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
