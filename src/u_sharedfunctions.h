#ifndef U_SHAREDFUNCTIONS_H
#define U_SHAREDFUNCTIONS_H
#include <locale>
#include <algorithm>
#include <string>

inline void removeLeading(std::string &s)
{
    auto leftIterator = std::find_if(s.begin(), s.end(), [](char c){ return !std::isspace<char>(c, std::locale::classic()); });
    s.erase(s.begin(), leftIterator);
}

inline void removeTrailing(std::string &s)
{
    auto rightIterator = std::find_if(s.rbegin(), s.rend(), [](char c){ return !std::isspace<char>(c, std::locale::classic()); });
    s.erase(rightIterator.base(), s.end());
}

#endif // U_SHAREDFUNCTIONS_H
