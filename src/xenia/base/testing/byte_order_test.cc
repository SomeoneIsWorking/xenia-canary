#include "xenia/base/byte_order.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>

#include "third_party/catch/include/catch.hpp"

namespace xe::test {

template <typename T, std::endian E>
void CheckEndianRepresentation() {
  using Stored = endian_store<T, E>;
  static_assert(std::is_trivially_copyable_v<Stored>);
  static_assert(sizeof(Stored) == sizeof(T));
  static_assert(alignof(Stored) == alignof(T));

  Stored original(T(1));
  std::array<unsigned char, sizeof(T)> expected{};
  T one = T(1);
  std::memcpy(expected.data(), &one, sizeof(one));
  if constexpr (std::endian::native != E) {
    std::reverse(expected.begin(), expected.end());
  }
  REQUIRE(std::memcmp(&original, expected.data(), sizeof(original)) == 0);
  REQUIRE(original.get() == T(1));

  // Copies preserve raw guest bytes, not just numerically equivalent values.
  // Exercise every bit, including floating-point sign and NaN payload bits.
  for (unsigned char fill : {0x00, 0xFF}) {
    for (size_t bit = 0; bit < sizeof(T) * 8; ++bit) {
      expected.fill(fill);
      expected[bit / 8] ^= static_cast<unsigned char>(1U << (bit % 8));
      std::memcpy(&original, expected.data(), sizeof(original));
      Stored copied(original);
      Stored assigned(T(0));
      assigned = original;
      REQUIRE(std::memcmp(&copied, expected.data(), sizeof(copied)) == 0);
      REQUIRE(std::memcmp(&assigned, expected.data(), sizeof(assigned)) == 0);
      expected[bit / 8] ^= static_cast<unsigned char>(1U << (bit % 8));
      REQUIRE(std::memcmp(&copied, expected.data(), sizeof(copied)) != 0);
    }
  }
}

TEMPLATE_TEST_CASE("Endian storage preserves guest representation",
                   "[byte_order]", uint8_t, uint16_t, uint32_t, uint64_t,
                   int16_t, int32_t, int64_t, float, double) {
  CheckEndianRepresentation<TestType, std::endian::big>();
  CheckEndianRepresentation<TestType, std::endian::little>();
}

}  // namespace xe::test
