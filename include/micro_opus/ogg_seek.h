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

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace micro_opus {

// Read up to len bytes at an absolute file offset. Returns bytes read (0 at EOF or on error).
struct OggSeekSource {
    size_t (*read_at)(void* ctx, uint64_t offset, uint8_t* buf, size_t len);
    void* ctx;
    uint64_t file_size;
};

struct OggSeekPoint {
    uint64_t offset;        // fseek here, then feed decode()
    int64_t start_granule;  // pass to prepare_seek()
    uint32_t discard;       // samples per channel to drop after resuming (at 48 kHz)
};

enum OggSeekResult : int8_t {
    OGG_SEEK_OK = 0,
    OGG_SEEK_RESTART = 1,      // target is too near the start: reset() and decode from byte 0
    OGG_SEEK_INVALID = -1,     // no valid Ogg pages found
    OGG_SEEK_UNSEEKABLE = -2,  // chained file: the file doesn't end in the stream it starts with
};

// pre_skip comes from OggOpusDecoder::get_pre_skip(). Both assume the stream starts at position 0.
OggSeekResult ogg_get_duration_ms(const OggSeekSource& src, uint16_t pre_skip,
                                  uint32_t& duration_ms);

// Clamp target_ms to the duration first: past the end, discard exceeds what's left to decode.
OggSeekResult ogg_find_seek_point(const OggSeekSource& src, uint32_t target_ms, uint16_t pre_skip,
                                  OggSeekPoint& point);

}  // namespace micro_opus
