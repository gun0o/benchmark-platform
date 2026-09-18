#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bench {

std::array<std::uint8_t, 32> sha256(std::span<const std::byte> data) noexcept;
std::string sha256_hex(std::string_view text);

} // namespace bench
