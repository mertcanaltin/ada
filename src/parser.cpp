#include "ada/parser.h"

#include <limits>

#include "ada/character_sets-inl.h"
#include "ada/common_defs.h"
#include "ada/log.h"
#include "ada/unicode.h"

namespace ada::parser {

template <class result_type, bool store_values>
result_type parse_url_impl(std::string_view user_input,
                           const result_type* base_url) {
  // We can specialize the implementation per type.
  // Important: result_type_is_ada_url is evaluated at *compile time*. This
  // means that doing if constexpr(result_type_is_ada_url) { something } else {
  // something else } is free (at runtime). This means that ada::url_aggregator
  // and ada::url **do not have to support the exact same API**.
  constexpr bool result_type_is_ada_url = std::is_same_v<url, result_type>;
  constexpr bool result_type_is_ada_url_aggregator =
      std::is_same_v<url_aggregator, result_type>;
  static_assert(result_type_is_ada_url ||
                result_type_is_ada_url_aggregator);  // We don't support
                                                     // anything else for now.

  ada_log("ada::parser::parse_url('", user_input, "' [", user_input.size(),
          " bytes],", (base_url != nullptr ? base_url->to_string() : "null"),
          ")");

  state state = state::SCHEME_START;
  result_type url{};

  // We refuse to parse URL strings that exceed 4GB. Such strings are almost
  // surely the result of a bug or are otherwise a security concern.
  if (user_input.size() > std::numeric_limits<uint32_t>::max()) [[unlikely]] {
    url.is_valid = false;
  }
  // Going forward, user_input.size() is in [0,
  // std::numeric_limits<uint32_t>::max). If we are provided with an invalid
  // base, or the optional_url was invalid, we must return.
  if (base_url != nullptr) {
    url.is_valid &= base_url->is_valid;
  }
  if (!url.is_valid) {
    return url;
  }
  if constexpr (result_type_is_ada_url_aggregator && store_values) {
    // Most of the time, we just need user_input.size().
    // In some instances, we may need a bit more.
    ///////////////////////////
    // This is *very* important. This line should *not* be removed
    // hastily. There are principled reasons why reserve is important
    // for performance. If you have a benchmark with small inputs,
    // it may not matter, but in other instances, it could.
    ////
    // This rounds up to the next power of two.
    // We know that user_input.size() is in [0,
    // std::numeric_limits<uint32_t>::max).
    uint32_t reserve_capacity =
        (0xFFFFFFFF >>
         helpers::leading_zeroes(uint32_t(1 | user_input.size()))) +
        1;
    url.reserve(reserve_capacity);
  }
  std::string tmp_buffer;
  std::string_view url_data;
  if (unicode::has_tabs_or_newline(user_input)) [[unlikely]] {
    tmp_buffer = user_input;
    // Optimization opportunity: Instead of copying and then pruning, we could
    // just directly build the string from user_input.
    helpers::remove_ascii_tab_or_newline(tmp_buffer);
    url_data = tmp_buffer;
  } else [[likely]] {
    url_data = user_input;
  }

  // Leading and trailing control characters are uncommon and easy to deal with
  // (no performance concern).
  helpers::trim_c0_whitespace(url_data);

  // Optimization opportunity. Most websites do not have fragment.
  std::optional<std::string_view> fragment = helpers::prune_hash(url_data);
  // We add it last so that an implementation like ada::url_aggregator
  // can append it last to its internal buffer, thus improving performance.

  // Here url_data no longer has its fragment.
  // We are going to access the data from url_data (it is immutable).
  // At any given time, we are pointing at byte 'input_position' in url_data.
  // The input_position variable should range from 0 to input_size.
  // It is illegal to access url_data at input_size.
  size_t input_position = 0;
  const size_t input_size = url_data.size();
  // Keep running the following state machine by switching on state.
  // If after a run pointer points to the EOF code point, go to the next step.
  // Otherwise, increase pointer by 1 and continue with the state machine.
  // We never decrement input_position.
  while (input_position <= input_size) {
    ada_log("In parsing at ", input_position, " out of ", input_size,
            " in state ", ada::to_string(state));
    switch (state) {
      case state::SCHEME_START: {
        ada_log("SCHEME_START ", helpers::substring(url_data, input_position));
        // If c is an ASCII alpha, append c, lowercased, to buffer, and set
        // state to scheme state.
        if ((input_position != input_size) &&
            checkers::is_alpha(url_data[input_position])) {
          state = state::SCHEME;
          input_position++;
        } else {
          // Otherwise, if state override is not given, set state to no scheme
          // state and decrease pointer by 1.
          state = state::NO_SCHEME;
        }
        break;
      }
      case state::SCHEME: {
        ada_log("SCHEME ", helpers::substring(url_data, input_position));
        // If c is an ASCII alphanumeric, U+002B (+), U+002D (-), or U+002E (.),
        // append c, lowercased, to buffer.
        while ((input_position != input_size) &&
               (unicode::is_alnum_plus(url_data[input_position]))) {
          input_position++;
        }
        // Otherwise, if c is U+003A (:), then:
        if ((input_position != input_size) &&
            (url_data[input_position] == ':')) {
          ada_log("SCHEME the scheme should be ",
                  url_data.substr(0, input_position));
          if constexpr (result_type_is_ada_url) {
            if (!url.parse_scheme(url_data.substr(0, input_position))) {
              return url;
            }
          } else {
            // we pass the colon along instead of painfully adding it back.
            if (!url.parse_scheme_with_colon(
                    url_data.substr(0, input_position + 1))) {
              return url;
            }
          }
          ada_log("SCHEME the scheme is ", url.get_protocol());

          // If url's scheme is "file", then:
          if (url.type == scheme::type::FILE) {
            // Set state to file state.
            state = state::FILE;
          }
          // Otherwise, if url is special, base is non-null, and base's scheme
          // is url's scheme: Note: Doing base_url->scheme is unsafe if base_url
          // != nullptr is false.
          else if (url.is_special() && base_url != nullptr &&
                   base_url->type == url.type) {
            // Set state to special relative or authority state.
            state = state::SPECIAL_RELATIVE_OR_AUTHORITY;
          }
          // Otherwise, if url is special, set state to special authority
          // slashes state.
          else if (url.is_special()) {
            state = state::SPECIAL_AUTHORITY_SLASHES;
          }
          // Otherwise, if remaining starts with an U+002F (/), set state to
          // path or authority state and increase pointer by 1.
          else if (input_position + 1 < input_size &&
                   url_data[input_position + 1] == '/') {
            state = state::PATH_OR_AUTHORITY;
            input_position++;
          }
          // Otherwise, set url's path to the empty string and set state to
          // opaque path state.
          else {
            state = state::OPAQUE_PATH;
          }
        }
        // Otherwise, if state override is not given, set buffer to the empty
        // string, state to no scheme state, and start over (from the first code
        // point in input).
        else {
          state = state::NO_SCHEME;
          input_position = 0;
          break;
        }
        input_position++;
        break;
      }
      case state::NO_SCHEME: {
        ada_log("NO_SCHEME ", helpers::substring(url_data, input_position));
        // If base is null, or base has an opaque path and c is not U+0023 (#),
        // validation error, return failure.
        if (base_url == nullptr ||
            (base_url->has_opaque_path && !fragment.has_value())) {
          ada_log("NO_SCHEME validation error");
          url.is_valid = false;
          return url;
        }
        // Otherwise, if base has an opaque path and c is U+0023 (#),
        // set url's scheme to base's scheme, url's path to base's path, url's
        // query to base's query, and set state to fragment state.
        else if (base_url->has_opaque_path && fragment.has_value() &&
                 input_position == input_size) {
          ada_log("NO_SCHEME opaque base with fragment");
          url.copy_scheme(*base_url);
          url.has_opaque_path = base_url->has_opaque_path;

          if constexpr (result_type_is_ada_url) {
            url.path = base_url->path;
            url.query = base_url->query;
          } else {
            url.update_base_pathname(base_url->get_pathname());
            url.update_base_search(base_url->get_search());
          }
          url.update_unencoded_base_hash(*fragment);
          return url;
        }
        // Otherwise, if base's scheme is not "file", set state to relative
        // state and decrease pointer by 1.
        else if (base_url->type != scheme::type::FILE) {
          ada_log("NO_SCHEME non-file relative path");
          state = state::RELATIVE_SCHEME;
        }
        // Otherwise, set state to file state and decrease pointer by 1.
        else {
          ada_log("NO_SCHEME file base type");
          state = state::FILE;
        }
        break;
      }
      case state::AUTHORITY: {
        ada_log("AUTHORITY ", helpers::substring(url_data, input_position));
        // most URLs have no @. Having no @ tells us that we don't have to worry
        // about AUTHORITY. Of course, we could have @ and still not have to
        // worry about AUTHORITY.
        // TODO: Instead of just collecting a bool, collect the location of the
        // '@' and do something useful with it.
        // TODO: We could do various processing early on, using a single pass
        // over the string to collect information about it, e.g., telling us
        // whether there is a @ and if so, where (or how many).

        // Check if url data contains an @.
        if (url_data.find('@', input_position) == std::string_view::npos) {
          state = state::HOST;
          break;
        }
        bool at_sign_seen{false};
        bool password_token_seen{false};
        /**
         * We expect something of the sort...
         * https://user:pass@example.com:1234/foo/bar?baz#quux
         * --------^
         */
        do {
          std::string_view view = url_data.substr(input_position);
          // The delimiters are @, /, ? \\.
          size_t location =
              url.is_special() ? helpers::find_authority_delimiter_special(view)
                               : helpers::find_authority_delimiter(view);
          std::string_view authority_view = view.substr(0, location);
          size_t end_of_authority = input_position + authority_view.size();
          // If c is U+0040 (@), then:
          if ((end_of_authority != input_size) &&
              (url_data[end_of_authority] == '@')) {
            // If atSignSeen is true, then prepend "%40" to buffer.
            if (at_sign_seen) {
              if (password_token_seen) {
                if constexpr (result_type_is_ada_url) {
                  url.password += "%40";
                } else {
                  url.append_base_password("%40");
                }
              } else {
                if constexpr (result_type_is_ada_url) {
                  url.username += "%40";
                } else {
                  url.append_base_username("%40");
                }
              }
            }

            at_sign_seen = true;

            if (!password_token_seen) {
              size_t password_token_location = authority_view.find(':');
              password_token_seen =
                  password_token_location != std::string_view::npos;

              if constexpr (store_values) {
                if (!password_token_seen) {
                  if constexpr (result_type_is_ada_url) {
                    url.username += unicode::percent_encode(
                        authority_view,
                        character_sets::USERINFO_PERCENT_ENCODE);
                  } else {
                    url.append_base_username(unicode::percent_encode(
                        authority_view,
                        character_sets::USERINFO_PERCENT_ENCODE));
                  }
                } else {
                  if constexpr (result_type_is_ada_url) {
                    url.username += unicode::percent_encode(
                        authority_view.substr(0, password_token_location),
                        character_sets::USERINFO_PERCENT_ENCODE);
                    url.password += unicode::percent_encode(
                        authority_view.substr(password_token_location + 1),
                        character_sets::USERINFO_PERCENT_ENCODE);
                  } else {
                    url.append_base_username(unicode::percent_encode(
                        authority_view.substr(0, password_token_location),
                        character_sets::USERINFO_PERCENT_ENCODE));
                    url.append_base_password(unicode::percent_encode(
                        authority_view.substr(password_token_location + 1),
                        character_sets::USERINFO_PERCENT_ENCODE));
                  }
                }
              }
            } else if constexpr (store_values) {
              if constexpr (result_type_is_ada_url) {
                url.password += unicode::percent_encode(
                    authority_view, character_sets::USERINFO_PERCENT_ENCODE);
              } else {
                url.append_base_password(unicode::percent_encode(
                    authority_view, character_sets::USERINFO_PERCENT_ENCODE));
              }
            }
          }
          // Otherwise, if one of the following is true:
          // - c is the EOF code point, U+002F (/), U+003F (?), or U+0023 (#)
          // - url is special and c is U+005C (\)
          else if (end_of_authority == input_size ||
                   url_data[end_of_authority] == '/' ||
                   url_data[end_of_authority] == '?' ||
                   (url.is_special() && url_data[end_of_authority] == '\\')) {
            // If atSignSeen is true and authority_view is the empty string,
            // validation error, return failure.
            if (at_sign_seen && authority_view.empty()) {
              url.is_valid = false;
              return url;
            }
            state = state::HOST;
            break;
          }
          if (end_of_authority == input_size) {
            if constexpr (store_values) {
              if (fragment.has_value()) {
                url.update_unencoded_base_hash(*fragment);
              }
            }
            return url;
          }
          input_position = end_of_authority + 1;
        } while (true);

        break;
      }
      case state::SPECIAL_RELATIVE_OR_AUTHORITY: {
        ada_log("SPECIAL_RELATIVE_OR_AUTHORITY ",
                helpers::substring(url_data, input_position));

        // If c is U+002F (/) and remaining starts with U+002F (/),
        // then set state to special authority ignore slashes state and increase
        // pointer by 1.
        if (url_data.substr(input_position, 2) == "//") {
          state = state::SPECIAL_AUTHORITY_IGNORE_SLASHES;
          input_position += 2;
        } else {
          // Otherwise, validation error, set state to relative state and
          // decrease pointer by 1.
          state = state::RELATIVE_SCHEME;
        }

        break;
      }
      case state::PATH_OR_AUTHORITY: {
        ada_log("PATH_OR_AUTHORITY ",
                helpers::substring(url_data, input_position));

        // If c is U+002F (/), then set state to authority state.
        if ((input_position != input_size) &&
            (url_data[input_position] == '/')) {
          state = state::AUTHORITY;
          input_position++;
        } else {
          // Otherwise, set state to path state, and decrease pointer by 1.
          state = state::PATH;
        }

        break;
      }
      case state::RELATIVE_SCHEME: {
        ada_log("RELATIVE_SCHEME ",
                helpers::substring(url_data, input_position));

        // Set url's scheme to base's scheme.
        url.copy_scheme(*base_url);

        // If c is U+002F (/), then set state to relative slash state.
        if ((input_position != input_size) &&
            (url_data[input_position] == '/')) {
          ada_log(
              "RELATIVE_SCHEME if c is U+002F (/), then set state to relative "
              "slash state");
          state = state::RELATIVE_SLASH;
        } else if (url.is_special() && (input_position != input_size) &&
                   (url_data[input_position] == '\\')) {
          // Otherwise, if url is special and c is U+005C (\), validation error,
          // set state to relative slash state.
          ada_log(
              "RELATIVE_SCHEME  if url is special and c is U+005C, validation "
              "error, set state to relative slash state");
          state = state::RELATIVE_SLASH;
        } else {
          ada_log("RELATIVE_SCHEME otherwise");
          // Set url's username to base's username, url's password to base's
          // password, url's host to base's host, url's port to base's port,
          // url's path to a clone of base's path, and url's query to base's
          // query.
          if constexpr (result_type_is_ada_url) {
            url.username = base_url->username;
            url.password = base_url->password;
            url.host = base_url->host;
            url.port = base_url->port;
            // cloning the base path includes cloning the has_opaque_path flag
            url.has_opaque_path = base_url->has_opaque_path;
            url.path = base_url->path;
            url.query = base_url->query;
          } else {
            url.update_base_authority(base_url->get_href(),
                                      base_url->get_components());
            url.update_host_to_base_host(base_url->get_hostname());
            url.update_base_port(base_url->retrieve_base_port());
            // cloning the base path includes cloning the has_opaque_path flag
            url.has_opaque_path = base_url->has_opaque_path;
            url.update_base_pathname(base_url->get_pathname());
            url.update_base_search(base_url->get_search());
          }

          url.has_opaque_path = base_url->has_opaque_path;

          // If c is U+003F (?), then set url's query to the empty string, and
          // state to query state.
          if ((input_position != input_size) &&
              (url_data[input_position] == '?')) {
            state = state::QUERY;
          }
          // Otherwise, if c is not the EOF code point:
          else if (input_position != input_size) {
            // Set url's query to null.
            url.clear_search();
            if constexpr (result_type_is_ada_url) {
              // Shorten url's path.
              helpers::shorten_path(url.path, url.type);
            } else {
              std::string_view path = url.get_pathname();
              if (helpers::shorten_path(path, url.type)) {
                url.update_base_pathname(std::move(std::string(path)));
              }
            }
            // Set state to path state and decrease pointer by 1.
            state = state::PATH;
            break;
          }
        }
        input_position++;
        break;
      }
      case state::RELATIVE_SLASH: {
        ada_log("RELATIVE_SLASH ",
                helpers::substring(url_data, input_position));

        // If url is special and c is U+002F (/) or U+005C (\), then:
        if (url.is_special() && (input_position != input_size) &&
            (url_data[input_position] == '/' ||
             url_data[input_position] == '\\')) {
          // Set state to special authority ignore slashes state.
          state = state::SPECIAL_AUTHORITY_IGNORE_SLASHES;
        }
        // Otherwise, if c is U+002F (/), then set state to authority state.
        else if ((input_position != input_size) &&
                 (url_data[input_position] == '/')) {
          state = state::AUTHORITY;
        }
        // Otherwise, set
        // - url's username to base's username,
        // - url's password to base's password,
        // - url's host to base's host,
        // - url's port to base's port,
        // - state to path state, and then, decrease pointer by 1.
        else {
          if constexpr (result_type_is_ada_url) {
            url.username = base_url->username;
            url.password = base_url->password;
            url.host = base_url->host;
            url.port = base_url->port;
          } else {
            url.update_base_authority(base_url->get_href(),
                                      base_url->get_components());
            url.update_host_to_base_host(base_url->get_hostname());
            url.update_base_port(base_url->retrieve_base_port());
          }
          state = state::PATH;
          break;
        }

        input_position++;
        break;
      }
      case state::SPECIAL_AUTHORITY_SLASHES: {
        ada_log("SPECIAL_AUTHORITY_SLASHES ",
                helpers::substring(url_data, input_position));

        // If c is U+002F (/) and remaining starts with U+002F (/),
        // then set state to special authority ignore slashes state and increase
        // pointer by 1.
        if (url_data.substr(input_position, 2) == "//") {
          input_position += 2;
        }

        [[fallthrough]];
      }
      case state::SPECIAL_AUTHORITY_IGNORE_SLASHES: {
        ada_log("SPECIAL_AUTHORITY_IGNORE_SLASHES ",
                helpers::substring(url_data, input_position));

        // If c is neither U+002F (/) nor U+005C (\), then set state to
        // authority state and decrease pointer by 1.
        while ((input_position != input_size) &&
               ((url_data[input_position] == '/') ||
                (url_data[input_position] == '\\'))) {
          input_position++;
        }
        state = state::AUTHORITY;

        break;
      }
      case state::QUERY: {
        ada_log("QUERY ", helpers::substring(url_data, input_position));
        if constexpr (store_values) {
          // Let queryPercentEncodeSet be the special-query percent-encode set
          // if url is special; otherwise the query percent-encode set.
          const uint8_t* query_percent_encode_set =
              url.is_special() ? character_sets::SPECIAL_QUERY_PERCENT_ENCODE
                               : character_sets::QUERY_PERCENT_ENCODE;

          // Percent-encode after encoding, with encoding, buffer, and
          // queryPercentEncodeSet, and append the result to url's query.
          url.update_base_search(url_data.substr(input_position),
                                 query_percent_encode_set);
          ada_log("QUERY update_base_search completed ");
          if (fragment.has_value()) {
            url.update_unencoded_base_hash(*fragment);
          }
        }
        return url;
      }
      case state::HOST: {
        ada_log("HOST ", helpers::substring(url_data, input_position));

        std::string_view host_view = url_data.substr(input_position);
        auto [location, found_colon] =
            helpers::get_host_delimiter_location(url.is_special(), host_view);
        input_position = (location != std::string_view::npos)
                             ? input_position + location
                             : input_size;
        // Otherwise, if c is U+003A (:) and insideBrackets is false, then:
        // Note: the 'found_colon' value is true if and only if a colon was
        // encountered while not inside brackets.
        if (found_colon) {
          // If buffer is the empty string, validation error, return failure.
          // Let host be the result of host parsing buffer with url is not
          // special.
          ada_log("HOST parsing ", host_view);
          if (!url.parse_host(host_view)) {
            return url;
          }
          ada_log("HOST parsing results in ", url.get_hostname());
          // Set url's host to host, buffer to the empty string, and state to
          // port state.
          state = state::PORT;
          input_position++;
        }
        // Otherwise, if one of the following is true:
        // - c is the EOF code point, U+002F (/), U+003F (?), or U+0023 (#)
        // - url is special and c is U+005C (\)
        // The get_host_delimiter_location function either brings us to
        // the colon outside of the bracket, or to one of those characters.
        else {
          // If url is special and host_view is the empty string, validation
          // error, return failure.
          if (host_view.empty() && url.is_special()) {
            url.is_valid = false;
            return url;
          }
          ada_log("HOST parsing ", host_view, " href=", url.get_href());
          // Let host be the result of host parsing host_view with url is not
          // special.
          if (host_view.empty()) {
            url.update_base_hostname("");
          } else if (!url.parse_host(host_view)) {
            return url;
          }
          ada_log("HOST parsing results in ", url.get_hostname(),
                  " href=", url.get_href());

          // Set url's host to host, and state to path start state.
          state = state::PATH_START;
        }

        break;
      }
      case state::OPAQUE_PATH: {
        ada_log("OPAQUE_PATH ", helpers::substring(url_data, input_position));
        std::string_view view = url_data.substr(input_position);
        // If c is U+003F (?), then set url's query to the empty string and
        // state to query state.
        size_t location = view.find('?');
        if (location != std::string_view::npos) {
          view.remove_suffix(view.size() - location);
          state = state::QUERY;
          input_position += location + 1;
        } else {
          input_position = input_size + 1;
        }
        url.has_opaque_path = true;
        // This is a really unlikely scenario in real world. We should not seek
        // to optimize it.
        url.update_base_pathname(unicode::percent_encode(
            view, character_sets::C0_CONTROL_PERCENT_ENCODE));
        break;
      }
      case state::PORT: {
        ada_log("PORT ", helpers::substring(url_data, input_position));
        std::string_view port_view = url_data.substr(input_position);
        input_position += url.parse_port(port_view, true);
        if (!url.is_valid) {
          return url;
        }
        state = state::PATH_START;
        [[fallthrough]];
      }
      case state::PATH_START: {
        ada_log("PATH_START ", helpers::substring(url_data, input_position));

        // If url is special, then:
        if (url.is_special()) {
          // Set state to path state.
          state = state::PATH;

          // Optimization: Avoiding going into PATH state improves the
          // performance of urls ending with /.
          if (input_position == input_size) {
            if constexpr (store_values) {
              url.update_base_pathname("/");
              if (fragment.has_value()) {
                url.update_unencoded_base_hash(*fragment);
              }
            }
            return url;
          }
          // If c is neither U+002F (/) nor U+005C (\), then decrease pointer
          // by 1. We know that (input_position == input_size) is impossible
          // here, because of the previous if-check.
          if ((url_data[input_position] != '/') &&
              (url_data[input_position] != '\\')) {
            break;
          }
        }
        // Otherwise, if state override is not given and c is U+003F (?),
        // set url's query to the empty string and state to query state.
        else if ((input_position != input_size) &&
                 (url_data[input_position] == '?')) {
          state = state::QUERY;
        }
        // Otherwise, if c is not the EOF code point:
        else if (input_position != input_size) {
          // Set state to path state.
          state = state::PATH;

          // If c is not U+002F (/), then decrease pointer by 1.
          if (url_data[input_position] != '/') {
            break;
          }
        }

        input_position++;
        break;
      }
      case state::PATH: {
        ada_log("PATH ", helpers::substring(url_data, input_position));
        std::string_view view = url_data.substr(input_position);

        // Most time, we do not need percent encoding.
        // Furthermore, we can immediately locate the '?'.
        size_t locofquestionmark = view.find('?');
        if (locofquestionmark != std::string_view::npos) {
          state = state::QUERY;
          view.remove_suffix(view.size() - locofquestionmark);
          input_position += locofquestionmark + 1;
        } else {
          input_position = input_size + 1;
        }
        if constexpr (store_values) {
          if constexpr (result_type_is_ada_url) {
            helpers::parse_prepared_path(view, url.type, url.path);
          } else {
            url.consume_prepared_path(view);
            ADA_ASSERT_TRUE(url.validate());
          }
        }
        break;
      }
      case state::FILE_SLASH: {
        ada_log("FILE_SLASH ", helpers::substring(url_data, input_position));

        // If c is U+002F (/) or U+005C (\), then:
        if ((input_position != input_size) &&
            (url_data[input_position] == '/' ||
             url_data[input_position] == '\\')) {
          ada_log("FILE_SLASH c is U+002F or U+005C");
          // Set state to file host state.
          state = state::FILE_HOST;
          input_position++;
        } else {
          ada_log("FILE_SLASH otherwise");
          // If base is non-null and base's scheme is "file", then:
          // Note: it is unsafe to do base_url->scheme unless you know that
          // base_url_has_value() is true.
          if (base_url != nullptr && base_url->type == scheme::type::FILE) {
            // Set url's host to base's host.
            if constexpr (result_type_is_ada_url) {
              url.host = base_url->host;
            } else {
              url.update_host_to_base_host(base_url->get_host());
            }
            // If the code point substring from pointer to the end of input does
            // not start with a Windows drive letter and base's path[0] is a
            // normalized Windows drive letter, then append base's path[0] to
            // url's path.
            if (!base_url->get_pathname().empty()) {
              if (!checkers::is_windows_drive_letter(
                      url_data.substr(input_position))) {
                std::string_view first_base_url_path =
                    base_url->get_pathname().substr(1);
                size_t loc = first_base_url_path.find('/');
                if (loc != std::string_view::npos) {
                  helpers::resize(first_base_url_path, loc);
                }
                if (checkers::is_normalized_windows_drive_letter(
                        first_base_url_path)) {
                  if constexpr (result_type_is_ada_url) {
                    url.path += '/';
                    url.path += first_base_url_path;
                  } else {
                    url.append_base_pathname(
                        helpers::concat("/", first_base_url_path));
                  }
                }
              }
            }
          }

          // Set state to path state, and decrease pointer by 1.
          state = state::PATH;
        }

        break;
      }
      case state::FILE_HOST: {
        ada_log("FILE_HOST ", helpers::substring(url_data, input_position));
        std::string_view view = url_data.substr(input_position);

        size_t location = view.find_first_of("/\\?");
        std::string_view file_host_buffer(
            view.data(),
            (location != std::string_view::npos) ? location : view.size());

        if (checkers::is_windows_drive_letter(file_host_buffer)) {
          state = state::PATH;
        } else if (file_host_buffer.empty()) {
          // Set url's host to the empty string.
          if constexpr (result_type_is_ada_url) {
            url.host = "";
          } else {
            url.update_base_hostname("");
          }
          // Set state to path start state.
          state = state::PATH_START;
        } else {
          size_t consumed_bytes = file_host_buffer.size();
          input_position += consumed_bytes;
          // Let host be the result of host parsing buffer with url is not
          // special.
          if (!url.parse_host(file_host_buffer)) {
            return url;
          }

          if constexpr (result_type_is_ada_url) {
            // If host is "localhost", then set host to the empty string.
            if (url.host.has_value() && url.host.value() == "localhost") {
              url.host = "";
            }
          } else {
            if (url.get_hostname() == "localhost") {
              url.update_base_hostname("");
            }
          }

          // Set buffer to the empty string and state to path start state.
          state = state::PATH_START;
        }

        break;
      }
      case state::FILE: {
        ada_log("FILE ", helpers::substring(url_data, input_position));
        std::string_view file_view = url_data.substr(input_position);

        url.set_protocol_as_file();
        if constexpr (result_type_is_ada_url) {
          // Set url's host to the empty string.
          url.host = "";
        } else {
          url.update_base_hostname("");
        }
        // If c is U+002F (/) or U+005C (\), then:
        if (input_position != input_size &&
            (url_data[input_position] == '/' ||
             url_data[input_position] == '\\')) {
          ada_log("FILE c is U+002F or U+005C");
          // Set state to file slash state.
          state = state::FILE_SLASH;
        }
        // Otherwise, if base is non-null and base's scheme is "file":
        else if (base_url != nullptr && base_url->type == scheme::type::FILE) {
          // Set url's host to base's host, url's path to a clone of base's
          // path, and url's query to base's query.
          ada_log("FILE base non-null");
          if constexpr (result_type_is_ada_url) {
            url.host = base_url->host;
            url.path = base_url->path;
            url.query = base_url->query;
          } else {
            url.update_host_to_base_host(base_url->get_hostname());
            url.update_base_pathname(base_url->get_pathname());
            url.update_base_search(base_url->get_search());
          }
          url.has_opaque_path = base_url->has_opaque_path;

          // If c is U+003F (?), then set url's query to the empty string and
          // state to query state.
          if (input_position != input_size && url_data[input_position] == '?') {
            state = state::QUERY;
          }
          // Otherwise, if c is not the EOF code point:
          else if (input_position != input_size) {
            // Set url's query to null.
            url.clear_search();
            // If the code point substring from pointer to the end of input does
            // not start with a Windows drive letter, then shorten url's path.
            if (!checkers::is_windows_drive_letter(file_view)) {
              if constexpr (result_type_is_ada_url) {
                helpers::shorten_path(url.path, url.type);
              } else {
                std::string_view path = url.get_pathname();
                if (helpers::shorten_path(path, url.type)) {
                  url.update_base_pathname(std::move(std::string(path)));
                }
              }
            }
            // Otherwise:
            else {
              // Set url's path to an empty list.
              url.clear_pathname();
              url.has_opaque_path = true;
            }

            // Set state to path state and decrease pointer by 1.
            state = state::PATH;
            break;
          }
        }
        // Otherwise, set state to path state, and decrease pointer by 1.
        else {
          ada_log("FILE go to path");
          state = state::PATH;
          break;
        }

        input_position++;
        break;
      }
      default:
        unreachable();
    }
  }
  if constexpr (store_values) {
    if (fragment.has_value()) {
      url.update_unencoded_base_hash(*fragment);
    }
  }
  return url;
}

tl::expected<url_pattern, errors> parse_url_pattern_impl(
    std::variant<std::string_view, url_pattern_init> input,
    const std::string_view* base_url, const url_pattern_options* options) {
  // Let init be null.
  url_pattern_init init;

  // If input is a scalar value string then:
  if (std::holds_alternative<std::string_view>(input)) {
    // Set init to the result of running parse a constructor string given input.
    auto parse_result = url_pattern_helpers::constructor_string_parser::parse(
        std::get<std::string_view>(input));
    if (!parse_result) {
      ada_log("constructor_string_parser::parse failed");
      return tl::unexpected(parse_result.error());
    }
    init = std::move(*parse_result);
    // If baseURL is null and init["protocol"] does not exist, then throw a
    // TypeError.
    if (!base_url && !init.protocol) {
      ada_log("base url is null and protocol is not set");
      return tl::unexpected(errors::type_error);
    }

    // If baseURL is not null, set init["baseURL"] to baseURL.
    if (base_url) {
      init.base_url = std::string(*base_url);
    }
  } else {
    // Assert: input is a URLPatternInit.
    ADA_ASSERT_TRUE(std::holds_alternative<url_pattern_init>(input));
    // If baseURL is not null, then throw a TypeError.
    if (base_url) {
      ada_log("base url is not null");
      return tl::unexpected(errors::type_error);
    }
    // Optimization: Avoid copy by moving the input value.
    // Set init to input.
    init = std::move(std::get<url_pattern_init>(input));
  }

  // Let processedInit be the result of process a URLPatternInit given init,
  // "pattern", null, null, null, null, null, null, null, and null.
  // TODO: Make "pattern" an enum to avoid creating a string everytime.
  auto processed_init = url_pattern_init::process(init, "pattern");
  if (!processed_init) {
    ada_log("url_pattern_init::process failed for init and 'pattern'");
    return tl::unexpected(processed_init.error());
  }

  // For each componentName of « "protocol", "username", "password", "hostname",
  // "port", "pathname", "search", "hash" If processedInit[componentName] does
  // not exist, then set processedInit[componentName] to "*".
  ADA_ASSERT_TRUE(processed_init.has_value());
  if (!processed_init->protocol) processed_init->protocol = "*";
  if (!processed_init->username) processed_init->username = "*";
  if (!processed_init->password) processed_init->password = "*";
  if (!processed_init->hostname) processed_init->hostname = "*";
  if (!processed_init->port) processed_init->port = "*";
  if (!processed_init->pathname) processed_init->pathname = "*";
  if (!processed_init->search) processed_init->search = "*";
  if (!processed_init->hash) processed_init->hash = "*";

  ada_log("-- processed_init->protocol: ", processed_init->protocol.value());
  ada_log("-- processed_init->username: ", processed_init->username.value());
  ada_log("-- processed_init->password: ", processed_init->password.value());
  ada_log("-- processed_init->hostname: ", processed_init->hostname.value());
  ada_log("-- processed_init->port: ", processed_init->port.value());
  ada_log("-- processed_init->pathname: ", processed_init->pathname.value());
  ada_log("-- processed_init->search: ", processed_init->search.value());
  ada_log("-- processed_init->hash: ", processed_init->hash.value());

  // If processedInit["protocol"] is a special scheme and processedInit["port"]
  // is a string which represents its corresponding default port in radix-10
  // using ASCII digits then set processedInit["port"] to the empty string.
  // TODO: Optimization opportunity.
  if (scheme::is_special(*processed_init->protocol)) {
    std::string_view port = processed_init->port.value();
    helpers::trim_c0_whitespace(port);
    if (std::to_string(scheme::get_special_port(*processed_init->protocol)) ==
        port) {
      processed_init->port->clear();
    }
  }

  // Let urlPattern be a new URL pattern.
  auto url_pattern_ = url_pattern{};

  // Set urlPattern’s protocol component to the result of compiling a component
  // given processedInit["protocol"], canonicalize a protocol, and default
  // options.
  auto protocol_component = url_pattern_component::compile(
      processed_init->protocol.value(),
      url_pattern_helpers::canonicalize_protocol,
      url_pattern_compile_component_options::DEFAULT);
  if (!protocol_component) {
    ada_log("url_pattern_component::compile failed for protocol ",
            processed_init->protocol.value());
    return tl::unexpected(protocol_component.error());
  }
  url_pattern_.protocol_component = std::move(*protocol_component);

  // Set urlPattern’s username component to the result of compiling a component
  // given processedInit["username"], canonicalize a username, and default
  // options.
  auto username_component = url_pattern_component::compile(
      processed_init->username.value(),
      url_pattern_helpers::canonicalize_username,
      url_pattern_compile_component_options::DEFAULT);
  if (!username_component) {
    ada_log("url_pattern_component::compile failed for username ",
            processed_init->username.value());
    return tl::unexpected(username_component.error());
  }
  url_pattern_.username_component = std::move(*username_component);

  // Set urlPattern’s password component to the result of compiling a component
  // given processedInit["password"], canonicalize a password, and default
  // options.
  auto password_component = url_pattern_component::compile(
      processed_init->password.value(),
      url_pattern_helpers::canonicalize_password,
      url_pattern_compile_component_options::DEFAULT);
  if (!password_component) {
    ada_log("url_pattern_component::compile failed for password ",
            processed_init->password.value());
    return tl::unexpected(password_component.error());
  }
  url_pattern_.password_component = std::move(*password_component);

  // TODO: Optimization opportunity. The following if statement can be
  // simplified.
  // If the result running hostname pattern is an IPv6 address given
  // processedInit["hostname"] is true, then set urlPattern’s hostname component
  // to the result of compiling a component given processedInit["hostname"],
  // canonicalize an IPv6 hostname, and hostname options.
  if (url_pattern_helpers::is_ipv6_address(processed_init->hostname.value())) {
    ada_log("processed_init->hostname is ipv6 address");
    // then set urlPattern’s hostname component to the result of compiling a
    // component given processedInit["hostname"], canonicalize an IPv6 hostname,
    // and hostname options.
    auto hostname_component = url_pattern_component::compile(
        processed_init->hostname.value(),
        url_pattern_helpers::canonicalize_ipv6_hostname,
        url_pattern_compile_component_options::DEFAULT);
    if (!hostname_component) {
      ada_log("url_pattern_component::compile failed for ipv6 hostname ",
              processed_init->hostname.value());
      return tl::unexpected(hostname_component.error());
    }
    url_pattern_.hostname_component = std::move(*hostname_component);
  } else {
    // Otherwise, set urlPattern’s hostname component to the result of compiling
    // a component given processedInit["hostname"], canonicalize a hostname, and
    // hostname options.
    auto hostname_component = url_pattern_component::compile(
        processed_init->hostname.value(),
        url_pattern_helpers::canonicalize_hostname,
        url_pattern_compile_component_options::HOSTNAME);
    if (!hostname_component) {
      ada_log("url_pattern_component::compile failed for hostname ",
              processed_init->hostname.value());
      return tl::unexpected(hostname_component.error());
    }
    url_pattern_.hostname_component = std::move(*hostname_component);
  }

  // Set urlPattern’s port component to the result of compiling a component
  // given processedInit["port"], canonicalize a port, and default options.
  auto port_component = url_pattern_component::compile(
      processed_init->port.value(), url_pattern_helpers::canonicalize_port,
      url_pattern_compile_component_options::DEFAULT);
  if (!port_component) {
    ada_log("url_pattern_component::compile failed for port ",
            processed_init->port.value());
    return tl::unexpected(port_component.error());
  }
  url_pattern_.port_component = std::move(*port_component);

  // Let compileOptions be a copy of the default options with the ignore case
  // property set to options["ignoreCase"].
  auto compile_options = url_pattern_compile_component_options::DEFAULT;
  if (options) {
    compile_options.ignore_case = options->ignore_case;
  }

  // TODO: Optimization opportunity: Simplify this if statement.
  // If the result of running protocol component matches a special scheme given
  // urlPattern’s protocol component is true, then:
  if (url_pattern_helpers::protocol_component_matches_special_scheme(
          url_pattern_.protocol_component)) {
    // Let pathCompileOptions be copy of the pathname options with the ignore
    // case property set to options["ignoreCase"].
    auto path_compile_options = url_pattern_compile_component_options::PATHNAME;
    if (options) {
      path_compile_options.ignore_case = options->ignore_case;
    }

    // Set urlPattern’s pathname component to the result of compiling a
    // component given processedInit["pathname"], canonicalize a pathname, and
    // pathCompileOptions.
    auto pathname_component = url_pattern_component::compile(
        processed_init->pathname.value(),
        url_pattern_helpers::canonicalize_pathname, path_compile_options);
    if (!pathname_component) {
      ada_log("url_pattern_component::compile failed for pathname ",
              processed_init->pathname.value());
      return tl::unexpected(pathname_component.error());
    }
    url_pattern_.pathname_component = std::move(*pathname_component);
  } else {
    // Otherwise set urlPattern’s pathname component to the result of compiling
    // a component given processedInit["pathname"], canonicalize an opaque
    // pathname, and compileOptions.
    auto pathname_component = url_pattern_component::compile(
        processed_init->pathname.value(),
        url_pattern_helpers::canonicalize_opaque_pathname, compile_options);
    if (!pathname_component) {
      ada_log("url_pattern_component::compile failed for opaque pathname ",
              processed_init->pathname.value());
      return tl::unexpected(pathname_component.error());
    }
    url_pattern_.pathname_component = std::move(*pathname_component);
  }

  // Set urlPattern’s search component to the result of compiling a component
  // given processedInit["search"], canonicalize a search, and compileOptions.
  auto search_component = url_pattern_component::compile(
      processed_init->search.value(), url_pattern_helpers::canonicalize_search,
      compile_options);
  if (!search_component) {
    ada_log("url_pattern_component::compile failed for search ",
            processed_init->search.value());
    return tl::unexpected(search_component.error());
  }
  url_pattern_.search_component = std::move(*search_component);

  // Set urlPattern’s hash component to the result of compiling a component
  // given processedInit["hash"], canonicalize a hash, and compileOptions.
  auto hash_component = url_pattern_component::compile(
      processed_init->hash.value(), url_pattern_helpers::canonicalize_hash,
      compile_options);
  if (!hash_component) {
    ada_log("url_pattern_component::compile failed for hash ",
            processed_init->hash.value());
    return tl::unexpected(hash_component.error());
  }
  url_pattern_.hash_component = std::move(*hash_component);

  // Return urlPattern.
  return url_pattern_;
}

template url parse_url_impl(std::string_view user_input,
                            const url* base_url = nullptr);
template url_aggregator parse_url_impl(
    std::string_view user_input, const url_aggregator* base_url = nullptr);

template <class result_type>
result_type parse_url(std::string_view user_input,
                      const result_type* base_url) {
  return parse_url_impl<result_type, true>(user_input, base_url);
}

template url parse_url<url>(std::string_view user_input,
                            const url* base_url = nullptr);
template url_aggregator parse_url<url_aggregator>(
    std::string_view user_input, const url_aggregator* base_url = nullptr);
}  // namespace ada::parser
