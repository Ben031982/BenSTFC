#pragma once

// The battle summary of [ngo] battle_summary, without touching the game so it can be tested: it reads the battle
// report the game exports as tab-separated text (BattleReportCSVUtility.ExportToCSV) and sums it up. The export is
// in the game's language; columns are found by position, and only "JA"/"YES" are read as words. German text.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace battle_summary
{
struct Side {
  std::string              name;
  std::string              level;
  std::string              result;
  std::string              ship;
  std::vector<std::string> officers;
  int64_t                  hull        = 0;
  int64_t                  hull_left   = 0;
  int64_t                  shield      = 0;
  int64_t                  shield_left = 0;
};

struct Summary {
  // The report's first rows: the player's side first.
  std::vector<Side>                                sides;
  std::string                                      location;
  std::string                                      time;
  std::vector<std::pair<std::string, std::string>> rewards;
  int32_t                                          rounds    = 0;
  int32_t                                          hits_out  = 0;
  int32_t                                          hits_in   = 0;
  int32_t                                          crits_out = 0;
  int32_t                                          crits_in  = 0;
  // The header rows of sections this does not read yet, for the log.
  std::vector<std::string> unread;
};

namespace detail
{
  inline std::vector<std::string_view> Split(std::string_view line, char separator = '\t')
  {
    std::vector<std::string_view> cells;
    while (true) {
      const auto at = line.find(separator);
      cells.push_back(line.substr(0, at));
      if (at == std::string_view::npos)
        return cells;
      line.remove_prefix(at + 1);
    }
  }

  inline std::optional<int64_t> Number(std::string_view text)
  {
    int64_t    value        = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size())
      return std::nullopt;
    return value;
  }

  inline bool Yes(std::string_view text)
  { return text == "JA" || text == "Ja" || text == "ja" || text == "YES" || text == "Yes" || text == "yes"; }

  // A cell that says nothing.
  inline bool Empty(std::string_view text)
  { return text.empty() || text == "--"; }

  // The report's sections: runs of non-empty lines, each split into cells.
  inline std::vector<std::vector<std::vector<std::string_view>>> Sections(std::string_view csv)
  {
    std::vector<std::vector<std::vector<std::string_view>>> sections(1);
    while (!csv.empty()) {
      const auto newline = csv.find('\n');
      auto       line    = csv.substr(0, newline);
      csv                = newline == std::string_view::npos ? std::string_view{} : csv.substr(newline + 1);
      if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
      if (line.find_first_not_of(" \t") == std::string_view::npos) {
        if (!sections.back().empty())
          sections.emplace_back();
        continue;
      }
      sections.back().push_back(Split(line));
    }
    if (sections.back().empty())
      sections.pop_back();
    return sections;
  }
} // namespace detail

// The summary of an exported report, or nothing when its first section is not the two sides.
inline std::optional<Summary> Parse(std::string_view csv)
{
  using namespace detail;
  const auto sections = Sections(csv);
  if (sections.empty() || sections.front().size() < 3 || sections.front().front().size() < 16)
    return std::nullopt;

  Summary summary;
  for (size_t row = 1; row < sections.front().size(); ++row) {
    const auto& cells = sections.front()[row];
    if (cells.size() < 16)
      continue;
    Side side{.name = std::string(cells[0]), .level = std::string(cells[1]), .result = std::string(cells[2]),
              .ship = std::string(cells[3])};
    for (size_t officer = 7; officer <= 9; ++officer)
      if (!Empty(cells[officer]))
        side.officers.emplace_back(cells[officer]);
    side.hull        = Number(cells[10]).value_or(0);
    side.hull_left   = Number(cells[11]).value_or(0);
    side.shield      = Number(cells[12]).value_or(0);
    side.shield_left = Number(cells[13]).value_or(0);
    if (summary.sides.empty()) {
      summary.location = std::string(cells[14]);
      summary.time     = std::string(cells[15]);
    }
    summary.sides.push_back(std::move(side));
  }
  if (summary.sides.size() < 2)
    return std::nullopt;
  const auto& player = summary.sides.front().name;

  for (size_t index = 1; index < sections.size(); ++index) {
    const auto& section = sections[index];
    const auto& header  = section.front();
    // The battle log: every row starts with the round and the event's number.
    const bool log = header.size() >= 20 && section.size() > 1 && Number(section[1][0]) && Number(section[1][1]);
    if (log) {
      for (size_t row = 1; row < section.size(); ++row) {
        const auto& cells = section[row];
        if (cells.size() < 16)
          continue;
        if (const auto round = Number(cells[0]); round && *round > summary.rounds)
          summary.rounds = static_cast<int32_t>(*round);
        // An attack has damage; other events leave the damage columns empty.
        if (Empty(cells[14]) && Empty(cells[15]))
          continue;
        const bool out = cells[3] == player;
        (out ? summary.hits_out : summary.hits_in) += 1;
        if (Yes(cells[13]))
          (out ? summary.crits_out : summary.crits_in) += 1;
      }
    } else if (header.size() == 2) {
      for (size_t row = 1; row < section.size(); ++row)
        if (section[row].size() >= 2)
          summary.rewards.emplace_back(std::string(section[row][0]), std::string(section[row][1]));
    } else if (!(header.size() >= 20 && section.size() > 1 && !Number(section[1][0]))) {
      // Not the fleet stats either: unknown so far, such as cargo taken.
      std::string text;
      for (size_t cell = 0; cell < header.size() && cell < 8; ++cell)
        text += (cell ? " | " : "") + std::string(header[cell]);
      summary.unread.push_back(text);
    }
  }
  return summary;
}

// "1,24 Mrd", "312 Tsd" or "950": short enough for the panel.
inline std::string Short(int64_t value)
{
  const auto scaled = [](int64_t number, int64_t unit, const char* name) {
    // One decimal below 10, none above.
    const int64_t tenths = (number * 10 + unit / 2) / unit;
    if (tenths >= 100)
      return std::to_string((number + unit / 2) / unit) + " " + name;
    std::string text = std::to_string(tenths / 10);
    if (tenths % 10 != 0)
      text += "," + std::to_string(tenths % 10);
    return text + " " + name;
  };
  if (value < 0)
    return "-" + Short(-value);
  if (value >= 1'000'000'000)
    return scaled(value, 1'000'000'000, "Mrd");
  if (value >= 1'000'000)
    return scaled(value, 1'000'000, "Mio");
  if (value >= 10'000)
    return scaled(value, 1'000, "Tsd");
  return std::to_string(value);
}

// One line for the log: what the panel will show.
inline std::string Describe(const Summary& summary)
{
  const auto& player = summary.sides[0];
  const auto& enemy  = summary.sides[1];
  std::string text   = player.result + ": " + player.ship + " gegen " + enemy.name + " (" + enemy.level + ")";
  if (!summary.location.empty())
    text += " in " + summary.location;
  text += ", " + summary.time + "; Runden " + std::to_string(summary.rounds);
  text += "; Treffer " + std::to_string(summary.hits_out) + " raus / " + std::to_string(summary.hits_in) + " rein";
  text += "; Crits " + std::to_string(summary.crits_out) + " raus / " + std::to_string(summary.crits_in) + " rein";
  text += "; Schaden raus " + Short(enemy.hull - enemy.hull_left) + " Hülle + "
          + Short(enemy.shield - enemy.shield_left) + " Schild";
  text += "; Schaden rein " + Short(player.hull - player.hull_left) + " Hülle + "
          + Short(player.shield - player.shield_left) + " Schild";
  if (player.hull > 0)
    text += "; Hülle danach " + std::to_string(player.hull_left * 100 / player.hull) + " %";
  text += "; Brücke:";
  for (size_t i = 0; i < player.officers.size(); ++i)
    text += (i ? ", " : " ") + player.officers[i];
  text += "; Beute:";
  if (summary.rewards.empty())
    text += " keine";
  for (size_t i = 0; i < summary.rewards.size(); ++i)
    text += (i ? ", " : " ") + summary.rewards[i].first + " " + summary.rewards[i].second;
  return text;
}
} // namespace battle_summary
