#pragma once

// Loading tip text that does not touch the game, kept apart so it can be tested: the times of [graphics]
// loader_defenses and the defense tip with its countdown. The tip is German, for the NGO build.

#include "config.h"
#include "text.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loading_tips
{
// A defense stays in the tip, as running, for this long after it starts.
constexpr int64_t kDefenseRunningSeconds = 60 * 60;

namespace detail
{
// A whole number of 1 to `max_digits` digits, without a sign, at the start of `text`, which it then skips.
inline std::optional<int32_t> TakeNumber(std::string_view& text, size_t max_digits)
{
  if (text.empty() || text.front() < '0' || text.front() > '9')
    return std::nullopt;
  int32_t    value        = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  const auto digits       = static_cast<size_t>(end - text.data());
  if (error != std::errc{} || digits > max_digits)
    return std::nullopt;
  text.remove_prefix(digits);
  return value;
}

inline bool TakeChar(std::string_view& text, char c)
{
  if (text.empty() || text.front() != c)
    return false;
  text.remove_prefix(1);
  return true;
}

inline std::tm LocalTm(std::time_t time)
{
  std::tm local{};
#if _WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  return local;
}

inline std::time_t LocalTime(int32_t year, const LoaderDefense& defense)
{
  std::tm local{};
  local.tm_year  = year - 1900;
  local.tm_mon   = defense.month - 1;
  local.tm_mday  = defense.day;
  local.tm_hour  = defense.hour;
  local.tm_min   = defense.minute;
  local.tm_isdst = -1;
  return std::mktime(&local);
}

inline std::string TwoDigits(int32_t value)
{ return value < 10 ? "0" + std::to_string(value) : std::to_string(value); }

inline std::string Name(const LoaderDefense& defense)
{ return defense.level > 0 ? defense.system + " (Stufe " + std::to_string(defense.level) + ")" : defense.system; }

inline std::string Clock(const std::tm& local)
{ return TwoDigits(local.tm_hour) + ":" + TwoDigits(local.tm_min) + " Uhr"; }

inline std::string Day(const std::tm& local)
{
  static constexpr std::string_view kWeekdays[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
  return std::string(kWeekdays[local.tm_wday % 7]) + " " + TwoDigits(local.tm_mday) + "."
         + TwoDigits(local.tm_mon + 1) + ".";
}
} // namespace detail

// "27.11. 13:00", "27.11.2026 13:00" or "2026-11-27 13:00" into the date fields of `defense`; year 0 when the text
// has none. False, leaving `defense` alone, when the text is not one of those or names an impossible time.
inline bool ParseDefenseTime(std::string_view text, LoaderDefense& defense)
{
  using detail::TakeChar;
  using detail::TakeNumber;
  text = StripAsciiWhitespace(text);
  const auto space = text.find(' ');
  if (space == std::string_view::npos)
    return false;
  auto date  = text.substr(0, space);
  auto clock = StripAsciiWhitespace(text.substr(space + 1));

  std::optional<int32_t> year, month, day;
  if (date.find('-') != std::string_view::npos) {
    year = TakeNumber(date, 4);
    if (!year || !TakeChar(date, '-') || !(month = TakeNumber(date, 2)) || !TakeChar(date, '-')
        || !(day = TakeNumber(date, 2)) || !date.empty())
      return false;
  } else {
    if (!(day = TakeNumber(date, 2)) || !TakeChar(date, '.') || !(month = TakeNumber(date, 2)))
      return false;
    TakeChar(date, '.');
    if (!date.empty() && !(year = TakeNumber(date, 4)))
      return false;
    if (!date.empty())
      return false;
  }
  const auto hour = TakeNumber(clock, 2);
  if (!hour || !TakeChar(clock, ':'))
    return false;
  const auto minute = TakeNumber(clock, 2);
  if (!minute || !clock.empty())
    return false;

  if ((year && *year < 2000) || *month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59)
    return false;
  defense.year   = year.value_or(0);
  defense.month  = *month;
  defense.day    = *day;
  defense.hour   = *hour;
  defense.minute = *minute;
  return true;
}

// When the defense starts. Without a year, the one that puts it closest to `now`, so "02.01." written in late
// December means the coming January.
inline std::time_t DefenseStart(const LoaderDefense& defense, std::time_t now)
{
  if (defense.year > 0)
    return detail::LocalTime(defense.year, defense);
  const int32_t year = detail::LocalTm(now).tm_year + 1900;
  std::time_t   best = detail::LocalTime(year, defense);
  for (const int32_t candidate : {year - 1, year + 1}) {
    const auto start = detail::LocalTime(candidate, defense);
    if (std::llabs(static_cast<long long>(start - now)) < std::llabs(static_cast<long long>(best - now)))
      best = start;
  }
  return best;
}

// "in 2 T 4 Std", "in 3 Std 20 Min" or "in 12 Min"; never less than a minute.
inline std::string Countdown(int64_t seconds)
{
  const int64_t minutes = std::max<int64_t>(1, (seconds + 59) / 60);
  const int64_t days    = minutes / (24 * 60);
  const int64_t hours   = minutes / 60 % 24;
  if (days > 0)
    return "in " + std::to_string(days) + " T " + std::to_string(hours) + " Std";
  if (hours > 0)
    return "in " + std::to_string(hours) + " Std " + std::to_string(minutes % 60) + " Min";
  return "in " + std::to_string(minutes) + " Min";
}

// The tip for the next defense that has not ended, and the one after it, or nothing when none is left.
inline std::optional<std::string> DefenseTip(const std::vector<LoaderDefense>& defenses, std::time_t now)
{
  struct Upcoming {
    std::time_t          start;
    const LoaderDefense* defense;
  };
  std::vector<Upcoming> upcoming;
  for (const auto& defense : defenses)
    if (const auto start = DefenseStart(defense, now); start + kDefenseRunningSeconds > now)
      upcoming.push_back({start, &defense});
  if (upcoming.empty())
    return std::nullopt;
  std::ranges::stable_sort(upcoming, {}, &Upcoming::start);

  const auto& next  = upcoming.front();
  const auto  local = detail::LocalTm(next.start);
  std::string tip   = "Gebietsverteidigung: " + detail::Name(*next.defense);
  if (next.start <= now)
    tip += " läuft gerade (seit " + detail::Clock(local) + ").";
  else
    tip += " am " + detail::Day(local) + " um " + detail::Clock(local) + ", "
           + Countdown(static_cast<int64_t>(next.start - now)) + ".";
  if (upcoming.size() > 1) {
    const auto& after = upcoming[1];
    const auto  then  = detail::LocalTm(after.start);
    tip += " Danach: " + detail::Name(*after.defense) + " am " + detail::Day(then) + " um " + detail::Clock(then) + ".";
  }
  return tip;
}
} // namespace loading_tips
