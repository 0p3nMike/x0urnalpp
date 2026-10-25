use super::parse_context::ParseContext;
use crate::errors::ErrorKind;
use crate::errors::Result;
use crate::onestore::desktop::common::FileChunkReference64x32;
use crate::onestore::desktop::file_node::FileNode;
use crate::onestore::desktop::file_node::FileNodeData;
use crate::onestore::desktop::parse::Parse;

#[derive(Debug, Clone)]
#[allow(dead_code)]
pub(crate) struct FileNodeListFragment {
    pub(crate) header: FileNodeListHeader,
    pub(crate) file_nodes: Vec<FileNode>,
    pub(crate) next_fragment: FileChunkReference64x32,
    pub(crate) footer: u64,
}

impl FileNodeListFragment {
    pub(crate) fn parse(
        reader: crate::Reader,
        context: &mut ParseContext,
        size: usize,
    ) -> Result<Self> {
        let header = FileNodeListHeader::parse(reader)?;
        let mut file_nodes: Vec<FileNode> = Vec::new();
        let mut file_node_size: usize = 0;

        let remaining_0 = reader.remaining();

        // Sometimes, the node count is specified externally
        // one2book fix: remember whether the committed transaction log gave a count for this
        // list ([MS-ONESTORE] 2.4.1), and only warn when it didn't (`unwrap_or` evaluated the
        // warning eagerly, so it fired for every fragment).
        let committed_count = context.get_file_node_count(&header);
        let mut maximum_node_count = committed_count.unwrap_or_else(|| {
            log::warn!("No node count found.");
            usize::MAX
        });

        while size - 36 - file_node_size >= 4 && maximum_node_count > 0 {
            // one2book fix: [MS-ONESTORE] 2.4.1 says the bytes after the last FileNode are
            // padding, "undefined and MUST be ignored", and large sections have leftover data
            // there. Look at the next FileNode header (2.4.3) before decoding it, and stop
            // when it can't be the start of a real node.
            let bytes_before_next_fragment = size - 36 - file_node_size;
            let header_bytes = reader.peek_bytes(4)?;
            let header_word = u32::from_le_bytes([
                header_bytes[0],
                header_bytes[1],
                header_bytes[2],
                header_bytes[3],
            ]);
            if is_end_of_node_stream(
                header_word,
                bytes_before_next_fragment,
                committed_count.is_some(),
                header.file_node_list_id,
            ) {
                break;
            }

            let file_node = FileNode::parse(reader, context)?;
            file_node_size += file_node.size;
            let is_chunk_terminator = matches!(file_node.fnd, FileNodeData::ChunkTerminatorFND);

            if !matches!(
                file_node.fnd,
                FileNodeData::ChunkTerminatorFND | FileNodeData::Null
            ) {
                maximum_node_count -= 1;
            }
            if !matches!(file_node.fnd, FileNodeData::Null) {
                file_nodes.push(file_node);
            }

            let consumed = remaining_0 - reader.remaining();
            if consumed != file_node_size {
                return Err(ErrorKind::MalformedOneNoteFileData(
                    format!(
                        "FileNodeListFragment size accounting mismatch: reader consumed {consumed} bytes, declared sizes summed to {file_node_size}"
                    )
                    .into(),
                )
                .into());
            }

            // one2book fix: [MS-ONESTORE] 2.4.1 - a ChunkTerminatorFND (FileNodeID 0x0FF) ends
            // this fragment's node stream; everything after it up to nextFragment is padding
            // and the list continues in the next fragment. (It stays in `file_nodes`, as
            // before; FileNodeListBuilder filters it out.)
            if is_chunk_terminator {
                break;
            }
        }

        // one2book fix: only carry a remaining count over to the next fragment when the list
        // has a committed count; storing `usize::MAX - n` made later fragments of a list
        // without one look as if they had a count.
        if committed_count.is_some() {
            context.update_remaining_nodes_in_fragment(&header, maximum_node_count);
        }

        let padding_length = size - 36 - file_node_size;
        reader.advance(padding_length)?;

        let next_fragment = FileChunkReference64x32::parse(reader)?;

        let footer = reader.get_u64()?;
        if footer != 0x8BC215C38233BA4B {
            return Err(ErrorKind::MalformedOneStoreData(
                format!("Invalid footer: {:#0x}", footer).into(),
            )
            .into());
        }

        Ok(Self {
            header,
            file_nodes,
            next_fragment,
            footer,
        })
    }
}

/// one2book fix: decides, from the 4-byte FileNode header word at the current position,
/// whether the fragment's node stream has ended and the rest is padding ([MS-ONESTORE] 2.4.1).
///
/// - FileNodeID 0 when the committed transaction log gives no node count for the list: no
///   FileNode type has ID 0, so this is the start of padding (Apache Tika and pyOneNote stop
///   here too). With a committed count, ID-0 words keep their old handling (skipped as `Null`)
///   unless the header is implausible.
/// - An implausible header ([MS-ONESTORE] 2.4.3): the Reserved bit is not 1, BaseType is not
///   0, 1 or 2, or Size is under 4 bytes (the header alone) or runs past nextFragment. Such
///   a word can't start a real FileNode; decoding it made the parser read garbage and then
///   fail with UnexpectedEof or a size mismatch.
fn is_end_of_node_stream(
    header_word: u32,
    bytes_before_next_fragment: usize,
    has_committed_count: bool,
    file_node_list_id: u32,
) -> bool {
    let node_id = header_word & 0x3FF;
    let size = ((header_word >> 10) & 0x1FFF) as usize;
    let base_type = (header_word >> 27) & 0xF;
    let reserved = header_word >> 31;

    if node_id == 0 && !has_committed_count {
        log::debug!(
            "FileNodeListFragment (list {:#x}): FileNodeID 0 with {} bytes before nextFragment; treating the rest as padding",
            file_node_list_id,
            bytes_before_next_fragment
        );
        return true;
    }

    if reserved != 1 || base_type > 2 || size < 4 || size > bytes_before_next_fragment {
        log::warn!(
            "FileNodeListFragment (list {:#x}): implausible FileNode header {:#010x} (id {:#x}, size {}, base type {}, reserved bit {}) with {} bytes before nextFragment; treating the rest of the fragment as padding",
            file_node_list_id,
            header_word,
            node_id,
            size,
            base_type,
            reserved,
            bytes_before_next_fragment
        );
        return true;
    }

    false
}

#[derive(Debug, Clone)]
#[allow(dead_code)]
pub(crate) struct FileNodeListHeader {
    magic: u64,
    pub(crate) file_node_list_id: u32,
    pub(crate) n_fragment_sequence: u32,
}

impl Parse for FileNodeListHeader {
    fn parse(reader: crate::Reader) -> Result<Self> {
        let magic = u64::parse(reader)?;
        if magic != 0xA4567AB1F5F7F4C4 {
            return Err(ErrorKind::ParseValidationFailed(
                "Failed to validate: magic == 0xA4567AB1F5F7F4C4".into(),
            )
            .into());
        }

        let file_node_list_id = u32::parse(reader)?;
        if file_node_list_id < 0x0010 {
            log::warn!(
                "FileNodeListHeader: file_node_list_id {:#x} is below spec minimum 0x10",
                file_node_list_id
            );
        }

        let n_fragment_sequence = u32::parse(reader)?;

        Ok(Self {
            magic,
            file_node_list_id,
            n_fragment_sequence,
        })
    }
}

// one2book fix: regression tests for the [MS-ONESTORE] 2.4.1 end-of-stream rules.
#[cfg(test)]
mod tests {
    use super::*;

    const REVISION_MANIFEST_END: u32 = 0x8000_101C; // id 0x01C, size 4, base type 0, reserved 1
    const CHUNK_TERMINATOR: u32 = 0x8000_10FF; // id 0x0FF, size 4, base type 0, reserved 1

    /// Builds a FileNodeListFragment for list 0x10: header, `body` (nodes + padding),
    /// nextFragment = (0x1000, 0x40), footer.
    fn fragment(body: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(&0xA4567AB1F5F7F4C4u64.to_le_bytes());
        out.extend_from_slice(&0x10u32.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(body);
        out.extend_from_slice(&0x1000u64.to_le_bytes());
        out.extend_from_slice(&0x40u32.to_le_bytes());
        out.extend_from_slice(&0x8BC215C38233BA4Bu64.to_le_bytes());
        out
    }

    fn context_with_count(count: usize) -> ParseContext {
        let mut context = ParseContext::new();
        let header = FileNodeListHeader {
            magic: 0xA4567AB1F5F7F4C4,
            file_node_list_id: 0x10,
            n_fragment_sequence: 0,
        };
        context.update_remaining_nodes_in_fragment(&header, count);
        context
    }

    fn parse(data: &[u8], context: &mut ParseContext) -> Result<FileNodeListFragment> {
        let mut reader = crate::reader::Reader::new(data);
        FileNodeListFragment::parse(&mut reader, context, data.len())
    }

    #[test]
    fn chunk_terminator_ends_stream_before_garbage_padding() {
        // Leftover bytes after the terminator, as seen in large desktop sections.
        let mut body = Vec::new();
        body.extend_from_slice(&REVISION_MANIFEST_END.to_le_bytes());
        body.extend_from_slice(&CHUNK_TERMINATOR.to_le_bytes());
        body.extend_from_slice(&[0xce, 0x20, 0x45, 0x8b, 0x1c, 0x0a]);
        let data = fragment(&body);

        let mut context = context_with_count(5);
        let fragment = parse(&data, &mut context).unwrap();
        assert_eq!(fragment.file_nodes.len(), 2);
        assert!(matches!(
            fragment.file_nodes[1].fnd,
            FileNodeData::ChunkTerminatorFND
        ));
        assert_eq!(fragment.next_fragment.stp, 0x1000);
        // One node used; the rest of the count carries over to the next fragment.
        assert_eq!(context.get_file_node_count(&fragment.header), Some(4));
    }

    #[test]
    fn node_id_zero_ends_stream_without_committed_count() {
        // A zero word followed by a word that decodes as "id 0x1, size 0".
        let mut body = Vec::new();
        body.extend_from_slice(&REVISION_MANIFEST_END.to_le_bytes());
        body.extend_from_slice(&0u32.to_le_bytes());
        body.extend_from_slice(&0x8000_0001u32.to_le_bytes());
        let data = fragment(&body);

        let fragment = parse(&data, &mut ParseContext::new()).unwrap();
        assert_eq!(fragment.file_nodes.len(), 1);
        assert_eq!(fragment.next_fragment.stp, 0x1000);
    }

    #[test]
    fn implausible_header_is_treated_as_padding() {
        // Size 4410 with only 8 bytes left before nextFragment.
        let oversized: u32 = 0x8000_0000 | (4410 << 10) | 0x0CE;
        let mut body = Vec::new();
        body.extend_from_slice(&REVISION_MANIFEST_END.to_le_bytes());
        body.extend_from_slice(&oversized.to_le_bytes());
        body.extend_from_slice(&[0xAA; 4]);
        let data = fragment(&body);

        let fragment = parse(&data, &mut context_with_count(5)).unwrap();
        assert_eq!(fragment.file_nodes.len(), 1);
        assert_eq!(fragment.next_fragment.stp, 0x1000);
    }

    #[test]
    fn plausible_header_check() {
        assert!(!is_end_of_node_stream(REVISION_MANIFEST_END, 4, true, 0x10));
        assert!(!is_end_of_node_stream(CHUNK_TERMINATOR, 4, false, 0x10));
        // Reserved bit clear
        assert!(is_end_of_node_stream(0x0000_101C, 64, true, 0x10));
        // BaseType 3
        assert!(is_end_of_node_stream(0x9800_101C, 64, true, 0x10));
        // Size below 4
        assert!(is_end_of_node_stream(0x8000_0001, 64, true, 0x10));
        // Runs past nextFragment
        assert!(is_end_of_node_stream(REVISION_MANIFEST_END, 3, true, 0x10));
    }
}
