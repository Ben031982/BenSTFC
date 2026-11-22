#pragma once

// The battle summary of [ngo] battle_summary, without touching the game so it can be tested: it reads the battle
// report the game exports as tab-separated text (BattleReportCSVUtility.ExportToCSV) and sums it up. The export is
// in the game's language; columns are found by position, and only "JA"/"YES" are read as words. German text.

#include <algorithm>
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

// The hits' damage by type, summed over the battle as the game's detailed log shows it, except infusion: the game's
// figure for it is the hull damage's digits times 1 000 to 100 000, so infusion here is the hull damage of the hits
// that carry infusion. Left out: the game's "standard damage" total, which can be negative, and its prevented
// infusion, which flips sign.
struct DamageTypes {
  int64_t isolytic         = 0;
  int64_t infusion         = 0;
  int64_t polaron          = 0;
  // What the target's defenses took off: mitigated standard damage, blocked isolytic damage, the Apex Barrier,
  // and the crit damage mitigation.
  int64_t mitigated        = 0;
  int64_t isolytic_blocked = 0;
  int64_t apex             = 0;
  int64_t crit_blocked     = 0;
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
  int64_t hull_in   = 0;
  int64_t shield_in = 0;
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
          missed = Number(cells[cell]).value_or(0) == 0;
        if (missed)
          continue;
        const bool out = cells[3] == player;
        (out ? summary.hits_out : summary.hits_in) += 1;
        if (Yes(cells[13]))
          (out ? summary.crits_out : summary.crits_in) += 1;
        if (!out) {
          summary.hull_in += Number(cells[14]).value_or(0);
          summary.shield_in += Number(cells[15]).value_or(0);
        }
        if (cells.size() > 22) {
          const auto at   = [&](size_t cell) { return Number(cells[cell]).value_or(0); };
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
inline int64_t Repaired(const Summary& summary)
{
  const auto& player   = summary.sides.front();
  const auto  repaired = summary.hull_in - (player.hull - player.hull_left);
  return player.hull_left > 0 && repaired > 0 && repaired * 100 >= summary.hull_in ? repaired : 0;
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
  if (value >= 1'000'000'000'000)
    return scaled(value, 1'000'000'000'000, "Bio");
  if (value >= 1'000'000'000)
    return scaled(value, 1'000'000'000, "Mrd");
  if (value >= 1'000'000)
    return scaled(value, 1'000'000, "Mio");
  if (value >= 10'000)
    return scaled(value, 1'000, "Tsd");
  return std::to_string(value);
}

// The panel's text, TextMeshPro rich text: the result and the matchup over the whole width, then two columns: the
// battle on the left (rounds, hits, crits, hull, bridge crew, loot), the damage by type on the right, dealt and
// taken. Damage types that are 0 both ways are left out.
inline std::string PanelText(const Summary& summary)
{
  const auto& player = summary.sides[0];
  const auto& enemy  = summary.sides[1];
  const auto  color  = player.result == "SIEG" || player.result == "VICTORY"        ? "#5EE07A"
                       : player.result == "NIEDERLAGE" || player.result == "DEFEAT" ? "#FF6B6B"
                                                                                    : "#FFC857";
  // The text comes from the game and from player names: no tags of its own.
  const auto plain = [](const std::string& text) { return "<noparse>" + text + "</noparse>"; };
  const auto grey  = [](const std::string& text) { return "<color=#8FA3B8>" + text + "</color>"; };

  std::vector<std::string> left;
  const auto               row = [&](const char* name, const std::string& value) {
    left.push_back(grey(name) + "<pos=22%>" + value);
  };
  row("Runden", std::to_string(summary.rounds));
  row("Treffer", std::to_string(summary.hits_out) + " raus / " + std::to_string(summary.hits_in) + " rein");
  row("Crits", std::to_string(summary.crits_out) + " raus / " + std::to_string(summary.crits_in) + " rein");
  if (player.hull > 0)
    row("Hülle danach", std::to_string(player.hull_left * 100 / player.hull) + " %");
  if (const auto repaired = Repaired(summary))
    row("Repariert", Short(repaired) + " Hülle");
  left.emplace_back();
  left.push_back(grey("Brücke"));
  for (const auto& officer : player.officers)
    left.push_back(plain(officer));
  left.emplace_back();
  left.push_back(grey("Beute"));
  if (summary.rewards.empty())
    left.emplace_back("keine");
  for (const auto& [name, count] : summary.rewards)
    left.push_back(plain(name) + "<pos=40%>" + plain(count));

  std::vector<std::string> right{grey("Schaden") + "<pos=72%>" + grey("raus") + "<pos=87%>" + grey("rein")};
  // Dealt in green, taken in red.
  const auto damage = [&](const char* name, int64_t dealt, int64_t taken) {
    if (dealt != 0 || taken != 0)
      right.push_back(grey(name) + "<pos=72%><color=#5EE07A>" + Short(dealt) + "</color><pos=87%><color=#FF6B6B>"
                      + Short(taken) + "</color>");
  };
  const auto& out = summary.dealt;
  const auto& in  = summary.taken;
  damage("Hülle", enemy.hull - enemy.hull_left, summary.hull_in);
  damage("Schild", enemy.shield - enemy.shield_left, summary.shield_in);
  damage("Isolytisch", out.isolytic, in.isolytic);
  damage("Infusion", out.infusion, in.infusion);
  damage("Polaron", out.polaron, in.polaron);
  damage("Abgemildert", out.mitigated, in.mitigated);
  damage("Isolytisch abgewehrt", out.isolytic_blocked, in.isolytic_blocked);
  damage("Apex abgemildert", out.apex, in.apex);
  damage("Krit abgewehrt", out.crit_blocked, in.crit_blocked);

  std::string text = std::string("<size=130%><b><color=") + color + ">" + plain(player.result) + "</color></b></size>\n";
  text += plain(player.ship) + " gegen " + plain(enemy.name) + " (" + plain(enemy.level) + ")\n";
  text += grey(plain(summary.location) + (summary.location.empty() ? "" : " · ") + plain(summary.time)) + "\n";
  for (size_t line = 0; line < std::max(left.size(), right.size()); ++line) {
    text += "\n";
    if (line < left.size())
      text += left[line];
    if (line < right.size())
      text += "<pos=50%>" + right[line];
  }
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
    text += "; Hülle danach " + std::to_string(player.hull_left * 100 / player.hull) + " %";
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
  const auto  pair = [](const char* name, int64_t dealt, int64_t taken) {
    return std::string(name) + " " + Short(dealt) + " / " + Short(taken);
  };
  return "Schaden nach Art, raus / rein: " + pair("Isolytisch", out.isolytic, in.isolytic) + "; "
         + pair("Infusion", out.infusion, in.infusion) + "; " + pair("Polaron", out.polaron, in.polaron) + "; "
         + pair("Abgemildert", out.mitigated, in.mitigated) + "; "
         + pair("Isolytisch abgewehrt", out.isolytic_blocked, in.isolytic_blocked) + "; "
         + pair("Apex abgemildert", out.apex, in.apex) + "; " + pair("Krit abgewehrt", out.crit_blocked, in.crit_blocked);
}
} // namespace battle_summary
