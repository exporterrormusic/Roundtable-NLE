/*
 * ClipSchemas.h — a clip as a v48 record: its type, the fields every clip
 * has, and one sub-record with its type's own fields. See Schema.h for the
 * rules; field ids are append-only.
 */
#pragma once

#include "project/schema/Schema.h"

#include <memory>

namespace rt {
class Clip;
}

namespace rt::record {

void writeClip(RecordWriter& w, const Clip& clip);

/// The clip a record describes, with its saved id (reserved so new clips
/// don't reuse it). nullptr when its type is unknown to this build (the
/// clip is then left out); a malformed record fails `r`.
[[nodiscard]] std::unique_ptr<Clip> readClip(RecordReader& r);

/// A clip's full saved state as bytes: what undo and the snapshot publisher
/// compare to tell whether a clip changed.
[[nodiscard]] std::vector<uint8_t> clipBytes(const Clip& clip);

} // namespace rt::record
