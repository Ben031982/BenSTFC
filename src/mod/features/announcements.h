#pragma once

// [ui] announcements, without touching the game so it can be tested: the alliance leadership's "!NGO" chat commands,
// the announcement they set, how it is kept on disk, and the banner text. The text is German, like the territory
// defense panel.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace announcements
{
enum class Command {
  kNone,
  kSet,
  kEnd,
};

struct Parsed {
  Command     command = Command::kNone;
  std::string text;
};

struct Announcement {
  // When it was posted, in Unix seconds.
  int64_t     posted = 0;
  std::string sender;
  std::string text;
};

struct State {
  std::optional<Announcement> current;
  // When the latest "!NGO-ENDE" was posted; anything posted before it stays gone.
  int64_t ended = 0;
};

namespace detail
{
  inline bool Space(char c)
  { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  inline std::string_view Trim(std::string_view text)
  {
    while (!text.empty() && Space(text.front()))
      text.remove_prefix(1);
    while (!text.empty() && Space(text.back()))
      text.remove_suffix(1);
    return text;
  }

  // Whether `text` starts with `prefix`, ignoring case for A to Z.
  inline bool StartsWith(std::string_view text, std::string_view prefix)
  {
    if (text.size() < prefix.size())
      return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
      const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
      if (lower(text[i]) != lower(prefix[i]))
        return false;
    }
    return true;
  }

  // One line, without the tabs the file separates fields with.
  inline std::string OneLine(std::string_view text)
  {
    std::string line(text);
    std::ranges::replace_if(line, [](char c) { return c == '\t' || c == '\r' || c == '\n'; }, ' ');
    return line;
  }

  // Shown as typed, with a "</" that would end <noparse> early broken by a zero-width space.
  inline std::string NoParse(std::string_view text)
  {
    std::string escaped;
    for (std::size_t i = 0; i < text.size(); ++i) {
      escaped += text[i];
      if (text[i] == '<' && i + 1 < text.size() && text[i + 1] == '/')
        escaped += "​";
    }
    return "<noparse>" + escaped + "</noparse>";
  }

  inline std::optional<int64_t> Number(std::string_view text)
  {
    int64_t    value        = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
      return std::nullopt;
    return value;
  }
} // namespace detail

// "!NGO <text>" sets an announcement and "!NGO-ENDE" ends it; case is ignored. Anything else, "!NGO" without text
// included, is no command.
inline Parsed Parse(std::string_view message)
{
  message = detail::Trim(message);
  if (detail::StartsWith(message, "!NGO-ENDE")) {
    const auto rest = message.substr(9);
    if (rest.empty() || detail::Space(rest.front()))
      return {Command::kEnd, {}};
    return {};
  }
  if (!detail::StartsWith(message, "!NGO"))
    return {};
  auto rest = message.substr(4);
  if (!rest.empty() && rest.front() == ':')
    rest.remove_prefix(1);
  else if (!rest.empty() && !detail::Space(rest.front()))
    return {};
  const auto text = detail::Trim(rest);
  if (text.empty())
    return {};
  return {Command::kSet, std::string(text)};
}

// A chat timestamp in Unix seconds, from seconds or milliseconds.
inline int64_t UnixSeconds(double timestamp)
{
  if (!(timestamp > 0))
    return 0;
  return static_cast<int64_t>(timestamp > 1e11 ? timestamp / 1000 : timestamp);
}

// Applies a leader's command posted at `posted`. A command no newer than the current announcement or the last end
// changes nothing, so the chat history can be read again in any order. Returns whether the state changed.
inline bool Apply(State& state, const Parsed& parsed, std::string_view sender, int64_t posted)
{
  if (parsed.command == Command::kNone || posted <= state.ended)
    return false;
  if (parsed.command == Command::kEnd) {
    state.ended = posted;
    if (state.current && state.current->posted < posted)
      state.current.reset();
    return true;
  }
  if (state.current && posted <= state.current->posted)
    return false;
  state.current = Announcement{posted, std::string(sender), parsed.text};
  return true;
}

// The announcement to show, if it is younger than `max_days`.
inline std::optional<Announcement> Shown(const State& state, int64_t now, int32_t max_days)
{
  if (!state.current || now - state.current->posted > static_cast<int64_t>(max_days) * 24 * 3600)
    return std::nullopt;
  return state.current;
}

// Two lines: "ended <seconds>" and, with an announcement, "<posted>\t<sender>\t<text>".
inline std::string Save(const State& state)
{
  std::string file = "ended\t" + std::to_string(state.ended) + "\n";
  if (state.current)
    file += std::to_string(state.current->posted) + "\t" + detail::OneLine(state.current->sender) + "\t"
            + detail::OneLine(state.current->text) + "\n";
  return file;
}

// What Save wrote; anything it does not recognise is left out.
inline State Load(std::string_view file)
{
  State state;
  while (!file.empty()) {
    const auto newline = file.find('\n');
    const auto line    = file.substr(0, newline);
    file               = newline == std::string_view::npos ? std::string_view{} : file.substr(newline + 1);
    const auto first   = line.find('\t');
    if (first == std::string_view::npos)
      continue;
    if (line.substr(0, first) == "ended") {
      state.ended = detail::Number(line.substr(first + 1)).value_or(0);
      continue;
    }
    const auto second = line.find('\t', first + 1);
    const auto posted = detail::Number(line.substr(0, first));
    if (second == std::string_view::npos || !posted || *posted <= state.ended)
      continue;
    const auto text = detail::Trim(line.substr(second + 1));
    if (!text.empty())
      state.current = Announcement{*posted, std::string(line.substr(first + 1, second - first - 1)), std::string(text)};
  }
  return state;
}

// The banner's rich text: a label, the sender and the announcement as typed.
inline std::string BannerText(const Announcement& announcement)
{
  return "<color=#7FD4FF><b>NGO-Ankündigung</b></color>  <color=#B8C7D9>" + detail::NoParse(announcement.sender)
         + ":</color> " + detail::NoParse(announcement.text);
}

// The alliance's ranks are numbered in order, the admiral's at one end; the commodore's is next to it.
inline bool LeaderRank(int32_t sender, int32_t admiral)
{ return std::abs(sender - admiral) <= 1; }
} // namespace announcements
