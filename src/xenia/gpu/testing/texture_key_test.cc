#include "xenia/gpu/texture_cache.h"

#include <array>
#include <cstring>
#include <new>
#include <type_traits>

#include "third_party/catch/include/catch.hpp"

namespace xe::gpu::test {

struct TextureKeyAccess : TextureCache {
  using TextureCache::TextureKey;
};
using TextureKey = TextureKeyAccess::TextureKey;
static_assert(std::is_trivially_copyable_v<TextureKey>);

TEST_CASE("Texture key initializes its entire hashed representation",
          "[texture_key]") {
  alignas(TextureKey) std::array<unsigned char, sizeof(TextureKey)> storage;
  storage.fill(0xFF);
  auto* key = new (storage.data()) TextureKey;
  std::array<unsigned char, sizeof(TextureKey)> zero{};
  REQUIRE(std::memcmp(key, zero.data(), sizeof(*key)) == 0);

  key->base_page = 0x1FFFF;
  key->dimension = static_cast<xenos::DataDimension>(3);
  key->width_minus_1 = 0x1FFF;
  key->height_minus_1 = 0x1FFF;
  key->tiled = 1;
  key->packed_mips = 1;
  key->mip_page = 0x1FFFF;
  key->depth_or_array_size_minus_1 = 0x3FF;
  key->pitch = 0x1FF;
  key->mip_max_level = 0xF;
  key->format = static_cast<xenos::TextureFormat>(0x3F);
  key->endianness = static_cast<xenos::Endian>(3);
  key->signed_separate = 1;
  key->scaled_resolve = 1;
  key->is_valid = 1;
  const std::array<uint32_t, 4> expected{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 3};
  REQUIRE(std::memcmp(key, expected.data(), sizeof(*key)) == 0);
  key->MakeInvalid();
  REQUIRE(std::memcmp(key, zero.data(), sizeof(*key)) == 0);
  key->~TextureKey();
}

TEST_CASE("Texture key copies preserve every hashed bit", "[texture_key]") {
  for (size_t bit = 0; bit < sizeof(TextureKey) * 8; ++bit) {
    std::array<unsigned char, sizeof(TextureKey)> bytes{};
    bytes[bit / 8] = static_cast<unsigned char>(1U << (bit % 8));
    TextureKey original;
    std::memcpy(&original, bytes.data(), sizeof(original));
    const TextureKey copied(original);
    TextureKey assigned;
    assigned = original;
    REQUIRE(std::memcmp(&copied, bytes.data(), sizeof(copied)) == 0);
    REQUIRE(std::memcmp(&assigned, bytes.data(), sizeof(assigned)) == 0);
    REQUIRE(original == copied);
    REQUIRE(original == assigned);
    REQUIRE(TextureKey::Hasher{}(original) == TextureKey::Hasher{}(copied));
    REQUIRE(TextureKey::Hasher{}(original) == TextureKey::Hasher{}(assigned));
    const TextureKey zero;
    REQUIRE(original != zero);
    REQUIRE(TextureKey::Hasher{}(original) != TextureKey::Hasher{}(zero));
  }
}

}  // namespace xe::gpu::test
