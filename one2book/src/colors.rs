//! OneNote colours, and the one recolouring rule the import applies.
//!
//! The converted notebooks use X0urnal++'s whiteboard look -- the void ground with the blue graph
//! grid -- and because Xournal++ draws ink colours literally, black ink would vanish into it. So black (and near-black) ink and text become the house
//! off-white; EVERY other colour is kept exactly as OneNote stored it.

use crate::xopp::Rgb;
use onenote_parser::property::common::ColorRef;

/// The whiteboard ground: the void every new X0urnal++ page uses.
pub const VOID: Rgb = Rgb(0x05, 0x07, 0x0a);

/// What black ink turns into on the void: the house palette's "bone" foreground.
pub const BONE: Rgb = Rgb(0xd8, 0xd4, 0xc4);

/// Anything at or below this in every channel counts as black. OneNote's own black pen is
/// 000000; this also catches the very dark greys some pens produce, without touching dark but
/// clearly coloured inks (OneNote's dark blue, 004f8b, keeps its colour).
const NEAR_BLACK: u8 = 0x40;

/// Apply the recolouring rule to one colour.
pub fn on_void(c: Rgb) -> Rgb {
    if c.0 <= NEAR_BLACK && c.1 <= NEAR_BLACK && c.2 <= NEAR_BLACK { BONE } else { c }
}

/// An ink stroke's colour. OneNote stores a Windows COLORREF: red in the LOW byte, then green,
/// then blue (0x00BBGGRR). Joplin's renderer and one2html decode it the same way.
pub fn ink(colorref: Option<u32>) -> Rgb {
    let v = colorref.unwrap_or(0);
    on_void(Rgb((v & 0xff) as u8, ((v >> 8) & 0xff) as u8, ((v >> 16) & 0xff) as u8))
}

/// A text colour. `Auto` (or no colour at all) is OneNote's default text colour, black.
pub fn text(c: Option<ColorRef>) -> Rgb {
    match c {
        Some(ColorRef::Manual { r, g, b }) => on_void(Rgb(r, g, b)),
        _ => BONE,
    }
}
