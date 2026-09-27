#pragma once

// M4 E: numbered image files as one clip ("Import Image Sequence…", an
// explicit command: camera photos are numbered too). The picked file's
// last run of digits is the frame number; its siblings with the same
// prefix, suffix and digit count, numbered on from it without a gap, are
// the sequence. Portable (std::filesystem), no MLT.

#include <optional>
#include <string>

namespace ustudio::core {

struct ImageSequence
{
    std::string pattern;     // the directory plus "frame_%04d.png": what MLT's pixbuf producer opens
    int begin = 0;           // the first file's number
    int count = 0;           // files in the gapless run
    std::string displayName; // "frame_[0001-0030].png"
};

// The gapless run of numbered files containing `pickedFile`, or nullopt if
// its name has no number or it has no numbered neighbour.
std::optional<ImageSequence> findImageSequence(const std::string &pickedFile);

// One file of a sequence: `pattern` with `number` in it.
std::string imageSequenceFile(const std::string &pattern, int number);

} // namespace ustudio::core
