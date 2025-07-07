#include "misc_util.hpp"

#include <fstream>
#include <iterator>
#include <map>
#include <regex>

namespace cjj365 {
namespace misc {

std::string append_GITHUB_HOST(const std::string& content,
                               const std::string& host) {
  // Regex to search for 'vcpkg_from_github' function
  std::regex github_func_pattern("vcpkg_from_github\\s*\\(");

  // Check if 'vcpkg_from_github' is present
  if (std::regex_search(content, github_func_pattern)) {
    // Check if GITHUB_HOST already exists
    
    if (content.find("GITHUB_HOST") == std::string::npos) {
      // Add GITHUB_HOST=<https://github.com> before the closing parenthesis
      std::string fmt = "$&\nGITHUB_HOST " + host + "\n";
      // Perform regex replace with a format string
      return std::regex_replace(content, github_func_pattern, fmt);
    } else {
      std::cerr << "GITHUB_HOST already exists!" << std::endl;
      return "b";
    }
  } else {
    std::cerr << "vcpkg_from_github not found!" << std::endl;
    return "c";
  }
  return content;
}

void modify_vcpkg_ports(const fs::path& directory) {
  // Regex to search for 'vcpkg_from_github' function and 'GITHUB_HOST'
  std::regex github_func_pattern(R"(vcpkg_from_github\([^\)]*\))");
  std::regex github_host_pattern(R"(GITHUB_HOST\s*<[^\>]*>)");

  // Iterate through all files in the directory
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (entry.is_regular_file() &&
        entry.path().extension() == ".portfile.cmake") {
      std::ifstream file(entry.path());
      std::string content((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());

      // Search for 'vcpkg_from_github' and check if 'GITHUB_HOST' is present
      if (std::regex_search(content, github_func_pattern)) {
        // Check if GITHUB_HOST already exists
        if (!std::regex_search(content, github_host_pattern)) {
          // Add GITHUB_HOST=<https://github.com> before the closing parenthesis

          // Format string to append GITHUB_HOST parameter
          std::string fmt = " GITHUB_HOST <https://github.com>";

          // Output string where the result will be stored
          std::string output;

          // Perform regex replace with a format string
          std::regex_replace(std::back_inserter(output), content.begin(),
                             content.end(), github_func_pattern, fmt);
          // Write the modified content back to the file
          std::ofstream out_file(entry.path());
          out_file << output;
          out_file.close();
          std::cout << "Modified: " << entry.path() << std::endl;
        }
      }
    }
  }
}

}  // namespace misc
}  // namespace cjj365