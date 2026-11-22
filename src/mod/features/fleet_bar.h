#pragma once

// The fleet list of [ui] fleet_bar, without touching the game so it can be tested: each ship's select key, name,
// status, remaining time and cargo as one line of TextMeshPro rich text, and the changes of state that
// [ui] fleet_notifications reports. The text is German, like the territory defense panel.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fleet_bar
{
enum class Status {
  kUnknown,
  kIdle,
  kDocked,
  kMining,
  kDestroyed,
  kRepairing,
  kBattling,
  kWarpCharging,
  kWarping,
  kImpulsing,
  kCapturing,
  kAutoHunting,
  kTieringUp,
  kOutposting,
};

struct Row {
  // The select-ship key, 1 to 8.
  int32_t                key    = 0;
  std::string            name;
  Status                 status = Status::kUnknown;
  // Seconds until the repair, warp or mining timer runs out.
  std::optional<int64_t> remaining;
  // How full the cargo hold is while mining.
  std::optional<int32_t> cargo_percent;
  // The game's id of the fleet, which stays with the ship's slot across sessions.
  int64_t                fleet_id = 0;
};

// The changes of state [ui] fleet_notifications can report.
enum class Event {
  kRepaired,
  kArrived,
  kCargoFull,
  kMiningDone,
  kDestroyed,
};

// Longer ship names are cut, so the columns stay in place.
constexpr std::size_t kNameLength = 14;

inline std::string_view StatusText(Status status)
{
  switch (status) {
  case Status::kIdle:
    return "Leerlauf";
  case Status::kDocked:
    return "Angedockt";
  case Status::kMining:
    return "Mining";
  case Status::kDestroyed:
    return "Zerstört";
  case Status::kRepairing:
    return "Reparatur";
  case Status::kBattling:
    return "Kampf";
  case Status::kWarpCharging:
    return "Warp lädt";
  case Status::kWarping:
    return "Warp";
  case Status::kImpulsing:
    return "Impuls";
  case Status::kCapturing:
    return "Eroberung";
  case Status::kAutoHunting:
    return "Jagd";
  case Status::kTieringUp:
    return "Aufstufung";
  case Status::kOutposting:
    return "Außenposten";
  case Status::kUnknown:
    break;
  }
  return "?";
}

// A muted palette that stays readable on the dark panel: idle ships stand out in amber.
inline std::string_view StatusColor(Status status)
{
  switch (status) {
  case Status::kIdle:
    return "#F0C26B";
  case Status::kDocked:
    return "#8A9BB0";
  case Status::kMining:
    return "#5FD4C4";
  case Status::kRepairing:
  case Status::kTieringUp:
    return "#F29C50";
  case Status::kWarpCharging:
  case Status::kWarping:
  case Status::kImpulsing:
    return "#7AA7FF";
  case Status::kDestroyed:
  case Status::kBattling:
  case Status::kCapturing:
    return "#FF7A7A";
  case Status::kAutoHunting:
  case Status::kOutposting:
  case Status::kUnknown:
    break;
  }
  return "#C7D4E4";
}

namespace detail
{
  inline std::string TwoDigits(int64_t value)
  { return (value < 10 ? "0" : "") + std::to_string(value); }
} // namespace detail

// "0:48", "4:12", "1:37:05".
inline std::string Timer(int64_t seconds)
{
  seconds = std::max<int64_t>(seconds, 0);
  const auto hours   = seconds / 3600;
  const auto minutes = seconds / 60 % 60;
  const auto rest    = detail::TwoDigits(seconds % 60);
  if (hours > 0)
    return std::to_string(hours) + ":" + detail::TwoDigits(minutes) + ":" + rest;
  return std::to_string(minutes) + ":" + rest;
}

// At most `length` characters, counted as UTF-8 code points; a cut name ends in a dot.
inline std::string ShortName(std::string_view name, std::size_t length)
{
  if (length == 0)
    return {};
  std::size_t count = 0;
  for (std::size_t i = 0; i < name.size(); ++i) {
    if ((static_cast<unsigned char>(name[i]) & 0xC0) == 0x80)
      continue;
    if (count == length - 1) {
      // Fits when nothing but this last character is left.
      std::size_t end = i + 1;
      while (end < name.size() && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80)
        ++end;
      if (end == name.size())
        return std::string(name);
      return std::string(name.substr(0, i)) + ".";
    }
    ++count;
  }
  return std::string(name);
}

// One ship. The full line has columns for name, status and time; the compact one only the key and the time, or the
// status when no timer runs. Names go in <noparse>, so a "<" in one is not read as a tag.
inline std::string Line(const Row& row, bool compact)
{
  const std::string color = std::string(StatusColor(row.status));
  const std::string key   = "<color=#8EA3BD>" + std::to_string(row.key) + "</color> ";
  if (compact) {
    const std::string value = row.remaining ? Timer(*row.remaining) : std::string(StatusText(row.status));
    return key + "<color=" + color + ">" + value + "</color>";
  }

  std::string status(StatusText(row.status));
  if (row.cargo_percent)
    status += " " + std::to_string(*row.cargo_percent) + "%";
  const std::string name_color = row.status == Status::kIdle ? color : std::string("#E4ECF5");
  std::string       line       = key + "<color=" + name_color + "><noparse>" + ShortName(row.name, kNameLength)
                     + "</noparse></color><pos=47%><color=" + color + ">" + status + "</color>";
  if (row.remaining)
    line += "<pos=77%><color=" + color + ">" + Timer(*row.remaining) + "</color>";
  return line;
}

inline bool Travelling(Status status)
{ return status == Status::kWarpCharging || status == Status::kWarping || status == Status::kImpulsing; }

// What a ship's change from `before` to `after` reports, if anything. A fight that interrupts a repair, a journey
// or mining reports nothing until it ends.
inline std::optional<Event> Change(const Row& before, const Row& after)
{
  if (before.status == Status::kUnknown || after.status == Status::kUnknown)
    return std::nullopt;
  if (before.status == after.status) {
    const bool full     = after.cargo_percent && *after.cargo_percent >= 100;
    const bool was_full = before.cargo_percent && *before.cargo_percent >= 100;
    if (after.status == Status::kMining && full && !was_full)
      return Event::kCargoFull;
    return std::nullopt;
  }
  if (after.status == Status::kDestroyed)
    return Event::kDestroyed;
  if (after.status == Status::kBattling)
    return std::nullopt;
  if (before.status == Status::kRepairing)
    return Event::kRepaired;
  if (Travelling(before.status) && !Travelling(after.status))
    return Event::kArrived;
  if (before.status == Status::kMining)
    return Event::kMiningDone;
  return std::nullopt;
}

inline std::string_view EventText(Event event)
{
  switch (event) {
  case Event::kRepaired:
    return "Reparatur fertig";
  case Event::kArrived:
    return "angekommen";
  case Event::kCargoFull:
    return "Frachtraum voll";
  case Event::kMiningDone:
    return "Mining beendet";
  case Event::kDestroyed:
    return "zerstört";
  }
  return "";
}

// The notification's title: the select key and the ship's whole name.
inline std::string NotificationTitle(const Row& row)
{ return std::to_string(row.key) + " " + row.name; }

inline std::string Text(std::span<const Row> rows, bool compact)
{
  std::string text;
  for (const auto& row : rows) {
    if (!text.empty())
      text += '\n';
    text += Line(row, compact);
  }
  return text;
}
} // namespace fleet_bar
