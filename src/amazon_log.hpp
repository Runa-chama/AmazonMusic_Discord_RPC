#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Best-effort lookup of the last "playbackTerminatedEvent" in Amazon Music's
// own log. Amazon's media session does not expose a timeline, so the moment the
// previous track ended is the best available anchor for the current position.
std::optional<int64_t> lastAmazonPlaybackTerminationUnixMs();

// The catalog ASINs for the most recent track start. Amazon's substituted ASIN
// (what is actually playing) comes first; the originally requested ASIN follows.
std::vector<std::string> lastAmazonTrackAsins();
