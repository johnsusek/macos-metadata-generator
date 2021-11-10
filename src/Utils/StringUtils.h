//
// Created by Yavor Georgiev on 14.01.16 г..
//

#pragma once

#include <algorithm>
#include <sstream>
#include <string>

namespace StringUtils {
static inline void rtrim(std::string &s) {
  s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), s.end());
}


static inline bool starts_with(const std::string value, const std::string prefix)
{
  using namespace std;
  
  if (value.length() < prefix.length()) {
    return false;
  }
  
  return std::mismatch(prefix.begin(), prefix.end(), value.begin()).first == prefix.end();
}

static inline bool ends_with(const std::string value, const std::string suffix)
{
  if (suffix.size() > value.size()) return false;
  
  return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

template <class OutputIterator, typename CharT>
size_t split(const std::basic_string<CharT>& input, CharT delim, OutputIterator output)
{
  size_t count = 0;
  std::basic_stringstream<CharT> ss(input);
  std::basic_string<CharT> item;
  while (std::getline(ss, item, delim)) {
    if (item.size() == 0) {
      continue;
    }
    
    *(output++) = item;
    count++;
  }
  
  return count;
};

template <typename Range, typename Value = typename Range::value_type>
std::string join(Range const& elements, const char *const delimiter) {
  std::ostringstream os;
  auto b = begin(elements), e = end(elements);
  
  if (b != e) {
    std::copy(b, prev(e), std::ostream_iterator<Value>(os, delimiter));
    b = prev(e);
  }
  if (b != e) {
    os << *b;
  }
  
  return os.str();
}

/*! note: imput is assumed to not contain NUL characters
 */
template <typename Input, typename Output, typename Value = typename Output::value_type>
void split(char delimiter, Output &output, Input const& input) {
  using namespace std;
  for (auto cur = begin(input), beg = cur; ; ++cur) {
    if (cur == end(input) || *cur == delimiter || !*cur) {
      output.insert(output.end(), Value(beg, cur));
      if (cur == end(input) || !*cur)
        break;
      else
        beg = next(cur);
    }
  }
}

}
