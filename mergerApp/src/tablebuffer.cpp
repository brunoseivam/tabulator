#include "tablebuffer.h"

#include <tab/util.h>

namespace tabulator {

namespace ts = util::ts;

// TODO: move timespan to ts
const epicsUInt32 TimeSpan::MAX_U32 = std::numeric_limits<epicsUInt32>::max();
const epicsTimeStamp TimeSpan::MAX_TS = { MAX_U32, 999999999 };
const epicsTimeStamp TimeSpan::MIN_TS = {0, 0};

TimeSpan::TimeSpan() {
    reset();
}

TimeSpan::TimeSpan(const epicsTimeStamp & start, const epicsTimeStamp & end)
: valid(true), start(start), end(end)
{
    //printf("start = %u.%u   end=%u.%u\n", start.secPastEpoch, start.nsec, end.secPastEpoch, end.nsec);
    assert(epicsTimeGreaterThanEqual(&end, &start));
}

void TimeSpan::update(const epicsTimeStamp & start, const epicsTimeStamp & end) {
    valid = true;
    this->start = ts::min(this->start, start);
    this->end = ts::max(this->end, end);
}

void TimeSpan::reset() {
    valid = false;
    start = MAX_TS;
    end = MIN_TS;
}

double TimeSpan::span_sec() const {
    assert(valid);
    return epicsTimeDiffInSeconds(&end, &start);
}

void TableBuffer::update_timestamps() {
    if (empty())
        return;

    const auto & first = buffer_.front();

    const auto start_seconds_past_epoch =
        first.get_column_as<const TimeTable::SECONDS_PAST_EPOCH_T>(TimeTable::SECONDS_PAST_EPOCH_COL);

    const auto start_nanoseconds =
        first.get_column_as<const TimeTable::NANOSECONDS_T>(TimeTable::NANOSECONDS_COL);

    start_ts_ = { start_seconds_past_epoch[inner_idx_], start_nanoseconds[inner_idx_] };

    const auto & last = buffer_.back();

    const auto end_seconds_past_epoch =
        last.get_column_as<const TimeTable::SECONDS_PAST_EPOCH_T>(TimeTable::SECONDS_PAST_EPOCH_COL);

    const auto & end_nanoseconds =
        last.get_column_as<const TimeTable::NANOSECONDS_T>(TimeTable::NANOSECONDS_COL);

    size_t n = end_seconds_past_epoch.size();
    end_ts_ = { end_seconds_past_epoch[n-1], end_nanoseconds[n-1] };
}

TableBuffer::TableBuffer()
: type_(), start_ts_(), end_ts_(), buffer_(), inner_idx_(0u)
{}

bool TableBuffer::initialized() const {
    // We are initialized when we have types for every column
    return type_.get() != 0;
}

bool TableBuffer::empty() const {
    return buffer_.empty();
}

const std::vector<nt::NTTable::ColumnSpec> & TableBuffer::columns() const {
    static const std::vector<nt::NTTable::ColumnSpec> EMPTY;
    return type_ ? type_->columns : EMPTY;
}

const std::vector<nt::NTTable::ColumnSpec> & TableBuffer::data_columns() const {
    static const std::vector<nt::NTTable::ColumnSpec> EMPTY;
    return type_ ? type_->data_columns : EMPTY;
}

TimeSpan TableBuffer::time_span() const {
    static const TimeSpan EMPTY;
    if (empty())
        return EMPTY;

    return TimeSpan(start_ts_, end_ts_);
}

std::vector<pvxs::shared_array<void>> TableBuffer::allocate_containers(size_t num_rows) const {
    std::vector<pvxs::shared_array<void>> result;

    for (auto & c : type_->data_columns)
        result.emplace_back(pvxs::allocArray(c.type_code.arrayType(), num_rows));

    return result;
}

void TableBuffer::push(pvxs::Value value) {
    if (!type_)
        type_.reset(new TimeTable(value));

    buffer_.emplace_back(type_->wrap(value, true));

    update_timestamps();
}

std::pair<size_t, size_t> TableBuffer::consume_each_row_inner(ConsumeFunc f) {
    size_t outer_idx = 0;
    size_t inner_idx;

    for (auto v : buffer_) {
        auto seconds_past_epoch =
            v.get_column_as<const TimeTable::SECONDS_PAST_EPOCH_T>(TimeTable::SECONDS_PAST_EPOCH_COL);

        auto nanoseconds =
            v.get_column_as<const TimeTable::NANOSECONDS_T>(TimeTable::NANOSECONDS_COL);

        std::vector<pvxs::shared_array<const void>> col_vals;
        for (auto & col : type_->data_columns) {
            auto col_val = v.get_column_as<const void>(col.name);
            col_vals.push_back(col_val);
        }

        // Iterate over rows in this Value
        size_t n = seconds_past_epoch.size();
        inner_idx = outer_idx == 0 ? inner_idx_ : 0;
        for (;inner_idx < n; ++inner_idx) {

            epicsTimeStamp ts {
                seconds_past_epoch[inner_idx],
                nanoseconds[inner_idx]
            };

            bool done = f(ts, col_vals, inner_idx);

            if (done)
                return std::make_pair(outer_idx, inner_idx);
        }

        ++outer_idx;
    }

    return std::make_pair(outer_idx, 0);

}

void TableBuffer::consume_each_row(ConsumeFunc f) {
    size_t outer_idx, inner_idx;
    std::tie(outer_idx, inner_idx) = consume_each_row_inner(f);

    for (size_t idx = 0; idx < outer_idx; ++idx)
        buffer_.pop_front();

    inner_idx_ = inner_idx;
    update_timestamps();
}

} // namespace tabulator