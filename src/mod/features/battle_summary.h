#pragma once

// The battle summary of [ngo] battle_summary, without touching the game so it can be tested: it reads the battle
// report the game exports as tab-separated text (BattleReportCSVUtility.ExportToCSV) and sums it up. The export is
// in the game's language; columns are found by position, and only "JA"/"YES" are read as words. German text.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
  // Hit points can pass what a 64-bit integer holds (the V'ger), so they are floating point.
  double hull        = 0;
  double hull_left   = 0;
  double shield      = 0;
  double shield_left = 0;
};

// The hits' damage by type, summed over the battle as the game's detailed log shows it, except infusion: the game's
// figure for it is the hull damage's digits times 1 000 to 100 000, so infusion here is the hull damage of the hits
// that carry infusion. Left out: the game's "standard damage" total, which can be negative, and its prevented
// infusion, which flips sign.
struct DamageTypes {
  double isolytic         = 0;
  double infusion         = 0;
  double polaron          = 0;
  // What the target's defenses took off: mitigated standard damage, blocked isolytic damage, the Apex Barrier,
  // and the crit damage mitigation.
  double mitigated        = 0;
  double isolytic_blocked = 0;
  double apex             = 0;
  double crit_blocked     = 0;
};

struct Summary {
  // The report's first rows: the player's side first.
  std::vector<Side>                                sides;
  std::string                                      location;
  std::string                                      time;
  std::vector<std::pair<std::string, std::string>> rewards;
  // Later two-column sections, such as the galactic anomalies' buffs: not loot.
  std::vector<std::pair<std::string, std::string>> buffs;
  int32_t                                          rounds    = 0;
  int32_t                                          hits_out  = 0;
  int32_t                                          hits_in   = 0;
  int32_t                                          crits_out = 0;
  int32_t                                          crits_in  = 0;
  // The damage the player took, hit by hit; more than the hull lost when it was repaired during the battle.
  double hull_in   = 0;
  double shield_in = 0;
  // By type: the player's hits, and the hits the player took.
  DamageTypes dealt;
  DamageTypes taken;
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

  inline std::optional<int64_t> Integer(std::string_view text)
  {
    int64_t    value        = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size())
      return std::nullopt;
    return value;
  }

  // A whole number, also as the export writes large ones: "1,50000000000001E+16". Rounded; without floating point.
  inline std::optional<int64_t> Number(std::string_view text)
  {
    if (const auto value = Integer(text))
      return value;
    const auto exponent_at = text.find_first_of("Ee");
    int64_t    exponent    = 0;
    if (exponent_at != std::string_view::npos) {
      auto exponent_text = text.substr(exponent_at + 1);
      if (!exponent_text.empty() && exponent_text.front() == '+')
        exponent_text.remove_prefix(1);
      const auto parsed = Integer(exponent_text);
      if (!parsed || *parsed < -30 || *parsed > 30)
        return std::nullopt;
      exponent = *parsed;
      text     = text.substr(0, exponent_at);
    }
    const bool negative = !text.empty() && text.front() == '-';
    if (negative)
      text.remove_prefix(1);
    const auto separator = text.find_first_of(",.");
    const auto whole     = text.substr(0, separator);
    const auto fraction  = separator == std::string_view::npos ? std::string_view{} : text.substr(separator + 1);
    constexpr std::string_view kDigits = "0123456789";
    if (whole.empty() || whole.size() + fraction.size() > 18 || whole.find_first_not_of(kDigits) != whole.npos
        || fraction.find_first_not_of(kDigits) != fraction.npos)
      return std::nullopt;
    int64_t digits = 0;
    for (const auto part : {whole, fraction})
      for (const char digit : part)
        digits = digits * 10 + (digit - '0');
    exponent -= static_cast<int64_t>(fraction.size());
    for (; exponent > 0; --exponent) {
      if (digits > INT64_MAX / 10)
        return std::nullopt;
      digits *= 10;
    }
    for (; exponent < 0; ++exponent)
      digits = exponent == -1 ? (digits + 5) / 10 : digits / 10;
    return negative ? -digits : digits;
  }

  // Any number as the export writes it, "1,50000000000001E+16" too, of any size. Whole numbers below 2^53, the
  // export's mantissas times a power of ten included, come out exact.
  inline std::optional<double> Real(std::string_view text)
  {
    if (const auto value = Integer(text))
      return static_cast<double>(*value);
    const auto exponent_at = text.find_first_of("Ee");
    int64_t    exponent    = 0;
    if (exponent_at != std::string_view::npos) {
      auto exponent_text = text.substr(exponent_at + 1);
      if (!exponent_text.empty() && exponent_text.front() == '+')
        exponent_text.remove_prefix(1);
      const auto parsed = Integer(exponent_text);
      if (!parsed || *parsed < -300 || *parsed > 300)
        return std::nullopt;
      exponent = *parsed;
      text     = text.substr(0, exponent_at);
    }
    const bool negative = !text.empty() && text.front() == '-';
    if (negative)
      text.remove_prefix(1);
    const auto separator = text.find_first_of(",.");
    const auto whole     = text.substr(0, separator);
    const auto fraction  = separator == std::string_view::npos ? std::string_view{} : text.substr(separator + 1);
    constexpr std::string_view kDigits = "0123456789";
    if (whole.empty() || whole.find_first_not_of(kDigits) != whole.npos
        || fraction.find_first_not_of(kDigits) != fraction.npos)
      return std::nullopt;
    double digits = 0;
    for (const auto part : {whole, fraction})
      for (const char digit : part)
        digits = digits * 10 + (digit - '0');
    exponent -= static_cast<int64_t>(fraction.size());
    for (; exponent > 0; --exponent)
      digits *= 10;
    for (; exponent < 0; ++exponent)
      digits /= 10;
    if (!std::isfinite(digits))
      return std::nullopt;
    return negative ? -digits : digits;
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
    side.hull        = Real(cells[10]).value_or(0);
    side.hull_left   = Real(cells[11]).value_or(0);
    side.shield      = Real(cells[12]).value_or(0);
    side.shield_left = Real(cells[13]).value_or(0);
    if (summary.sides.empty()) {
      summary.location = std::string(cells[14]);
      summary.time     = std::string(cells[15]);
    }
    summary.sides.push_back(std::move(side));
  }
  if (summary.sides.size() < 2)
    return std::nullopt;
  const auto& player    = summary.sides.front().name;
  bool        loot_read = false;

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
        // A shot with 0 in every damage column missed, even when it is marked critical.
        bool missed = true;
        for (size_t cell = 14; cell <= 25 && cell < cells.size() && missed; ++cell)
          missed = Real(cells[cell]).value_or(0) == 0;
        if (missed)
          continue;
        const bool out = cells[3] == player;
        (out ? summary.hits_out : summary.hits_in) += 1;
        if (Yes(cells[13]))
          (out ? summary.crits_out : summary.crits_in) += 1;
        if (!out) {
          summary.hull_in += Real(cells[14]).value_or(0);
          summary.shield_in += Real(cells[15]).value_or(0);
        }
        if (cells.size() > 22) {
          const auto at   = [&](size_t cell) { return Real(cells[cell]).value_or(0); };
          auto&      sums = out ? summary.dealt : summary.taken;
          sums.mitigated += at(16);
          sums.isolytic_blocked += at(17);
          sums.apex += at(18);
          sums.isolytic += at(20);
          sums.crit_blocked += at(21);
          sums.polaron += at(22);
          if (cells.size() > 24 && at(24) != 0)
            sums.infusion += at(14);
        }
      }
    } else if (header.size() == 2) {
      // The loot comes first; a later two-column section holds buffs.
      auto& pairs = loot_read ? summary.buffs : summary.rewards;
      loot_read   = true;
      for (size_t row = 1; row < section.size(); ++row)
        if (section[row].size() >= 2)
          pairs.emplace_back(std::string(section[row][0]), std::string(section[row][1]));
    } else if (!(header.size() >= 20 && section.size() > 1 && !Number(section[1][0]))) {
      // Not the fleet stats either: unknown so far, such as cargo taken.
      std::string text;
      for (size_t cell = 0; cell < header.size() && cell < 8; ++cell)
        text += (cell ? " | " : "") + std::string(header[cell]);
      summary.unread.push_back(text);
    }
  }
  // Without a battle log, the damage taken is what the hull and shield lost; a destroyed ship cannot lose more than
  // the hull it had, beyond which the last hit is overkill.
  const auto& side = summary.sides.front();
  if (summary.rounds == 0) {
    summary.hull_in   = side.hull - side.hull_left;
    summary.shield_in = side.shield - side.shield_left;
  } else if (side.hull_left == 0 && summary.hull_in > side.hull) {
    summary.hull_in = side.hull;
  }
  return summary;
}

// The hull repaired during the battle: what the hits took beyond what the hull lost. Nothing when the ship was
// destroyed (the last hit's overkill is no repair) or for rounding noise below 1 %.
inline double Repaired(const Summary& summary)
{
  const auto& player   = summary.sides.front();
  const auto  repaired = summary.hull_in - (player.hull - player.hull_left);
  return player.hull_left > 0 && repaired > 0 && repaired * 100 >= summary.hull_in ? repaired : 0;
}

// "1,24 Mrd", "312 Tsd" or "950": short enough for the panel. Up to Trd (Trillion, 10^18): the V'ger's numbers.
inline std::string Short(double value)
{
  const auto whole = [](double number) {
    char buffer[400];
    std::snprintf(buffer, sizeof buffer, "%.0f", std::floor(number + 0.5));
    return std::string(buffer);
  };
  const auto scaled = [&](double number, double unit, const char* name) {
    // One decimal below 10, none above.
    const auto tenths = static_cast<int64_t>(std::floor(number * 10 / unit + 0.5));
    if (tenths >= 100)
      return whole(number / unit) + " " + name;
    std::string text = std::to_string(tenths / 10);
    if (tenths % 10 != 0)
      text += "," + std::to_string(tenths % 10);
    return text + " " + name;
  };
  if (value < 0)
    return "-" + Short(-value);
  if (value >= 1e18)
    return scaled(value, 1e18, "Trd");
  if (value >= 1e15)
    return scaled(value, 1e15, "Brd");
  if (value >= 1e12)
    return scaled(value, 1e12, "Bio");
  if (value >= 1e9)
    return scaled(value, 1e9, "Mrd");
  if (value >= 1e6)
    return scaled(value, 1e6, "Mio");
  if (value >= 1e4)
    return scaled(value, 1e3, "Tsd");
  return whole(value);
}

// Whole percent of `part` in `total`, rounded down.
inline std::string Percent(double part, double total)
{ return std::to_string(static_cast<int64_t>(part * 100 / total)) + " %"; }

// "8.268.261": a whole number with dots between the thousands; anything else as it is.
inline std::string Grouped(const std::string& text)
{
  if (text.empty() || text.size() > 30 || text.find_first_not_of("0123456789") != std::string::npos)
    return text;
  std::string grouped;
  for (size_t i = 0; i < text.size(); ++i) {
    if (i > 0 && (text.size() - i) % 3 == 0)
      grouped += '.';
    grouped += text[i];
  }
  return grouped;
}

// A piece of a line of the summary: where it starts, in percent of the width (0 continues the line), its text, its
// color as "#RRGGBB" (empty for the default), and whether the text comes from the game or a player, so it must not
// be read as tags.
struct Segment {
  int32_t     at = 0;
  std::string text;
  std::string color;
  bool        game = false;
};
struct Line {
  std::vector<Segment> segments;
  // The result, larger and bold.
  bool headline = false;
};

inline constexpr const char* kGrey  = "#8FA3B8";
inline constexpr const char* kGreen = "#5EE07A";
inline constexpr const char* kRed   = "#FF6B6B";
inline constexpr const char* kAmber = "#FFC857";

// The summary as the panel and the copied image show it: the result and the matchup over the whole width, then two
// columns: the battle on the left (rounds, hits, crits, hull, bridge crew, loot), the damage by type on the right,
// dealt in green and taken in red, and below it, in plain white, what the enemy's defenses took off the player's
// hits and what the player's took off the enemy's. Types that are 0 both ways are left out.
inline std::vector<Line> Layout(const Summary& summary)
{
  const auto& player = summary.sides[0];
  const auto& enemy  = summary.sides[1];
  const auto  color  = player.result == "SIEG" || player.result == "VICTORY"        ? kGreen
                       : player.result == "NIEDERLAGE" || player.result == "DEFEAT" ? kRed
                                                                                    : kAmber;

  std::vector<Line> left;
  const auto        row = [&](const char* name, const std::string& value) {
    left.push_back({{{0, name, kGrey}, {22, value}}});
  };
  row("Runden", std::to_string(summary.rounds));
  row("Treffer", std::to_string(summary.hits_out) + " raus / " + std::to_string(summary.hits_in) + " rein");
  row("Crits", std::to_string(summary.crits_out) + " raus / " + std::to_string(summary.crits_in) + " rein");
  if (player.hull > 0)
    row("Hülle danach", Percent(player.hull_left, player.hull));
  if (const auto repaired = Repaired(summary))
    row("Repariert", Short(repaired) + " Hülle");
  left.emplace_back();
  left.push_back({{{0, "Brücke", kGrey}}});
  for (const auto& officer : player.officers)
    left.push_back({{{0, officer, "", true}}});
  left.emplace_back();
  left.push_back({{{0, "Beute", kGrey}}});
  if (summary.rewards.empty())
    left.push_back({{{0, "keine"}}});
  for (const auto& [name, count] : summary.rewards)
    left.push_back({{{0, name, "", true}, {40, Grouped(count), "", true}}});

  std::vector<Line> right{{{{50, "Schaden", kGrey}, {72, "raus", kGrey}, {87, "rein", kGrey}}}};
  const auto        damage = [&](const char* name, double dealt, double taken) {
    if (dealt != 0 || taken != 0)
      right.push_back({{{50, name, kGrey}, {72, Short(dealt), kGreen}, {87, Short(taken), kRed}}});
  };
  const auto& out = summary.dealt;
  const auto& in  = summary.taken;
  damage("Hülle", enemy.hull - enemy.hull_left, summary.hull_in);
  damage("Schild", enemy.shield - enemy.shield_left, summary.shield_in);
  damage("Isolytisch", out.isolytic, in.isolytic);
  damage("Infusion", out.infusion, in.infusion);
  damage("Polaron", out.polaron, in.polaron);

  std::vector<Line> defense;
  const auto        defended = [&](const char* name, double by_enemy, double by_player) {
    if (by_enemy != 0 || by_player != 0)
      defense.push_back({{{50, name, kGrey}, {72, Short(by_enemy)}, {87, Short(by_player)}}});
  };
  defended("Standard", out.mitigated, in.mitigated);
  defended("Isolytisch", out.isolytic_blocked, in.isolytic_blocked);
  defended("Apex-Barriere", out.apex, in.apex);
  defended("Krit-Abmilderung", out.crit_blocked, in.crit_blocked);
  if (!defense.empty()) {
    right.emplace_back();
    right.push_back({{{50, "Abgewehrt", kGrey}, {72, "vom Gegner", kGrey}, {87, "von dir", kGrey}}});
    right.insert(right.end(), defense.begin(), defense.end());
  }

  std::vector<Line> lines;
  lines.push_back({{{0, player.result, color, true}}, true});
  lines.push_back({{{0, player.ship, "", true},
                    {0, " gegen "},
                    {0, enemy.name, "", true},
                    {0, " ("},
                    {0, enemy.level, "", true},
                    {0, ")"}}});
  Line where;
  if (!summary.location.empty())
    where.segments = {{0, summary.location, kGrey, true}, {0, " · ", kGrey}};
  where.segments.push_back({0, summary.time, kGrey, true});
  lines.push_back(std::move(where));
  lines.emplace_back();
  for (size_t line = 0; line < std::max(left.size(), right.size()); ++line) {
    Line merged;
    if (line < left.size())
      merged.segments = left[line].segments;
    if (line < right.size())
      merged.segments.insert(merged.segments.end(), right[line].segments.begin(), right[line].segments.end());
    lines.push_back(std::move(merged));
  }
  return lines;
}

// The panel's text: the layout as TextMeshPro rich text.
inline std::string PanelText(const Summary& summary)
{
  std::string text;
  for (const auto& line : Layout(summary)) {
    std::string rendered;
    for (const auto& segment : line.segments) {
      if (segment.at > 0)
        rendered += "<pos=" + std::to_string(segment.at) + "%>";
      if (!segment.color.empty())
        rendered += "<color=" + segment.color + ">";
      rendered += segment.game ? "<noparse>" + segment.text + "</noparse>" : segment.text;
      if (!segment.color.empty())
        rendered += "</color>";
    }
    if (line.headline)
      rendered = "<size=130%><b>" + rendered + "</b></size>";
    text += rendered + "\n";
  }
  if (!text.empty())
    text.pop_back();
  return text;
}

// The panel's lines, for its height; the first is the larger headline.
inline int32_t PanelLines(const std::string& text)
{ return static_cast<int32_t>(std::count(text.begin(), text.end(), '\n')) + 1; }

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
  text += "; Schaden rein " + Short(summary.hull_in) + " Hülle";
  if (const auto repaired = Repaired(summary))
    text += " (" + Short(repaired) + " repariert)";
  text += " + " + Short(summary.shield_in) + " Schild";
  if (player.hull > 0)
    text += "; Hülle danach " + Percent(player.hull_left, player.hull);
  text += "; Brücke:";
  for (size_t i = 0; i < player.officers.size(); ++i)
    text += (i ? ", " : " ") + player.officers[i];
  text += "; Beute:";
  if (summary.rewards.empty())
    text += " keine";
  for (size_t i = 0; i < summary.rewards.size(); ++i)
    text += (i ? ", " : " ") + summary.rewards[i].first + " " + summary.rewards[i].second;
  if (!summary.buffs.empty())
    text += "; Buffs:";
  for (size_t i = 0; i < summary.buffs.size(); ++i)
    text += (i ? ", " : " ") + summary.buffs[i].first + " " + summary.buffs[i].second;
  return text;
}

// A second log line: the damage by type, dealt / taken, every type.
inline std::string DescribeDamage(const Summary& summary)
{
  const auto& out  = summary.dealt;
  const auto& in   = summary.taken;
  const auto  pair = [](const char* name, double dealt, double taken) {
    return std::string(name) + " " + Short(dealt) + " / " + Short(taken);
  };
  return "Schaden nach Art, raus / rein: " + pair("Isolytisch", out.isolytic, in.isolytic) + "; "
         + pair("Infusion", out.infusion, in.infusion) + "; " + pair("Polaron", out.polaron, in.polaron) + "; "
         + pair("Abgemildert", out.mitigated, in.mitigated) + "; "
         + pair("Isolytisch abgewehrt", out.isolytic_blocked, in.isolytic_blocked) + "; "
         + pair("Apex abgemildert", out.apex, in.apex) + "; " + pair("Krit abgewehrt", out.crit_blocked, in.crit_blocked);
}
} // namespace battle_summary
