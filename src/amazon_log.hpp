#pragma once

#include <cstdint>
#include <optional>

// Best-effort lookup of the last "playbackTerminatedEvent" in Amazon Music's
// own log. Amazon's media session does not expose a timeline, so the moment the
// previous track ended is the best available anchor for the current position.
std::optional<int64_t> lastAmazonPlaybackTerminationUnixMs();
