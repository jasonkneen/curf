#pragma once
#include <map>
#include <string>

namespace curf {

std::string trim(const std::string& s);
std::string urlEncode(const std::string& s);
std::string urlDecode(const std::string& s);
std::map<std::string, std::string> parseQuery(const std::string& q);

// Turns whatever the user typed into a loadable URL: explicit URLs and full
// domains are opened directly, everything else becomes a Google search.
std::string resolveInput(const std::string& input);

}  // namespace curf
