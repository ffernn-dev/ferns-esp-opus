// Copyright 2026 ffernn-dev
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "micro_opus/ogg_seek.h"

#include <micro_ogg/ogg_demuxer.h>

#include <cstring>

namespace micro_opus {

namespace {

struct PageInfo {
    uint64_t offset;
    uint64_t next_offset;
    int64_t granule;  // -1 if no packet completes on this page
    uint32_t serial;
    uint8_t flags;
};

constexpr size_t MIN_PAGE_HEADER = 27;
constexpr size_t MAX_PAGE_HEADER = 282;  // 27 + 255 lacing values
constexpr uint64_t MAX_PAGE_SIZE = 65307;
constexpr uint64_t BISECT_STOP_BYTES = 64 * 1024;
constexpr int64_t PRE_ROLL_SAMPLES = 3840;  // 80 ms, as per RFC 7845 section 4.6
constexpr int64_t SAMPLES_PER_MS = 48;

bool next_page(const OggSeekSource& src, uint64_t from, PageInfo& out) {
    uint8_t buf[MAX_PAGE_HEADER];
    while (from < src.file_size) {
        size_t got = src.read_at(src.ctx, from, buf, sizeof(buf));
        if (got < MIN_PAGE_HEADER) {
            return false;
        }

        size_t i = 0;
        while (i + 4 <= got && memcmp(buf + i, "OggS", 4) != 0) {
            i++;
        }
        if (i + 4 > got) {
            from += got - 3;  // "OggS" may straddle the chunk
            continue;
        }
        if (i > 0) {
            from += i;  // re-read so the whole header is in buf
            continue;
        }

        micro_ogg::OggPageHeader header;
        size_t header_size = 0;
        if (micro_ogg::OggDemuxer::parse_page_header(buf, got, header, header_size) ==
            micro_ogg::OGG_OK) {
            out.offset = from;
            out.next_offset = from + header_size +
                              micro_ogg::OggDemuxer::calculate_body_size(buf + MIN_PAGE_HEADER,
                                                                         header.segment_count);
            out.granule = header.granule_position;
            out.serial = header.stream_serial;
            out.flags = header.header_type;
            return true;
        }
        from += 1;  // false match inside audio data
    }
    return false;
}

bool read_first_page(const OggSeekSource& src, PageInfo& first) {
    return next_page(src, 0, first) && (first.flags & micro_ogg::OGG_BEGINNING_OF_STREAM);
}

// Finds the first page in [from, limit) on this stream that has a granule position.
bool next_timed_page(const OggSeekSource& src, uint64_t from, uint64_t limit, uint32_t serial,
                     PageInfo& out) {
    while (from < limit && next_page(src, from, out) && out.offset < limit) {
        if (out.serial != serial) {
            from = out.offset + 1;
        } else if (out.granule != -1) {
            return true;
        } else {
            from = out.next_offset;
        }
    }
    return false;
}

}  // namespace

OggSeekResult ogg_get_duration_ms(const OggSeekSource& src, uint16_t pre_skip,
                                  uint32_t& duration_ms) {
    PageInfo first;
    if (!read_first_page(src, first)) {
        return OGG_SEEK_INVALID;
    }

    uint64_t from = src.file_size > MAX_PAGE_SIZE ? src.file_size - MAX_PAGE_SIZE : 0;
    int64_t last_granule = -1;
    PageInfo page;
    while (next_page(src, from, page)) {
        if (page.offset > 0 && (page.flags & micro_ogg::OGG_BEGINNING_OF_STREAM)) {
            return OGG_SEEK_UNSEEKABLE;
        }
        if (page.serial != first.serial) {
            from = page.offset + 1;  // probably a false match, so don't skip by its length
            continue;
        }
        if (page.granule != -1) {
            last_granule = page.granule;
        }
        from = page.next_offset;
    }

    // The window always holds the start of the file's last page, so nothing from our stream
    // means the file ends in a different one.
    if (last_granule < 0) {
        return OGG_SEEK_UNSEEKABLE;
    }
    if (last_granule < pre_skip) {
        return OGG_SEEK_INVALID;
    }

    duration_ms = static_cast<uint32_t>((last_granule - pre_skip) / SAMPLES_PER_MS);
    return OGG_SEEK_OK;
}

OggSeekResult ogg_find_seek_point(const OggSeekSource& src, uint32_t target_ms, uint16_t pre_skip,
                                  OggSeekPoint& point) {
    PageInfo first;
    if (!read_first_page(src, first)) {
        return OGG_SEEK_INVALID;
    }

    const int64_t target = static_cast<int64_t>(target_ms) * SAMPLES_PER_MS + pre_skip;
    const int64_t search = target - PRE_ROLL_SAMPLES;

    uint64_t lo = 0;
    uint64_t hi = src.file_size;
    PageInfo page;
    while (hi - lo > BISECT_STOP_BYTES) {
        const uint64_t mid = lo + (hi - lo) / 2;
        if (next_timed_page(src, mid, hi, first.serial, page) && page.granule <= search) {
            lo = page.offset;
        } else {
            hi = mid;
        }
    }

    // A page's first sample is the previous page's granule position, unless it opens with the
    // tail of a packet from the page before. The demuxer can't start on one of those.
    int64_t prev_granule = 0;
    bool found = false;
    uint64_t from = lo;
    while (prev_granule <= search && next_page(src, from, page)) {
        if (page.serial != first.serial) {
            from = page.offset + 1;
            continue;
        }
        if (!(page.flags & micro_ogg::OGG_CONTINUED_PACKET) && prev_granule > 0) {
            point.offset = page.offset;
            point.start_granule = prev_granule;
            found = true;
        }
        if (page.granule != -1) {
            prev_granule = page.granule;
        }
        from = page.next_offset;
    }

    if (!found) {
        return OGG_SEEK_RESTART;
    }
    point.discard = static_cast<uint32_t>(target - point.start_granule);
    return OGG_SEEK_OK;
}

}  // namespace micro_opus
