// The allocation counter's replacement operator new and delete: every form a
// test or the standard library uses is counted and released by its own
// replacement. Under AddressSanitizer a form left to the runtime allocates
// with ASan's operator new and is freed with free here, which ASan reports as
// an alloc-dealloc-mismatch; the sanitizer CI run is that check.

#include "oscillate/test/allocation_counter.hpp"
#include "oscillate/test/cases.hpp"
#include "oscillate/test/check.hpp"

#include <algorithm>
#include <cstddef>
#include <new>
#include <vector>

namespace {

namespace test = oscillate::test;

constexpr std::size_t block_bytes = 64;
constexpr std::size_t sort_values = 4096;
constexpr std::size_t one_allocation = 1;

// Counts the operator new calls `allocate` makes on this thread.
template <typename Allocate> std::size_t allocations_of(Allocate&& allocate) {
  test::counted_allocations = 0;
  test::counting_allocations = true;
  allocate();
  test::counting_allocations = false;
  return test::counted_allocations;
}

// New and delete expressions only, through volatile pointers so the compiler
// cannot elide the pairs; a delete of a complete type calls the sized form.
void single_and_array_forms_are_counted() {
  REQUIRE_EQ(allocations_of([] {
               char* volatile single = new char;
               delete single;
             }),
             one_allocation);
  REQUIRE_EQ(allocations_of([] {
               char* volatile array = new char[block_bytes];
               delete[] array;
             }),
             one_allocation);
}

void nothrow_forms_are_counted_and_report_failure_as_null() {
  REQUIRE_EQ(allocations_of([] {
               char* volatile single = new (std::nothrow) char;
               REQUIRE_TRUE(single != nullptr);
               delete single;
             }),
             one_allocation);
  REQUIRE_EQ(allocations_of([] {
               char* volatile array = new (std::nothrow) char[block_bytes];
               REQUIRE_TRUE(array != nullptr);
               delete[] array;
             }),
             one_allocation);
  test::fail_next_allocation = true;
  char* volatile single = new (std::nothrow) char;
  REQUIRE_TRUE(single == nullptr);
  test::fail_next_allocation = true;
  char* volatile array = new (std::nothrow) char[block_bytes];
  REQUIRE_TRUE(array == nullptr);
}

// std::ranges::stable_sort takes its buffer with the nothrow form and releases it with
// the sized delete: the pairing the native DSP tests rely on.
void stable_sort_buffer_is_counted() {
  std::vector<std::size_t> values(sort_values);
  for (std::size_t index = 0; index < values.size(); ++index)
    values[index] = values.size() - index;
  REQUIRE_TRUE(allocations_of([&] { std::ranges::stable_sort(values); }) > 0);
  REQUIRE_TRUE(std::ranges::is_sorted(values));
}

} // namespace

int main(int argc, char** argv) {
  return test::run_cases(
      argc, argv, "allocation counter contract passed",
      {
          {"single_and_array_forms_are_counted", single_and_array_forms_are_counted},
          {"nothrow_forms_are_counted_and_report_failure_as_null",
           nothrow_forms_are_counted_and_report_failure_as_null},
          {"stable_sort_buffer_is_counted", stable_sort_buffer_is_counted},
      });
}
