#pragma once

// The game window saved by [graphics] remember_window, without touching Windows so it can be tested: its rectangle on
// screen, or for a maximized window the rectangle it restores to, and whether it was maximized, as one line of text.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace window_placement
{
// Smaller saved windows are ignored, so a broken file never leaves the game a sliver.
constexpr int32_t kMinimumWidth  = 320;
constexpr int32_t kMinimumHeight = 240;

struct Placement {
  int32_t left      = 0;
  int32_t top       = 0;
  int32_t right     = 0;
  int32_t bottom    = 0;
  bool    maximized = false;

  bool operator==(const Placement&) const = default;
};

// "left top right bottom maximized", such as "100 80 1700 1000 0".
inline std::string Format(const Placement& placement)
{
  return std::to_string(placement.left) + " " + std::to_string(placement.top) + " " + std::to_string(placement.right)
         + " " + std::to_string(placement.bottom) + " " + (placement.maximized ? "1" : "0");
}

// The placement Format wrote, or nothing when `text` is malformed or the window would be too small.
inline std::optional<Placement> Parse(std::string_view text)
{
  int32_t values[5] = {};
  for (auto& value : values) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
      text.remove_prefix(1);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{})
      return std::nullopt;
    text.remove_prefix(static_cast<size_t>(end - text.data()));
  }
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
    text.remove_prefix(1);
  if (!text.empty() || (values[4] != 0 && values[4] != 1))
    return std::nullopt;
  const Placement placement{values[0], values[1], values[2], values[3], values[4] == 1};
  if (int64_t{placement.right} - placement.left < kMinimumWidth
      || int64_t{placement.bottom} - placement.top < kMinimumHeight)
    return std::nullopt;
  return placement;
}
} // namespace window_placement
