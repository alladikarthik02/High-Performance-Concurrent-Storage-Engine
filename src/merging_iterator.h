#pragma once
#include <vector>

#include "lsmeng/dbformat.h"
#include "lsmeng/iterator.h"

namespace lsmeng {

// SPEC 3.12. A k-way merge over child iterators, in internal-key order.
//
// Because equal user keys sort newest-first (SPEC 3.1), a merge over the memtable, the
// immutable memtable, and every SST yields, for each user key, its newest version FIRST.
// That single property is what lets both the read path and compaction dedup with a
// one-entry memory of what they just saw.
//
// The smallest child is found by a LINEAR SCAN rather than a heap. n here is the number of
// live SSTs plus two memtables -- typically under twenty -- and at that size a scan of a
// contiguous array beats a heap's pointer chasing and is far easier to get right in both
// directions. If tier counts ever grow enough for this to matter, `lsmeng.blocks-read` and
// a profile will say so; guessing now would be optimising before measuring.
Iterator* NewMergingIterator(Iterator** children, int n);

}  // namespace lsmeng
