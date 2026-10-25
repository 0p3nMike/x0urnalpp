//! Images on OneNote pages: what format the bytes are, and getting them into one Xournal++ reads.
//!
//! OneNote keeps pasted pictures and PDF printouts as image blobs with no reliable type field
//! (the parser's `extension()` is empty for every image in the notebooks tested, and the stored
//! file name is a generic "Untitled picture.png" or "<printout>.pdf"), so the type is read from
//! the bytes. PNG, JPEG, GIF and BMP go into the `.xopp` untouched (gdk-pixbuf reads them all).
//! Windows metafiles -- EMF and WMF, which older printouts use (CS 1's are EMF) -- are vector
//! formats Xournal++ cannot load, so they are rendered to PNG first.

use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Command;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Png,
    Jpeg,
    Gif,
    Bmp,
    Emf,
    Wmf,
    Unknown,
}

/// Identify an image from its first bytes.
pub fn sniff(b: &[u8]) -> Kind {
    if b.starts_with(&[0x89, b'P', b'N', b'G']) {
        Kind::Png
    } else if b.starts_with(&[0xff, 0xd8]) {
        Kind::Jpeg
    } else if b.starts_with(b"GIF8") {
        Kind::Gif
    } else if b.starts_with(b"BM") {
        Kind::Bmp
    } else if b.len() >= 44 && &b[40..44] == b" EMF" {
        // EMR_HEADER: the " EMF" signature sits at byte 40
        Kind::Emf
    } else if b.starts_with(&[0xd7, 0xcd, 0xc6, 0x9a]) || b.starts_with(&[0x01, 0x00, 0x09, 0x00]) {
        // placeable WMF, or a bare WMF header
        Kind::Wmf
    } else {
        Kind::Unknown
    }
}

/// Pixel size of a PNG, from its IHDR chunk. Used only when OneNote stored no picture size.
pub fn png_size(b: &[u8]) -> Option<(u32, u32)> {
    if sniff(b) != Kind::Png || b.len() < 24 {
        return None;
    }
    Some((
        u32::from_be_bytes([b[16], b[17], b[18], b[19]]),
        u32::from_be_bytes([b[20], b[21], b[22], b[23]]),
    ))
}

/// Renders metafiles to PNG through LibreOffice (`soffice --headless --convert-to png`), a
/// metafile renderer most Linux machines already have. Results are cached by content for the run, because a
/// printout's pages are separate images but the same picture is often pasted more than once.
pub struct MetafileRenderer {
    dir: PathBuf,
    cache: HashMap<Vec<u8>, Option<Vec<u8>>>,
    counter: usize,
}

impl MetafileRenderer {
    pub fn new() -> std::io::Result<Self> {
        let dir = std::env::temp_dir().join(format!("one2book-{}", std::process::id()));
        std::fs::create_dir_all(&dir)?;
        Ok(Self { dir, cache: HashMap::new(), counter: 0 })
    }

    /// PNG bytes for an EMF/WMF blob, or None if the renderer failed (the caller logs it).
    pub fn to_png(&mut self, data: &[u8], kind: Kind) -> Option<Vec<u8>> {
        if let Some(hit) = self.cache.get(data) {
            return hit.clone();
        }
        self.counter += 1;
        let ext = if kind == Kind::Emf { "emf" } else { "wmf" };
        let src = self.dir.join(format!("m{}.{ext}", self.counter));
        let out = self.dir.join(format!("m{}.png", self.counter));
        let result = std::fs::write(&src, data).ok().and_then(|_| {
            let ok = Command::new("soffice")
                .args(["--headless", "--convert-to", "png", "--outdir"])
                .arg(&self.dir)
                .arg(&src)
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .status()
                .map(|s| s.success())
                .unwrap_or(false);
            if ok { std::fs::read(&out).ok().map(strip_png_metadata) } else { None }
        });
        let _ = std::fs::remove_file(&src);
        let _ = std::fs::remove_file(&out);
        self.cache.insert(data.to_vec(), result.clone());
        result
    }
}

/// Drop the PNG chunks that only say when and by what a picture was made (tIME, tEXt, zTXt,
/// iTXt). The same OneNote page has to give the same bytes on every import (the books are git
/// repos), and a renderer is free to stamp those on every run; the picture
/// itself is untouched. Anything that doesn't parse as PNG chunks is returned as it came.
pub fn strip_png_metadata(png: Vec<u8>) -> Vec<u8> {
    const SIGNATURE: usize = 8;
    if sniff(&png) != Kind::Png || png.len() < SIGNATURE {
        return png;
    }
    let mut out = png[..SIGNATURE].to_vec();
    let mut i = SIGNATURE;
    // each chunk: length (4, big-endian), type (4), data (length), CRC (4) over type + data
    while i + 12 <= png.len() {
        let len = u32::from_be_bytes([png[i], png[i + 1], png[i + 2], png[i + 3]]) as usize;
        let Some(end) = (i + 12).checked_add(len).filter(|&e| e <= png.len()) else { return png };
        if !matches!(&png[i + 4..i + 8], b"tIME" | b"tEXt" | b"zTXt" | b"iTXt") {
            out.extend_from_slice(&png[i..end]);
        }
        i = end;
    }
    if i == png.len() { out } else { png }
}

impl Drop for MetafileRenderer {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chunk(kind: &[u8; 4], data: &[u8]) -> Vec<u8> {
        // the CRC isn't checked here, so any four bytes do
        let mut c = (data.len() as u32).to_be_bytes().to_vec();
        c.extend_from_slice(kind);
        c.extend_from_slice(data);
        c.extend_from_slice(&[1, 2, 3, 4]);
        c
    }

    #[test]
    fn png_time_and_text_chunks_are_dropped() {
        let sig = [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
        let ihdr = chunk(b"IHDR", &[0; 13]);
        let idat = chunk(b"IDAT", b"pixels");
        let iend = chunk(b"IEND", b"");
        let with = [&sig[..], &ihdr, &chunk(b"tIME", &[7; 7]), &chunk(b"tEXt", b"Software\0LibreOffice"), &idat, &iend].concat();
        let without = [&sig[..], &ihdr, &idat, &iend].concat();
        assert_eq!(strip_png_metadata(with), without);
        assert_eq!(strip_png_metadata(without.clone()), without);
        // not a PNG, or cut short: left alone
        assert_eq!(strip_png_metadata(b"GIF89a".to_vec()), b"GIF89a".to_vec());
        let cut = [&sig[..], &ihdr[..10]].concat();
        assert_eq!(strip_png_metadata(cut.clone()), cut);
    }
}
