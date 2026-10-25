//! Node ids for the book tree (format version 2; the contract is ../book-sync/README.md "Ids").
//!
//! Synced books are written on every device and re-imported from OneNote, so an id has to mean
//! the same page in every import. v1 counted (n1, n2, ... in document order): one
//! page added near the front renumbered every page after it, which a merge would read as every
//! page replaced. Ids now come from what OneNote itself keeps stable:
//!
//!   a page                                  on-<OneNote page GUID, lowercase, no braces>
//!   a section / section group               on-sec-<first 16 hex of sha256(path in notebook)>
//!   the chapter a page with subpages        on-chap-<that page's GUID>
//!     becomes (a section can't hold others)
//!
//! The page GUID is OneNote's own (PageMetadata's NotebookManagementEntityGuid, which the parser
//! hands out as `Page::link_target_id()` formatted "{6F1C0B2E-...}", upper case and braced);
//! editing or renaming a page keeps it. Sections have no id the import can rely on -- the file
//! identity may change when OneNote replaces the file -- so their path inside the notebook names
//! them: "Group A/Discrete". Renaming or moving a section therefore gives it a new id, while its
//! pages keep theirs, so a merge sees the pages move rather than vanish.
//!
//! Every id is also a file name (`sections/<id>.xopp`), so only [a-z0-9-] ever gets through.

use sha2::{Digest, Sha256};
use std::collections::HashSet;

/// `on-<guid>` for a page, from the parser's link target id ("{6F1C0B2E-...}").
pub fn page_id(link_target_id: &str) -> String {
    format!("on-{}", normalise_guid(link_target_id))
}

/// `on-sec-<16 hex>` for a section or section group at `path` ("Group A/Discrete").
pub fn section_id(path: &str) -> String {
    let digest = Sha256::digest(path.as_bytes());
    let hex: String = digest.iter().take(8).map(|b| format!("{b:02x}")).collect();
    format!("on-sec-{hex}")
}

/// `on-chap-<guid>` for the chapter made around a page with subpages, from that page's id.
pub fn chapter_id(page_id: &str) -> String {
    format!("on-chap-{}", page_id.strip_prefix("on-").unwrap_or(page_id))
}

/// A section's or group's path inside the notebook: its parent's path, a slash, its name.
pub fn join_path(parent: &str, name: &str) -> String {
    if parent.is_empty() { name.to_string() } else { format!("{parent}/{name}") }
}

/// "{6F1C0B2E-AAAA-...}" -> "6f1c0b2e-aaaa-...". A GUID is only hex digits and hyphens; anything
/// else (there shouldn't be) is dropped so the id stays a safe file name, and an empty result
/// becomes "nil" rather than a bare "on-".
fn normalise_guid(raw: &str) -> String {
    let g: String = raw
        .chars()
        .map(|c| c.to_ascii_lowercase())
        .filter(|c| c.is_ascii_hexdigit() || *c == '-')
        .collect();
    if g.is_empty() { "nil".to_string() } else { g }
}

/// The ids handed out in one book, so none is used twice. A clash shouldn't happen (GUIDs are
/// unique and two sections can't share a path), but a copied file or a damaged GUID could make
/// one, and two nodes sharing an id would share one page file. The second gets `-2`, the third
/// `-3`, ... -- in document order, so the same input still gives the same ids -- and a warning
/// says so.
#[derive(Default)]
pub struct Ids {
    seen: HashSet<String>,
    pub warnings: Vec<String>,
}

impl Ids {
    /// Take `id` for the node described by `what` (for the warning), or the next free variant.
    pub fn claim(&mut self, id: String, what: &str) -> String {
        if self.seen.insert(id.clone()) {
            return id;
        }
        let mut n = 2;
        loop {
            let alt = format!("{id}-{n}");
            if self.seen.insert(alt.clone()) {
                self.warnings.push(format!("duplicate id {id} ({what}); written as {alt}"));
                return alt;
            }
            n += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn page_ids_are_the_lowercase_guid_without_braces() {
        assert_eq!(page_id("{6F1C0B2E-1A2B-4C3D-8E9F-0A1B2C3D4E5F}"), "on-6f1c0b2e-1a2b-4c3d-8e9f-0a1b2c3d4e5f");
        // already normalised, or with stray whitespace: the same id
        assert_eq!(page_id("6f1c0b2e-1a2b-4c3d-8e9f-0a1b2c3d4e5f"), "on-6f1c0b2e-1a2b-4c3d-8e9f-0a1b2c3d4e5f");
        assert_eq!(page_id(" {6F1C0B2E-1A2B-4C3D-8E9F-0A1B2C3D4E5F} "), "on-6f1c0b2e-1a2b-4c3d-8e9f-0a1b2c3d4e5f");
        assert_eq!(page_id(""), "on-nil");
    }

    #[test]
    fn section_ids_hash_the_path() {
        // printf 'Group A/Discrete' | sha256sum -> 73c18d588fa5ba60...
        assert_eq!(section_id("Group A/Discrete"), "on-sec-73c18d588fa5ba60");
        assert_eq!(section_id("Discrete"), "on-sec-1cf8f2179a77e83b");
        assert_eq!(section_id(&join_path("Group A", "Discrete")), section_id("Group A/Discrete"));
        assert_eq!(join_path("", "Discrete"), "Discrete");
    }

    #[test]
    fn chapter_ids_follow_their_page() {
        assert_eq!(chapter_id("on-6f1c0b2e-1a2b-4c3d-8e9f-0a1b2c3d4e5f"), "on-chap-6f1c0b2e-1a2b-4c3d-8e9f-0a1b2c3d4e5f");
        assert_eq!(chapter_id("on-6f1c0b2e-1a2b-4c3d-8e9f-0a1b2c3d4e5f-2"), "on-chap-6f1c0b2e-1a2b-4c3d-8e9f-0a1b2c3d4e5f-2");
    }

    #[test]
    fn duplicates_get_numbered_and_reported() {
        let mut ids = Ids::default();
        assert_eq!(ids.claim("on-a".into(), "page x"), "on-a");
        assert_eq!(ids.claim("on-a".into(), "page y"), "on-a-2");
        assert_eq!(ids.claim("on-a".into(), "page z"), "on-a-3");
        assert_eq!(ids.claim("on-b".into(), "page w"), "on-b");
        assert_eq!(ids.warnings.len(), 2);
        assert!(ids.warnings[0].contains("on-a-2") && ids.warnings[0].contains("page y"));
    }

    #[test]
    fn ids_are_safe_file_names() {
        for id in [page_id("{AB/../CD}"), section_id("a/b"), chapter_id(&page_id("{X Y}"))] {
            assert!(id.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-'), "{id}");
        }
    }
}
