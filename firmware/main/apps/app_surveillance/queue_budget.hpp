#pragma once

#include <cstddef>

namespace surveillance {

inline constexpr std::size_t kMaxPendingImages = 12;
inline constexpr std::size_t kMaxPendingBytes = 1024 * 1024;

constexpr bool queue_needs_oldest_drop(std::size_t pending_count,
                                       std::size_t pending_bytes,
                                       std::size_t incoming_bytes)
{
    return pending_count >= kMaxPendingImages || incoming_bytes > kMaxPendingBytes ||
           pending_bytes > kMaxPendingBytes - incoming_bytes;
}

}  // namespace surveillance
