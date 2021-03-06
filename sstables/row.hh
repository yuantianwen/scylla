/*
 * Copyright (C) 2015 ScyllaDB
 *
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <boost/dynamic_bitset.hpp>

#include "bytes.hh"
#include "key.hh"
#include "core/temporary_buffer.hh"
#include "consumer.hh"
#include "sstables/types.hh"
#include "reader_concurrency_semaphore.hh"
#include "liveness_info.hh"
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/noncopyable_function.hh>
#include "utils/chunked_vector.hh"
#include "types.hh"
#include "gc_clock.hh"
#include "timestamp.hh"
#include "column_translation.hh"

// sstables::data_consume_row feeds the contents of a single row into a
// row_consumer object:
//
// * First, consume_row_start() is called, with some information about the
//   whole row: The row's key, timestamp, etc.
// * Next, consume_cell() is called once for every column.
// * Finally, consume_row_end() is called. A consumer written for a single
//   column will likely not want to do anything here.
//
// Important note: the row key, column name and column value, passed to the
// consume_* functions, are passed as a "bytes_view" object, which points to
// internal data held by the feeder. This internal data is only valid for the
// duration of the single consume function it was passed to. If the object
// wants to hold these strings longer, it must make a copy of the bytes_view's
// contents. [Note, in reality, because our implementation reads the whole
// row into one buffer, the byte_views remain valid until consume_row_end()
// is called.]
class row_consumer {
    reader_resource_tracker _resource_tracker;
    const io_priority_class& _pc;

public:
    using proceed = data_consumer::proceed;

    row_consumer(reader_resource_tracker resource_tracker, const io_priority_class& pc)
        : _resource_tracker(resource_tracker)
        , _pc(pc) {
    }

    virtual ~row_consumer() = default;

    // Consume the row's key and deletion_time. The latter determines if the
    // row is a tombstone, and if so, when it has been deleted.
    // Note that the key is in serialized form, and should be deserialized
    // (according to the schema) before use.
    // As explained above, the key object is only valid during this call, and
    // if the implementation wishes to save it, it must copy the *contents*.
    virtual proceed consume_row_start(sstables::key_view key, sstables::deletion_time deltime) = 0;

    // Consume one cell (column name and value). Both are serialized, and need
    // to be deserialized according to the schema.
    // When a cell is set with an expiration time, "ttl" is the time to live
    // (in seconds) originally set for this cell, and "expiration" is the
    // absolute time (in seconds since the UNIX epoch) when this cell will
    // expire. Typical cells, not set to expire, will get expiration = 0.
    virtual proceed consume_cell(bytes_view col_name, bytes_view value,
            int64_t timestamp,
            int32_t ttl, int32_t expiration) = 0;

    // Consume one counter cell. Column name and value are serialized, and need
    // to be deserialized according to the schema.
    virtual proceed consume_counter_cell(bytes_view col_name, bytes_view value,
            int64_t timestamp) = 0;

    // Consume a deleted cell (i.e., a cell tombstone).
    virtual proceed consume_deleted_cell(bytes_view col_name, sstables::deletion_time deltime) = 0;

    // Consume one row tombstone.
    virtual proceed consume_shadowable_row_tombstone(bytes_view col_name, sstables::deletion_time deltime) = 0;

    // Consume one range tombstone.
    virtual proceed consume_range_tombstone(
            bytes_view start_col, bytes_view end_col,
            sstables::deletion_time deltime) = 0;

    // Called at the end of the row, after all cells.
    // Returns a flag saying whether the sstable consumer should stop now, or
    // proceed consuming more data.
    virtual proceed consume_row_end() = 0;

    // Called when the reader is fast forwarded to given element.
    virtual void reset(sstables::indexable_element) = 0;

    // Under which priority class to place I/O coming from this consumer
    const io_priority_class& io_priority() const {
        return _pc;
    }

    // The restriction that applies to this consumer
    reader_resource_tracker resource_tracker() const {
        return _resource_tracker;
    }
};

class consumer_m {
    reader_resource_tracker _resource_tracker;
    const io_priority_class& _pc;
public:
    using proceed = data_consumer::proceed;

    consumer_m(reader_resource_tracker resource_tracker, const io_priority_class& pc)
    : _resource_tracker(resource_tracker)
    , _pc(pc) {
    }

    virtual ~consumer_m() = default;

    // Consume the row's key and deletion_time. The latter determines if the
    // row is a tombstone, and if so, when it has been deleted.
    // Note that the key is in serialized form, and should be deserialized
    // (according to the schema) before use.
    // As explained above, the key object is only valid during this call, and
    // if the implementation wishes to save it, it must copy the *contents*.
    virtual proceed consume_partition_start(sstables::key_view key, sstables::deletion_time deltime) = 0;

    // Called at the end of the row, after all cells.
    // Returns a flag saying whether the sstable consumer should stop now, or
    // proceed consuming more data.
    virtual proceed consume_partition_end() = 0;

    virtual proceed consume_row_start(const std::vector<temporary_buffer<char>>& ecp) = 0;

    virtual proceed consume_static_row_start() = 0;

    virtual proceed consume_column(stdx::optional<column_id> column_id,
                                   bytes_view value,
                                   api::timestamp_type timestamp,
                                   gc_clock::duration ttl,
                                   gc_clock::time_point local_deletion_time) = 0;

    virtual proceed consume_row_end(const sstables::liveness_info&) = 0;

    // Called when the reader is fast forwarded to given element.
    virtual void reset(sstables::indexable_element) = 0;

    // Under which priority class to place I/O coming from this consumer
    const io_priority_class& io_priority() const {
        return _pc;
    }

    // The restriction that applies to this consumer
    reader_resource_tracker resource_tracker() const {
        return _resource_tracker;
    }
};

namespace sstables {

// data_consume_rows_context remembers the context that an ongoing
// data_consume_rows() future is in.
class data_consume_rows_context : public data_consumer::continuous_data_consumer<data_consume_rows_context> {
private:
    enum class state {
        ROW_START,
        DELETION_TIME,
        DELETION_TIME_2,
        DELETION_TIME_3,
        ATOM_START,
        ATOM_START_2,
        ATOM_MASK,
        ATOM_MASK_2,
        COUNTER_CELL,
        COUNTER_CELL_2,
        EXPIRING_CELL,
        EXPIRING_CELL_2,
        EXPIRING_CELL_3,
        CELL,
        CELL_2,
        CELL_VALUE_BYTES,
        CELL_VALUE_BYTES_2,
        RANGE_TOMBSTONE,
        RANGE_TOMBSTONE_2,
        RANGE_TOMBSTONE_3,
        RANGE_TOMBSTONE_4,
        STOP_THEN_ATOM_START,
    } _state = state::ROW_START;

    row_consumer& _consumer;

    temporary_buffer<char> _key;
    temporary_buffer<char> _val;

    // state for reading a cell
    bool _deleted;
    bool _counter;
    uint32_t _ttl, _expiration;

    bool _shadowable;
public:
    using consumer = row_consumer;
    bool non_consuming() const {
        return (((_state == state::DELETION_TIME_3)
                || (_state == state::CELL_VALUE_BYTES_2)
                || (_state == state::ATOM_START_2)
                || (_state == state::ATOM_MASK_2)
                || (_state == state::STOP_THEN_ATOM_START)
                || (_state == state::COUNTER_CELL_2)
                || (_state == state::RANGE_TOMBSTONE_4)
                || (_state == state::EXPIRING_CELL_3)) && (_prestate == prestate::NONE));
    }

    // process() feeds the given data into the state machine.
    // The consumer may request at any point (e.g., after reading a whole
    // row) to stop the processing, in which case we trim the buffer to
    // leave only the unprocessed part. The caller must handle calling
    // process() again, and/or refilling the buffer, as needed.
    data_consumer::processing_result process_state(temporary_buffer<char>& data) {
#if 0
        // Testing hack: call process() for tiny chunks separately, to verify
        // that primitive types crossing input buffer are handled correctly.
        constexpr size_t tiny_chunk = 1; // try various tiny sizes
        if (data.size() > tiny_chunk) {
            for (unsigned i = 0; i < data.size(); i += tiny_chunk) {
                auto chunk_size = std::min(tiny_chunk, data.size() - i);
                auto chunk = data.share(i, chunk_size);
                if (process(chunk) == row_consumer::proceed::no) {
                    data.trim_front(i + chunk_size - chunk.size());
                    return row_consumer::proceed::no;
                }
            }
            data.trim(0);
            return row_consumer::proceed::yes;
        }
#endif
        sstlog.trace("data_consume_row_context {}: state={}, size={}", this, static_cast<int>(_state), data.size());
        switch (_state) {
        case state::ROW_START:
            if (read_short_length_bytes(data, _key) != read_status::ready) {
                _state = state::DELETION_TIME;
                break;
            }
        case state::DELETION_TIME:
            if (read_32(data) != read_status::ready) {
                _state = state::DELETION_TIME_2;
                break;
            }
            // fallthrough
        case state::DELETION_TIME_2:
            if (read_64(data) != read_status::ready) {
                _state = state::DELETION_TIME_3;
                break;
            }
            // fallthrough
        case state::DELETION_TIME_3: {
            deletion_time del;
            del.local_deletion_time = _u32;
            del.marked_for_delete_at = _u64;
            auto ret = _consumer.consume_row_start(key_view(to_bytes_view(_key)), del);
            // after calling the consume function, we can release the
            // buffers we held for it.
            _key.release();
            _state = state::ATOM_START;
            if (ret == row_consumer::proceed::no) {
                return row_consumer::proceed::no;
            }
        }
        case state::ATOM_START:
            if (read_short_length_bytes(data, _key) != read_status::ready) {
                _state = state::ATOM_START_2;
                break;
            }
        case state::ATOM_START_2:
            if (_u16 == 0) {
                // end of row marker
                _state = state::ROW_START;
                if (_consumer.consume_row_end() ==
                        row_consumer::proceed::no) {
                    return row_consumer::proceed::no;
                }
            } else {
                _state = state::ATOM_MASK;
            }
            break;
        case state::ATOM_MASK:
            if (read_8(data) != read_status::ready) {
                _state = state::ATOM_MASK_2;
                break;
            }
            // fallthrough
        case state::ATOM_MASK_2: {
            auto const mask = column_mask(_u8);

            if ((mask & (column_mask::range_tombstone | column_mask::shadowable)) != column_mask::none) {
                _state = state::RANGE_TOMBSTONE;
                _shadowable = (mask & column_mask::shadowable) != column_mask::none;
            } else if ((mask & column_mask::counter) != column_mask::none) {
                _deleted = false;
                _counter = true;
                _state = state::COUNTER_CELL;
            } else if ((mask & column_mask::expiration) != column_mask::none) {
                _deleted = false;
                _counter = false;
                _state = state::EXPIRING_CELL;
            } else {
                // FIXME: see ColumnSerializer.java:deserializeColumnBody
                if ((mask & column_mask::counter_update) != column_mask::none) {
                    throw malformed_sstable_exception("FIXME COUNTER_UPDATE_MASK");
                }
                _ttl = _expiration = 0;
                _deleted = (mask & column_mask::deletion) != column_mask::none;
                _counter = false;
                _state = state::CELL;
            }
            break;
        }
        case state::COUNTER_CELL:
            if (read_64(data) != read_status::ready) {
                _state = state::COUNTER_CELL_2;
                break;
            }
            // fallthrough
        case state::COUNTER_CELL_2:
            // _timestamp_of_last_deletion = _u64;
            _state = state::CELL;
            goto state_CELL;
        case state::EXPIRING_CELL:
            if (read_32(data) != read_status::ready) {
                _state = state::EXPIRING_CELL_2;
                break;
            }
            // fallthrough
        case state::EXPIRING_CELL_2:
            _ttl = _u32;
            if (read_32(data) != read_status::ready) {
                _state = state::EXPIRING_CELL_3;
                break;
            }
            // fallthrough
        case state::EXPIRING_CELL_3:
            _expiration = _u32;
            _state = state::CELL;
        state_CELL:
        case state::CELL: {
            if (read_64(data) != read_status::ready) {
                _state = state::CELL_2;
                break;
            }
        }
        case state::CELL_2:
            if (read_32(data) != read_status::ready) {
                _state = state::CELL_VALUE_BYTES;
                break;
            }
        case state::CELL_VALUE_BYTES:
            if (read_bytes(data, _u32, _val) == read_status::ready) {
                // If the whole string is in our buffer, great, we don't
                // need to copy, and can skip the CELL_VALUE_BYTES_2 state.
                //
                // finally pass it to the consumer:
                row_consumer::proceed ret;
                if (_deleted) {
                    if (_val.size() != 4) {
                        throw malformed_sstable_exception("deleted cell expects local_deletion_time value");
                    }
                    deletion_time del;
                    del.local_deletion_time = consume_be<uint32_t>(_val);
                    del.marked_for_delete_at = _u64;
                    ret = _consumer.consume_deleted_cell(to_bytes_view(_key), del);
                } else if (_counter) {
                    ret = _consumer.consume_counter_cell(to_bytes_view(_key),
                            to_bytes_view(_val), _u64);
                } else {
                    ret = _consumer.consume_cell(to_bytes_view(_key),
                            to_bytes_view(_val), _u64, _ttl, _expiration);
                }
                // after calling the consume function, we can release the
                // buffers we held for it.
                _key.release();
                _val.release();
                _state = state::ATOM_START;
                if (ret == row_consumer::proceed::no) {
                    return row_consumer::proceed::no;
                }
            } else {
                _state = state::CELL_VALUE_BYTES_2;
            }
            break;
        case state::CELL_VALUE_BYTES_2:
        {
            row_consumer::proceed ret;
            if (_deleted) {
                if (_val.size() != 4) {
                    throw malformed_sstable_exception("deleted cell expects local_deletion_time value");
                }
                deletion_time del;
                del.local_deletion_time = consume_be<uint32_t>(_val);
                del.marked_for_delete_at = _u64;
                ret = _consumer.consume_deleted_cell(to_bytes_view(_key), del);
            } else if (_counter) {
                ret = _consumer.consume_counter_cell(to_bytes_view(_key),
                        to_bytes_view(_val), _u64);
            } else {
                ret = _consumer.consume_cell(to_bytes_view(_key),
                        to_bytes_view(_val), _u64, _ttl, _expiration);
            }
            // after calling the consume function, we can release the
            // buffers we held for it.
            _key.release();
            _val.release();
            _state = state::ATOM_START;
            if (ret == row_consumer::proceed::no) {
                return row_consumer::proceed::no;
            }
            break;
        }
        case state::RANGE_TOMBSTONE:
            if (read_short_length_bytes(data, _val) != read_status::ready) {
                _state = state::RANGE_TOMBSTONE_2;
                break;
            }
        case state::RANGE_TOMBSTONE_2:
            if (read_32(data) != read_status::ready) {
                _state = state::RANGE_TOMBSTONE_3;
                break;
            }
        case state::RANGE_TOMBSTONE_3:
            if (read_64(data) != read_status::ready) {
                _state = state::RANGE_TOMBSTONE_4;
                break;
            }
        case state::RANGE_TOMBSTONE_4:
        {
            deletion_time del;
            del.local_deletion_time = _u32;
            del.marked_for_delete_at = _u64;
            auto ret = _shadowable
                     ? _consumer.consume_shadowable_row_tombstone(to_bytes_view(_key), del)
                     : _consumer.consume_range_tombstone(to_bytes_view(_key), to_bytes_view(_val), del);
            _key.release();
            _val.release();
            _state = state::ATOM_START;
            if (ret == row_consumer::proceed::no) {
                return row_consumer::proceed::no;
            }
            break;
        }
        case state::STOP_THEN_ATOM_START:
            _state = state::ATOM_START;
            return row_consumer::proceed::no;
        default:
            throw malformed_sstable_exception("unknown state");
        }

        return row_consumer::proceed::yes;
    }

    data_consume_rows_context(const schema&,
                              const shared_sstable&,
                              row_consumer& consumer,
                              input_stream<char>&& input, uint64_t start, uint64_t maxlen)
                : continuous_data_consumer(std::move(input), start, maxlen)
                , _consumer(consumer) {
    }

    void verify_end_state() {
        // If reading a partial row (i.e., when we have a clustering row
        // filter and using a promoted index), we may be in ATOM_START or ATOM_START_2
        // state instead of ROW_START. In that case we did not read the
        // end-of-row marker and consume_row_end() was never called.
        if (_state == state::ATOM_START || _state == state::ATOM_START_2) {
            _consumer.consume_row_end();
            return;
        }
        if (_state != state::ROW_START || _prestate != prestate::NONE) {
            throw malformed_sstable_exception("end of input, but not end of row");
        }
    }

    void reset(indexable_element el) {
        switch (el) {
        case indexable_element::partition:
            _state = state::ROW_START;
            break;
        case indexable_element::cell:
            _state = state::ATOM_START;
            break;
        default:
            assert(0);
        }
        _consumer.reset(el);
    }
};

// data_consume_rows_context_m remembers the context that an ongoing
// data_consume_rows() future is in for SSTable in 3_x format.
class data_consume_rows_context_m : public data_consumer::continuous_data_consumer<data_consume_rows_context_m> {
private:
    enum class state {
        PARTITION_START,
        DELETION_TIME,
        DELETION_TIME_2,
        DELETION_TIME_3,
        FLAGS,
        FLAGS_2,
        EXTENDED_FLAGS,
        CLUSTERING_ROW,
        CK_BLOCK,
        CK_BLOCK_HEADER,
        CK_BLOCK2,
        CK_BLOCK_VALUE_LENGTH,
        CK_BLOCK_VALUE_BYTES,
        CK_BLOCK_END,
        CLUSTERING_ROW_CONSUME,
        ROW_BODY,
        ROW_BODY_SIZE,
        ROW_BODY_PREV_SIZE,
        ROW_BODY_TIMESTAMP,
        ROW_BODY_TIMESTAMP_TTL,
        ROW_BODY_TIMESTAMP_DELTIME,
        ROW_BODY_DELETION,
        ROW_BODY_DELETION_2,
        ROW_BODY_DELETION_3,
        ROW_BODY_MISSING_COLUMNS,
        ROW_BODY_MISSING_COLUMNS_2,
        ROW_BODY_MISSING_COLUMNS_READ_COLUMNS,
        ROW_BODY_MISSING_COLUMNS_READ_COLUMNS_2,
        COLUMN,
        SIMPLE_COLUMN,
        COMPLEX_COLUMN,
        NEXT_COLUMN,
        COLUMN_FLAGS,
        COLUMN_TIMESTAMP,
        COLUMN_DELETION_TIME,
        COLUMN_DELETION_TIME_2,
        COLUMN_TTL,
        COLUMN_TTL_2,
        COLUMN_VALUE,
        COLUMN_VALUE_LENGTH,
        COLUMN_VALUE_BYTES,
        COLUMN_END,
        RANGE_TOMBSTONE_MARKER,
    } _state = state::PARTITION_START;

    consumer_m& _consumer;
    const serialization_header& _header;
    column_translation _column_translation;

    temporary_buffer<char> _pk;

    unfiltered_flags_m _flags{0};
    unfiltered_extended_flags_m _extended_flags{0};
    liveness_info _liveness;
    bool _is_first_unfiltered = true;

    std::vector<temporary_buffer<char>> _row_key;

    boost::iterator_range<std::vector<stdx::optional<column_id>>::const_iterator> _column_ids;
    boost::iterator_range<std::vector<std::optional<uint32_t>>::const_iterator> _column_value_fix_lengths;
    boost::dynamic_bitset<uint64_t> _columns_selector;
    uint64_t _missing_columns_to_read;

    boost::iterator_range<std::vector<std::optional<uint32_t>>::const_iterator> _ck_column_value_fix_lengths;

    column_flags_m _column_flags{0};
    api::timestamp_type _column_timestamp;
    gc_clock::time_point _column_local_deletion_time;
    gc_clock::duration _column_ttl;
    uint32_t _column_value_length;
    temporary_buffer<char> _column_value;
    uint64_t _ck_blocks_header;
    uint32_t _ck_blocks_header_offset;

    void setup_columns(const std::vector<stdx::optional<column_id>>& column_ids,
                       const std::vector<std::optional<uint32_t>>& column_value_fix_lengths) {
        _column_ids = boost::make_iterator_range(column_ids);
        _column_value_fix_lengths = boost::make_iterator_range(column_value_fix_lengths);
    }
    bool is_current_column_present() {
        return _columns_selector.test(_columns_selector.size() - _column_ids.size());
    }
    void skip_absent_columns() {
        size_t pos = _columns_selector.find_first();
        if (pos == boost::dynamic_bitset<uint64_t>::npos) {
            pos = _column_ids.size();
        }
        _column_ids.advance_begin(pos);
        _column_value_fix_lengths.advance_begin(pos);
    }
    bool no_more_columns() { return _column_ids.empty(); }
    void move_to_next_column() {
        size_t current_pos = _columns_selector.size() - _column_ids.size();
        size_t next_pos = _columns_selector.find_next(current_pos);
        size_t jump_to_next = (next_pos == boost::dynamic_bitset<uint64_t>::npos) ? _column_ids.size()
                                                                                  : next_pos - current_pos;
        _column_ids.advance_begin(jump_to_next);
        _column_value_fix_lengths.advance_begin(jump_to_next);
    }
    bool is_column_simple() { return true; }
    stdx::optional<column_id> get_column_id() {
        return _column_ids.front();
    }
    std::optional<uint32_t> get_column_value_length() {
        return _column_value_fix_lengths.front();
    }
    void setup_ck(const std::vector<std::optional<uint32_t>>& column_value_fix_lengths) {
        _row_key.clear();
        _row_key.reserve(column_value_fix_lengths.size());
        _ck_column_value_fix_lengths = boost::make_iterator_range(column_value_fix_lengths);
        _ck_blocks_header_offset = 0u;
    }
    bool no_more_ck_blocks() { return _ck_column_value_fix_lengths.empty(); }
    void move_to_next_ck_block() {
        _ck_column_value_fix_lengths.advance_begin(1);
        ++_ck_blocks_header_offset;
        if (_ck_blocks_header_offset == 32u) {
            _ck_blocks_header_offset = 0u;
        }
    }
    std::optional<uint32_t> get_ck_block_value_length() {
        return _ck_column_value_fix_lengths.front();
    }
    bool is_block_empty() {
        return (_ck_blocks_header & (1u << (2 * _ck_blocks_header_offset))) != 0;
    }
    bool should_read_block_header() {
        return _ck_blocks_header_offset == 0u;
    }
public:
    using consumer = consumer_m;
    bool non_consuming() const {
        return (_state == state::DELETION_TIME_3
                || _state == state::FLAGS_2
                || _state == state::EXTENDED_FLAGS
                || _state == state::CLUSTERING_ROW
                || _state == state::CK_BLOCK_HEADER
                || _state == state::CK_BLOCK_VALUE_LENGTH
                || _state == state::CK_BLOCK_END
                || _state == state::CLUSTERING_ROW_CONSUME
                || _state == state::ROW_BODY_TIMESTAMP_DELTIME
                || _state == state::ROW_BODY_DELETION_3
                || _state == state::ROW_BODY_MISSING_COLUMNS_2
                || _state == state::ROW_BODY_MISSING_COLUMNS_READ_COLUMNS_2
                || _state == state::COLUMN
                || _state == state::NEXT_COLUMN
                || _state == state::COLUMN_TIMESTAMP
                || _state == state::COLUMN_DELETION_TIME_2
                || _state == state::COLUMN_TTL_2
                || _state == state::COLUMN_VALUE_LENGTH
                || _state == state::COLUMN_END) && (_prestate == prestate::NONE);
    }

    data_consumer::processing_result process_state(temporary_buffer<char>& data) {
        switch (_state) {
        case state::PARTITION_START:
        partition_start_label:
            _is_first_unfiltered = true;
            if (read_short_length_bytes(data, _pk) != read_status::ready) {
                _state = state::DELETION_TIME;
                break;
            }
        case state::DELETION_TIME:
            if (read_32(data) != read_status::ready) {
                _state = state::DELETION_TIME_2;
                break;
            }
        case state::DELETION_TIME_2:
            if (read_64(data) != read_status::ready) {
                _state = state::DELETION_TIME_3;
                break;
            }
        case state::DELETION_TIME_3: {
            deletion_time del;
            del.local_deletion_time = _u32;
            del.marked_for_delete_at = _u64;
            auto ret = _consumer.consume_partition_start(key_view(to_bytes_view(_pk)), del);
            // after calling the consume function, we can release the
            // buffers we held for it.
            _pk.release();
            _state = state::FLAGS;
            if (ret == consumer_m::proceed::no) {
                return consumer_m::proceed::no;
            }
        }
        case state::FLAGS:
        flags_label:
            _liveness.reset();
            if (read_8(data) != read_status::ready) {
                _state = state::FLAGS_2;
                break;
            }
        case state::FLAGS_2:
            _flags = unfiltered_flags_m(_u8);
            if (_flags.is_end_of_partition()) {
                _state = state::PARTITION_START;
                if (_consumer.consume_partition_end() == consumer_m::proceed::no) {
                    return consumer_m::proceed::no;
                }
                goto partition_start_label;
            } else if (_flags.is_range_tombstone()) {
                _state = state::RANGE_TOMBSTONE_MARKER;
                goto range_tombstone_marker_label;
            } else if (!_flags.has_extended_flags()) {
                _extended_flags = unfiltered_extended_flags_m(uint8_t{0u});
                _state = state::CLUSTERING_ROW;
                setup_columns(_column_translation.regular_columns(),
                              _column_translation.regular_column_value_fix_legths());
                goto clustering_row_label;
            }
            if (read_8(data) != read_status::ready) {
                _state = state::EXTENDED_FLAGS;
                break;
            }
        case state::EXTENDED_FLAGS:
            _extended_flags = unfiltered_extended_flags_m(_u8);
            if (_extended_flags.is_static()) {
                if (_is_first_unfiltered) {
                    setup_columns(_column_translation.static_columns(),
                                  _column_translation.static_column_value_fix_legths());
                    _is_first_unfiltered = false;
                    _consumer.consume_static_row_start();
                    goto row_body_label;
                } else {
                    throw malformed_sstable_exception("static row should be a first unfiltered in a partition");
                }
            }
            setup_columns(_column_translation.regular_columns(),
                          _column_translation.regular_column_value_fix_legths());
        case state::CLUSTERING_ROW:
        clustering_row_label:
            _is_first_unfiltered = false;
            setup_ck(_column_translation.clustering_column_value_fix_legths());
        case state::CK_BLOCK:
        ck_block_label:
            if (no_more_ck_blocks()) {
                goto clustering_row_consume_label;
            }
            if (!should_read_block_header()) {
                _state = state::CK_BLOCK2;
                goto ck_block2_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::CK_BLOCK_HEADER;
                break;
            }
        case state::CK_BLOCK_HEADER:
            _ck_blocks_header = _u64;
        case state::CK_BLOCK2:
        ck_block2_label:
            if (is_block_empty()) {
                _row_key.push_back({});
                move_to_next_ck_block();
                goto ck_block_label;
            }
            if (auto len = get_ck_block_value_length()) {
                _column_value_length = *len;
                _column_value = temporary_buffer<char>(_column_value_length);
                _state = state::CK_BLOCK_VALUE_BYTES;
                goto ck_block_value_bytes_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::CK_BLOCK_VALUE_LENGTH;
                break;
            }
        case state::CK_BLOCK_VALUE_LENGTH:
            _column_value_length = static_cast<uint32_t>(_u64);
            _column_value = temporary_buffer<char>(_column_value_length);
        case state::CK_BLOCK_VALUE_BYTES:
        ck_block_value_bytes_label:
            if (read_bytes(data, _column_value_length, _column_value) != read_status::ready) {
                _state = state::CK_BLOCK_END;
                break;
            }
        case state::CK_BLOCK_END:
            _row_key.push_back(std::move(_column_value));
            move_to_next_ck_block();
            _state = state::CK_BLOCK;
            goto ck_block_label;
        case state::CLUSTERING_ROW_CONSUME:
        clustering_row_consume_label:
            {
                auto ret = _consumer.consume_row_start(_row_key);
                _row_key.clear();
                _state = state::ROW_BODY;
                if (ret == consumer_m::proceed::no) {
                    return consumer_m::proceed::no;
                }
            }
        case state::ROW_BODY:
        row_body_label:
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::ROW_BODY_SIZE;
                break;
            }
        case state::ROW_BODY_SIZE:
            // Ignore the result
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::ROW_BODY_PREV_SIZE;
                break;
            }
        case state::ROW_BODY_PREV_SIZE:
            // Ignore the result
            if (!_flags.has_timestamp()) {
                _state = state::ROW_BODY_DELETION;
                goto row_body_deletion_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::ROW_BODY_TIMESTAMP;
                break;
            }
        case state::ROW_BODY_TIMESTAMP:
            _liveness.set_timestamp(_u64);
            if (!_flags.has_ttl()) {
                _state = state::ROW_BODY_DELETION;
                goto row_body_deletion_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::ROW_BODY_TIMESTAMP_TTL;
                break;
            }
        case state::ROW_BODY_TIMESTAMP_TTL:
            _liveness.set_ttl(uint32_t(_u64));
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::ROW_BODY_TIMESTAMP_DELTIME;
                break;
            }
        case state::ROW_BODY_TIMESTAMP_DELTIME:
            _liveness.set_local_deletion_time(uint32_t(_u64));
        case state::ROW_BODY_DELETION:
        row_body_deletion_label:
            if (!_flags.has_deletion()) {
                _state = state::ROW_BODY_MISSING_COLUMNS;
                goto row_body_missing_columns_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::ROW_BODY_DELETION_2;
                break;
            }
        case state::ROW_BODY_DELETION_2:
            // TODO consume mark_for_deleted_at
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::ROW_BODY_DELETION_3;
                break;
            }
        case state::ROW_BODY_DELETION_3:
            // TODO consume local_deletion_time
        case state::ROW_BODY_MISSING_COLUMNS:
        row_body_missing_columns_label:
            if (!_flags.has_all_columns()) {
                if (read_unsigned_vint(data) != read_status::ready) {
                    _state = state::ROW_BODY_MISSING_COLUMNS_2;
                    break;
                }
                goto row_body_missing_columns_2_label;
            } else {
                _columns_selector = boost::dynamic_bitset<uint64_t>(_column_ids.size());
                _columns_selector.set();
            }
        case state::COLUMN:
        column_label:
            if (no_more_columns()) {
                _state = state::FLAGS;
                if (_consumer.consume_row_end(_liveness) == consumer_m::proceed::no) {
                    return consumer_m::proceed::no;
                }
                goto flags_label;
            }
            if (!is_column_simple()) {
                _state = state::COMPLEX_COLUMN;
                goto complex_column_label;
            }
        case state::SIMPLE_COLUMN:
            if (read_8(data) != read_status::ready) {
                _state = state::COLUMN_FLAGS;
                break;
            }
        case state::COLUMN_FLAGS:
            _column_flags = column_flags_m(_u8);

            if (_column_flags.use_row_timestamp()) {
                _column_timestamp = _liveness.timestamp();
                _state = state::COLUMN_DELETION_TIME;
                goto column_deletion_time_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::COLUMN_TIMESTAMP;
                break;
            }
        case state::COLUMN_TIMESTAMP:
            _column_timestamp = parse_timestamp(_header, _u64);
        case state::COLUMN_DELETION_TIME:
        column_deletion_time_label:
            if (_column_flags.use_row_ttl()) {
                _column_local_deletion_time = _liveness.local_deletion_time();
                _state = state::COLUMN_TTL;
                goto column_ttl_label;
            } else if (!_column_flags.is_deleted() && ! _column_flags.is_expiring()) {
                _column_local_deletion_time = gc_clock::time_point::max();
                _state = state::COLUMN_TTL;
                goto column_ttl_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::COLUMN_DELETION_TIME_2;
                break;
            }
        case state::COLUMN_DELETION_TIME_2:
            _column_local_deletion_time = parse_expiry(_header, _u64);
        case state::COLUMN_TTL:
        column_ttl_label:
            if (_column_flags.use_row_timestamp()) {
                _column_ttl = _liveness.ttl();
                _state = state::COLUMN_VALUE;
                goto column_value_label;
            } else if (!_column_flags.is_expiring()) {
                _column_ttl = gc_clock::duration::zero();
                _state = state::COLUMN_VALUE;
                goto column_value_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::COLUMN_TTL_2;
                break;
            }
        case state::COLUMN_TTL_2:
            _column_ttl = parse_ttl(_header, _u64);
        case state::COLUMN_VALUE:
        column_value_label:
            if (!_column_flags.has_value()) {
                _column_value = temporary_buffer<char>(0);
                _state = state::COLUMN_END;
                goto column_end_label;
            }
            if (auto len = get_column_value_length()) {
                _column_value_length = *len;
                _column_value = temporary_buffer<char>(_column_value_length);
                _state = state::COLUMN_VALUE_BYTES;
                goto column_value_bytes_label;
            }
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::COLUMN_VALUE_LENGTH;
                break;
            }
        case state::COLUMN_VALUE_LENGTH:
            _column_value_length = static_cast<uint32_t>(_u64);
            _column_value = temporary_buffer<char>(_column_value_length);
        case state::COLUMN_VALUE_BYTES:
        column_value_bytes_label:
            if (read_bytes(data, _column_value_length, _column_value) != read_status::ready) {
                _state = state::COLUMN_END;
                break;
            }
        case state::COLUMN_END:
        column_end_label:
            _state = state::NEXT_COLUMN;
            if (_consumer.consume_column(get_column_id(),
                                         to_bytes_view(_column_value),
                                         _column_timestamp,
                                         _column_ttl,
                                         _column_local_deletion_time) == consumer_m::proceed::no) {
                return consumer_m::proceed::no;
            }
        case state::NEXT_COLUMN:
            move_to_next_column();
            _state = state::COLUMN;
            goto column_label;
        case state::ROW_BODY_MISSING_COLUMNS_2:
        row_body_missing_columns_2_label: {
            uint64_t missing_column_bitmap_or_count = _u64;
            if (_column_ids.size() < 64) {
                _columns_selector.clear();
                _columns_selector.append(missing_column_bitmap_or_count);
                _columns_selector.flip();
                _columns_selector.resize(_column_ids.size());
                skip_absent_columns();
                goto column_label;
            }
            _columns_selector.resize(_column_ids.size());
            if (_column_ids.size() - missing_column_bitmap_or_count < _column_ids.size() / 2) {
                _missing_columns_to_read = _column_ids.size() - missing_column_bitmap_or_count;
                _columns_selector.reset();
            } else {
                _missing_columns_to_read = missing_column_bitmap_or_count;
                _columns_selector.set();
            }
            goto row_body_missing_columns_read_columns_label;
        }
        case state::ROW_BODY_MISSING_COLUMNS_READ_COLUMNS:
        row_body_missing_columns_read_columns_label:
            if (_missing_columns_to_read == 0) {
                skip_absent_columns();
                goto column_label;
            }
            --_missing_columns_to_read;
            if (read_unsigned_vint(data) != read_status::ready) {
                _state = state::ROW_BODY_MISSING_COLUMNS_READ_COLUMNS_2;
                break;
            }
        case state::ROW_BODY_MISSING_COLUMNS_READ_COLUMNS_2:
            _columns_selector.flip(_u64);
            goto row_body_missing_columns_read_columns_label;
        case state::COMPLEX_COLUMN:
        complex_column_label:
            throw malformed_sstable_exception("unimplemented state: complex columns not supported");
        case state::RANGE_TOMBSTONE_MARKER:
        range_tombstone_marker_label:
            throw malformed_sstable_exception("unimplemented state");
        default:
            throw malformed_sstable_exception("unknown state");
        }

        return row_consumer::proceed::yes;
    }

    data_consume_rows_context_m(const schema& s,
                                const shared_sstable& sst,
                                consumer_m& consumer,
                                input_stream<char> && input,
                                uint64_t start,
                                uint64_t maxlen)
        : continuous_data_consumer(std::move(input), start, maxlen)
        , _consumer(consumer)
        , _header(sst->get_serialization_header())
        , _column_translation(sst->get_column_translation(s, _header))
        , _liveness(_header)
    { }

    void verify_end_state() {
        if (_state != state::PARTITION_START || _prestate != prestate::NONE) {
            throw malformed_sstable_exception("end of input, but not end of partition");
        }
    }

    void reset(indexable_element el) {
        switch (el) {
            case indexable_element::partition:
                _state = state::PARTITION_START;
                break;
            default:
                assert(0);
        }
        _consumer.reset(el);
    }
};

}
//
