use std::collections::HashMap;

use crate::onestore::desktop::file_structure::{
    TransactionLogFragment, file_node_list_fragment::FileNodeListHeader,
};

#[derive(Default)]
pub(crate) struct ParseContext {
    file_node_count_mapping: HashMap<u32, usize>,
}

impl ParseContext {
    pub(crate) fn new() -> Self {
        Self {
            file_node_count_mapping: HashMap::new(),
        }
    }

    /// Returns the maximum number of nodes in the provided node list
    pub(crate) fn get_file_node_count(&self, header: &FileNodeListHeader) -> Option<usize> {
        self.file_node_count_mapping
            .get(&header.file_node_list_id)
            .cloned()
    }

    pub(crate) fn update_remaining_nodes_in_fragment(
        &mut self,
        header: &FileNodeListHeader,
        remaining: usize,
    ) {
        self.file_node_count_mapping
            .insert(header.file_node_list_id, remaining);
    }

    /// one2book fix: [MS-ONESTORE] 2.3.3 - only the first `c_transactions_in_log`
    /// transactions (`Header.cTransactionsInLog`) are committed; the entries of later
    /// transactions, and the FileNodes they added, MUST be ignored. Per 2.3.3.2 each
    /// non-sentinel entry holds the *new* number of FileNodes in list `srcID`, and per 2.4.1
    /// the count that ends a list is the one from the last committed TransactionEntry that
    /// modified it. So this records each list's count from its last committed entry instead
    /// of the maximum over every entry in the log.
    pub(crate) fn update_from_transaction_log(
        &mut self,
        log: &Vec<TransactionLogFragment>,
        c_transactions_in_log: u32,
    ) {
        // cTransactionsInLog "MUST NOT be 0" (2.3.1). If it is 0 anyway, there's no usable
        // commit boundary, so fall back to reading every entry (the old behaviour).
        let committed = if c_transactions_in_log == 0 {
            log::warn!("Header.cTransactionsInLog is 0; using every transaction log entry");
            usize::MAX
        } else {
            c_transactions_in_log as usize
        };
        let mut transactions_seen = 0usize;

        'fragments: for fragment in log {
            // See [MS-ONESTORE 2.3.3.2](https://learn.microsoft.com/en-us/openspecs/office_file_formats/ms-onestore/c00897d9-d90a-4707-b9fb-58c93e490322)
            // The transaction entries specify the number of nodes in the FileNodeListFragments.
            for entry in &fragment.size_table {
                if transactions_seen >= committed {
                    break 'fragments;
                }
                if entry.is_sentinel() {
                    transactions_seen += 1;
                    continue;
                }

                let new_count = entry.transaction_entry_switch as usize;
                self.file_node_count_mapping.insert(entry.src_id, new_count);
            }
        }
    }
}
