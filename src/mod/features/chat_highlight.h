#pragma once

// [ui] chat_highlight_words, without touching the game so it can be tested: finds the words in a chat message's
// TextMeshPro text and marks them, and the whole message, in rich text.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chat_highlight
{
namespace detail
{
  inline char Lower(char c)
  { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

  // Letters, digits and every byte of a multi-byte UTF-8 character belong to a word.
  inline bool WordByte(char c)
  {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u >= 0x80;
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
} // namespace detail

// The comma list of words, trimmed; blank entries are left out.
inline std::vector<std::string> Words(std::string_view list)
{
  std::vector<std::string> words;
  while (!list.empty()) {
    const auto comma = list.find(',');
    auto       word  = list.substr(0, comma);
    list             = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    while (!word.empty() && (word.front() == ' ' || word.front() == '\t'))
      word.remove_prefix(1);
    while (!word.empty() && (word.back() == ' ' || word.back() == '\t'))
      word.remove_suffix(1);
    if (!word.empty())
      words.emplace_back(word);
  }
  return words;
}

// The text with each whole-word match of `words` in bold and `color`, and the message on a faint background of that
// color; empty without a match. Case is ignored for A to Z. With `escape`, the text is shown as typed, for a label
// that does not read rich text; otherwise its tags are skipped while matching and kept.
inline std::optional<std::string> Mark(std::string_view text, std::span<const std::string> words,
                                       std::string_view color, bool escape)
{
  const std::string open = "<mark=" + std::string(color) + "33>";
  if (text.find(open) != std::string_view::npos)
    return std::nullopt;

  std::vector<std::pair<std::size_t, std::size_t>> found;
  for (std::size_t i = 0; i < text.size();) {
    if (!escape && text[i] == '<') {
      const auto close = text.find('>', i);
      if (close == std::string_view::npos)
        break;
      i = close + 1;
      continue;
    }
    if (i > 0 && detail::WordByte(text[i - 1]) && detail::WordByte(text[i])) {
      ++i;
      continue;
    }
    std::size_t length = 0;
    for (const auto& word : words) {
      if (word.empty() || word.size() > text.size() - i || word.size() <= length)
        continue;
      bool same = true;
      for (std::size_t k = 0; k < word.size() && same; ++k)
        same = detail::Lower(text[i + k]) == detail::Lower(word[k]);
      const auto end = i + word.size();
      if (same && !(end < text.size() && detail::WordByte(text[end]) && detail::WordByte(word.back())))
        length = word.size();
    }
    if (length > 0) {
      found.emplace_back(i, length);
      i += length;
    } else {
      ++i;
    }
  }
  if (found.empty())
    return std::nullopt;

  const auto plain = [escape](std::string_view part) {
    return escape && !part.empty() ? detail::NoParse(part) : std::string(part);
  };
  std::string marked = open;
  std::size_t from   = 0;
  for (const auto& [start, length] : found) {
    marked += plain(text.substr(from, start - from));
    marked += "<b><color=" + std::string(color) + ">" + plain(text.substr(start, length)) + "</color></b>";
    from = start + length;
  }
  marked += plain(text.substr(from));
  marked += "</mark>";
  return marked;
}

// "#RRGGBB".
inline bool ValidColor(std::string_view color)
{
  if (color.size() != 7 || color[0] != '#')
    return false;
  for (const char c : color.substr(1))
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  return true;
}
} // namespace chat_highlight
