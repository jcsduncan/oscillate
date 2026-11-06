#pragma once

// A contract's cases, runnable one at a time so CTest can spread a long
// contract over cores and give each case its own budget (see
// oscillate_add_case_tests in cmake/ProjectOptions.cmake).
//
//   <binary>           runs every case, in order
//   <binary> <name>... runs the named cases
//   <binary> --list    prints the case names, one per line
//
//   int main(int argc, char** argv) {
//     return oscillate::test::run_cases(argc, argv, "contract passed",
//                                       {{"first_case", first_case}, ...});
//   }

#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <vector>

namespace oscillate::test {

struct Case {
  std::string_view name;
  std::function<void()> run;
};

// Exit status of a run that named a case the binary does not have.
inline constexpr int unknown_case_status = 2;

inline int run_cases(int argc, char** argv, std::string_view passed,
                     std::initializer_list<Case> cases) {
  const std::vector<Case> all(cases);
  std::vector<std::string_view> wanted(argv + 1, argv + argc);
  if (wanted.size() == 1 && wanted.front() == "--list") {
    for (const auto& c : all) {
      std::cout << c.name << '\n';
    }
    return 0;
  }
  for (const auto name : wanted) {
    bool known = false;
    for (const auto& c : all) {
      known = known || c.name == name;
    }
    if (!known) {
      std::cerr << "unknown case: " << name << '\n';
      return unknown_case_status;
    }
  }
  std::string_view current;
  try {
    for (const auto& c : all) {
      bool selected = wanted.empty();
      for (const auto name : wanted) {
        selected = selected || c.name == name;
      }
      if (selected) {
        current = c.name;
        c.run();
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "FAILED in case " << current << ": " << error.what() << '\n';
    return 1;
  }
  std::cout << passed << '\n';
  return 0;
}

} // namespace oscillate::test
