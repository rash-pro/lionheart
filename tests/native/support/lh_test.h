// Native test framework. See tests/README.md for how to use it.
//
// Case code runs in a host process and treats everything it starts as an
// outside program, so the same case can drive a Windows harness under Wine.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lh {

namespace fs = std::filesystem;

enum class Tier { standard, game, realtime };

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
struct Skip : std::runtime_error { using std::runtime_error::runtime_error; };

[[noreturn]] void fail_at(const char *file, int line, const std::string &message);
[[noreturn]] void skip(const std::string &reason);

// One command-line argument. Paths are kept apart from plain text so a Windows
// target can be given a drive-letter path without each case converting it.
class Arg {
public:
    Arg(const char *text) : text_(text) {}
    Arg(std::string text) : text_(std::move(text)) {}
    Arg(const fs::path &path) : text_(path.string()), path_(true) {}
    template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    Arg(T value) : text_(std::to_string(value)) {}
    const std::string &text() const { return text_; }
    bool is_path() const { return path_; }
private:
    std::string text_;
    bool path_ = false;
};

// Exit status follows Python's returncode: negative means killed by that signal.
struct Result {
    int rc = 0;
    bool timed_out = false;
    double timeout = 0;
    std::string out, err, command;
};

class Command {
public:
    Command(std::string program, bool target) : program_(std::move(program)), target_(target) {}
    Command &arg(Arg value) { args_.push_back(std::move(value)); return *this; }
    Command &args(std::initializer_list<Arg> values) { args_.insert(args_.end(), values); return *this; }
    Command &stdin_file(fs::path path) { stdin_ = std::move(path); return *this; }
    const fs::path &input_file() const { return stdin_; }
    Command &timeout(double seconds) { timeout_ = seconds; return *this; }
    Command &cwd(fs::path directory) { cwd_ = std::move(directory); return *this; }
    Command &env(std::string name, std::string value) { env_[std::move(name)] = std::move(value); return *this; }
    Command &unset(std::string name) { env_.erase(name); unset_.push_back(std::move(name)); return *this; }

    const std::string &program() const { return program_; }
    bool target() const { return target_; }
    const std::vector<Arg> &arguments() const { return args_; }
    double timeout() const { return timeout_; }
    const fs::path &working_directory() const { return cwd_; }
    const std::map<std::string, std::string> &environment() const { return env_; }
    const std::vector<std::string> &unset_names() const { return unset_; }

private:
    std::string program_;
    bool target_;
    std::vector<Arg> args_;
    double timeout_ = 300;
    fs::path cwd_, stdin_;
    std::map<std::string, std::string> env_;
    std::vector<std::string> unset_;
};

// State for one running test. Each CTest test is one process and one Case.
class Case {
public:
    explicit Case(std::string name);
    ~Case();
    Case(const Case &) = delete;
    Case &operator=(const Case &) = delete;

    const std::string &name() const { return name_; }
    const std::string &stem() const { return stem_; }
    const fs::path &temp_if_created() const { return temp_; }

    // A fresh directory for this case; removed on success, kept on failure.
    const fs::path &temp();
    void keep_temp() { keep_temp_ = true; }

    fs::path root() const;

    // A harness declared by this file's .cmake fragment.
    Command harness(const std::string &name) const;
    // The player selected with FF8_PLAYER_HOST.
    Command player() const;
    // An already built game binary, such as build/dev/ff8.
    Command target(const fs::path &binary) const { return Command(binary.string(), true); }
    // A tool that always runs on the build machine.
    Command host(const std::string &program) const { return Command(program, false); }

    Result run(const Command &command);
    // Run and require exit status 0.
    Result ok(const Command &command);

private:
    std::string name_, stem_;
    fs::path temp_;
    bool keep_temp_ = false;
};

// Files and bytes.
std::string read_file(const fs::path &path);
void write_file(const fs::path &path, const std::string &contents);
std::string sha256(const std::string &bytes);
std::string sha256_file(const fs::path &path);
// A unique directory under `parent`; never removed. For output that must stay in build/.
fs::path mkdtemp_under(const fs::path &parent, const std::string &prefix);

// First capture group of a search, or a failure showing the text.
std::string capture(const std::string &text, const std::string &pattern, int group = 1);

// Comparison failure text, used by the LH_* macros.
std::string describe_bytes_diff(const std::string &actual, const std::string &expected);
std::string describe_text_diff(const std::string &actual, const std::string &expected);
std::string excerpt(const std::string &text);

// Discs the developer owns, found under LIONHEART_DISCS.
fs::path owned_disc(int number);
// saves/NAME from LIONHEART_SAVES, or a file extracted into build/generated/extracted.
fs::path owned_path(const std::string &relative);

// Registration. Use the macros below rather than calling these directly.
void add_case(const char *file, const std::string &name, Tier tier, std::function<void(Case &)> body);

namespace detail {
template <class T, class = void> struct has_id : std::false_type {};
template <class T> struct has_id<T, std::void_t<decltype(std::declval<const T &>().id)>> : std::true_type {};
template <class T, class = void> struct has_tier : std::false_type {};
template <class T> struct has_tier<T, std::void_t<decltype(std::declval<const T &>().tier)>> : std::true_type {};

template <class T> std::string param_id(const T &value) {
    if constexpr (has_id<T>::value) return std::string(value.id);
    else if constexpr (std::is_integral_v<T>) return std::to_string(value);
    else return std::string(value);
}
template <class T> Tier param_tier(const T &value, Tier fallback) {
    if constexpr (has_tier<T>::value) return value.tier;
    else return fallback;
}
template <class V> using value_t = std::decay_t<decltype(*std::begin(std::declval<const V &>()))>;

struct Registrar {
    Registrar(const char *file, const char *name, Tier tier, void (*body)(Case &)) {
        add_case(file, name, tier, body);
    }
};

template <class V> struct ParamRegistrar {
    ParamRegistrar(const char *file, const char *name, Tier tier, const V &values,
                   void (*body)(Case &, const value_t<V> &)) {
        for (const auto &value : values) {
            value_t<V> copy = value;
            add_case(file, std::string(name) + "[" + param_id(value) + "]", param_tier(value, tier),
                     [copy, body](Case &t) { body(t, copy); });
        }
    }
};

template <class A, class B> void check_eq(const char *file, int line, const char *actual_text,
                                          const A &actual, const char *expected_text, const B &expected) {
    if (actual == expected) return;
    std::ostringstream message;
    message << "expected " << actual_text << " == " << expected_text
            << "\n  actual:   " << actual << "\n  expected: " << expected;
    fail_at(file, line, message.str());
}
inline void check_contains(const char *file, int line, const char *expression, const std::string &text,
                           const std::string &needle, bool want) {
    if ((text.find(needle) != std::string::npos) == want) return;
    fail_at(file, line, std::string(expression) + (want ? " does not contain " : " must not contain ") +
                        "\"" + needle + "\"\n" + excerpt(text));
}
} // namespace detail
} // namespace lh

#define LH_DETAIL_TEST(function, tier)                                                         \
    static void lh_body_##function(lh::Case &t);                                               \
    static const lh::detail::Registrar lh_register_##function(__FILE__, #function, tier,       \
                                                              lh_body_##function);             \
    static void lh_body_##function([[maybe_unused]] lh::Case &t)

#define LH_DETAIL_TEST_P(function, tier, values)                                               \
    using lh_values_##function = decltype(values);                                             \
    static void lh_body_##function(lh::Case &t,                                                \
                                   const lh::detail::value_t<lh_values_##function> &p);        \
    static const lh::detail::ParamRegistrar<lh_values_##function> lh_register_##function(      \
        __FILE__, #function, tier, values, lh_body_##function);                                \
    static void lh_body_##function([[maybe_unused]] lh::Case &t,                               \
                                   const lh::detail::value_t<lh_values_##function> &p)

// One pytest function. The body sees `lh::Case &t`.
#define LH_TEST(function) LH_DETAIL_TEST(function, lh::Tier::standard)
#define LH_GAME_TEST(function) LH_DETAIL_TEST(function, lh::Tier::game)
#define LH_REALTIME_TEST(function) LH_DETAIL_TEST(function, lh::Tier::realtime)

// One parametrized pytest function, one case per element of `values`. The body
// also sees `p`, the element. An element's id comes from its `id` member, or
// from the element itself when it is a string or an integer. A `tier` member
// overrides the tier for that element.
#define LH_TEST_P(function, values) LH_DETAIL_TEST_P(function, lh::Tier::standard, values)
#define LH_GAME_TEST_P(function, values) LH_DETAIL_TEST_P(function, lh::Tier::game, values)
#define LH_REALTIME_TEST_P(function, values) LH_DETAIL_TEST_P(function, lh::Tier::realtime, values)

#define LH_FAIL(message) lh::fail_at(__FILE__, __LINE__, (message))
#define LH_CHECK(condition)                                                                    \
    do {                                                                                       \
        if (!(condition)) lh::fail_at(__FILE__, __LINE__, "check failed: " #condition);        \
    } while (0)
#define LH_CHECK_MSG(condition, message)                                                       \
    do {                                                                                       \
        if (!(condition)) lh::fail_at(__FILE__, __LINE__, std::string(#condition "\n") + (message)); \
    } while (0)
#define LH_EQ(actual, expected)                                                                \
    lh::detail::check_eq(__FILE__, __LINE__, #actual, (actual), #expected, (expected))
#define LH_CONTAINS(text, needle)                                                              \
    lh::detail::check_contains(__FILE__, __LINE__, #text, (text), (needle), true)
#define LH_NOT_CONTAINS(text, needle)                                                          \
    lh::detail::check_contains(__FILE__, __LINE__, #text, (text), (needle), false)
#define LH_BYTES_EQ(actual, expected)                                                          \
    do {                                                                                       \
        const std::string lh_a = (actual), lh_b = (expected);                                  \
        if (lh_a != lh_b) lh::fail_at(__FILE__, __LINE__, #actual " differs from " #expected   \
                                      "\n" + lh::describe_bytes_diff(lh_a, lh_b));             \
    } while (0)
#define LH_TEXT_EQ(actual, expected)                                                           \
    do {                                                                                       \
        const std::string lh_a = (actual), lh_b = (expected);                                  \
        if (lh_a != lh_b) lh::fail_at(__FILE__, __LINE__, #actual " differs from " #expected   \
                                      "\n" + lh::describe_text_diff(lh_a, lh_b));              \
    } while (0)
#define LH_FILE_EQ(actual_path, expected_path)                                                 \
    LH_BYTES_EQ(lh::read_file(actual_path), lh::read_file(expected_path))
