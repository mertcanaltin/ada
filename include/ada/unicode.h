/**
 * @file unicode.h
 * @brief Definitions for all unicode specific functions.
 */
#ifndef ADA_UNICODE_H
#define ADA_UNICODE_H

#include "ada/common_defs.h"
#include <string>
#include <optional>

namespace ada::unicode {

  /**
   * We receive a UTF-8 string representing a domain name.
   * If the string is percent encoded, we apply percent decoding.
   *
   * Given a domain, we need to identify its labels.
   * They are separated by label-separators:
   *
   * U+002E ( . ) FULL STOP
   * U+FF0E ( ． ) FULLWIDTH FULL STOP
   * U+3002 ( 。 ) IDEOGRAPHIC FULL STOP
   * U+FF61 ( ｡ ) HALFWIDTH IDEOGRAPHIC FULL STOP
   *
   * They are all mapped to U+002E.
   *
   * We process each label into a string that should not exceed 63 octets.
   * If the string is already punycode (starts with "xn--"), then we must
   * scan it to look for unallowed code points.
   * Otherwise, if the string is not pure ASCII, we need to transcode it
   * to punycode by following RFC 3454 which requires us to
   * - Map characters  (see section 3),
   * - Normalize (see section 4),
   * - Reject forbidden characters,
   * - Check for right-to-left characters and if so, check all requirements (see section 6),
   * - Optionally reject based on unassigned code points (section 7).
   *
   * The Unicode standard provides a table of code points with a mapping, a list of
   * forbidden code points and so forth. This table is subject to change and will
   * vary based on the implementation. For Unicode 15, the table is at
   * https://www.unicode.org/Public/idna/15.0.0/IdnaMappingTable.txt
   * If you use ICU, they parse this table and map it to code using a Python script.
   *
   * The resulting strings should not exceed 255 octets according to RFC 1035 section 2.3.4.
   * ICU checks for label size and domain size, but if we pass "be_strict = false", these
   * errors are ignored.
   *
   * @see https://url.spec.whatwg.org/#concept-domain-to-ascii
   *
   */
  bool to_ascii(std::optional<std::string>& out, std::string_view plain, bool be_strict, size_t first_percent);

  /**
   * Checks if the input has tab or newline characters.
   *
   * @attention The has_tabs_or_newline function is a bottleneck and it is simple enough that compilers
   * like GCC can 'autovectorize it'.
   */
  ada_really_inline constexpr bool has_tabs_or_newline(std::string_view user_input) noexcept;

  /**
   * Checks if the input is a forbidden host code point.
   * @see https://url.spec.whatwg.org/#forbidden-host-code-point
   */
  ada_really_inline constexpr bool is_forbidden_host_code_point(const char c) noexcept;

  /**
   * Checks if the input is a forbidden doamin code point.
   * @see https://url.spec.whatwg.org/#forbidden-domain-code-point
   */
  ada_really_inline constexpr bool is_forbidden_domain_code_point(const char c) noexcept;

  /**
   * Checks if the input is alphanumeric, '+', '-' or '.'
   */
  ada_really_inline constexpr bool is_alnum_plus(const char c) noexcept;

  /**
   * @details An ASCII hex digit is an ASCII upper hex digit or ASCII lower hex digit.
   * An ASCII upper hex digit is an ASCII digit or a code point in the range U+0041 (A) to U+0046 (F), inclusive.
   * An ASCII lower hex digit is an ASCII digit or a code point in the range U+0061 (a) to U+0066 (f), inclusive.
   */
  ada_really_inline constexpr bool is_ascii_hex_digit(const char c) noexcept;

  /**
   * Checks if the input is a C0 control or space character.
   *
   * @details A C0 control or space is a C0 control or U+0020 SPACE.
   * A C0 control is a code point in the range U+0000 NULL to U+001F INFORMATION SEPARATOR ONE, inclusive.
   */
  ada_really_inline constexpr bool is_c0_control_or_space(const char c) noexcept;

  /**
   * Checks if the input is a ASCII tab or newline character.
   *
   * @details An ASCII tab or newline is U+0009 TAB, U+000A LF, or U+000D CR.
   */
  ada_really_inline constexpr bool is_ascii_tab_or_newline(const char c) noexcept;

  /**
   * @details A double-dot path segment must be ".." or an ASCII case-insensitive match for ".%2e", "%2e.", or "%2e%2e".
   */
  ada_really_inline ada_constexpr bool is_double_dot_path_segment(const std::string_view input) noexcept;

  /**
   * @details A single-dot path segment must be "." or an ASCII case-insensitive match for "%2e".
   */
  ada_really_inline constexpr bool is_single_dot_path_segment(const std::string_view input) noexcept;

  /**
   * @details ipv4 character might contain 0-9 or a-f character ranges.
   */
  ada_really_inline constexpr bool is_lowercase_hex(const char c) noexcept;

  unsigned constexpr convert_hex_to_binary(char c) noexcept;

  /**
   * first_percent should be  = input.find('%')
   *
   * @todo It would be faster as noexcept maybe, but it could be unsafe since.
   * @author Node.js
   * @see https://github.com/nodejs/node/blob/main/src/node_url.cc#L245
   * @see https://encoding.spec.whatwg.org/#utf-8-decode-without-bom
   */
  std::string percent_decode(const std::string_view input, size_t first_percent);

  /**
   * Returns a percent-encoding string whether percent encoding was needed or not.
   * @see https://github.com/nodejs/node/blob/main/src/node_url.cc#L226
   */
  std::string percent_encode(const std::string_view input, const uint8_t character_set[]);

  /**
   * Returns true if percent encoding was needed, in which case, we store
   * the percent-encoded content in 'out'. Otherwise, out is left unchanged.
   * @see https://github.com/nodejs/node/blob/main/src/node_url.cc#L226
   */
  bool percent_encode(const std::string_view input, const uint8_t character_set[], std::string& out);

} // namespace ada::unicode

#endif // ADA_UNICODE_H
