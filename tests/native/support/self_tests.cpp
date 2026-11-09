// Tests of the test framework. Their names do not start with test_, so they are
// labelled infra and never count toward parity with the Python suite.
#include "lh_internal.h"

#include <chrono>
#include <csignal>
#include <string>
#include <unistd.h>


LH_TEST(identity_parity) {
    LH_CHECK_MSG(lh::internal::check_identities({}), "native identities differ from tests/native/identities.tsv");
}

LH_TEST(process_captures_both_streams_exit_status_and_environment) {
    auto result = t.run(t.host("/bin/sh").args({"-c", "echo out; echo err >&2; printf %s \"$A$B\"; exit 3"})
                         .env("A", "x").env("B", "y").timeout(10));
    LH_EQ(result.rc, 3);
    LH_TEXT_EQ(result.out, "out\nxy");
    LH_TEXT_EQ(result.err, "err\n");
}

LH_TEST(process_unsets_environment_and_uses_working_directory) {
    const auto dir = t.temp();
    auto result = t.ok(t.host("/bin/sh").args({"-c", "pwd; printf %s \"${HOME-unset}\""})
                        .cwd(dir).unset("HOME").timeout(10));
    LH_TEXT_EQ(result.out, lh::fs::canonical(dir).string() + "\nunset");
}

LH_TEST(process_reports_signals_like_python) {
    auto result = t.run(t.host("/bin/sh").args({"-c", "kill -SEGV $$"}).timeout(10));
    LH_EQ(result.rc, -11);
}

LH_TEST(process_timeout_kills_the_program_and_its_children) {
    const auto pid_file = t.temp() / "child.pid";
    const auto started = std::chrono::steady_clock::now();
    bool timed_out = false;
    try {
        // A shell loop rather than sleep, so this needs nothing from PATH.
        t.run(t.host("/bin/sh").args({"-c", "(while :; do :; done) & echo $! > " + pid_file.string() + "; wait"}).timeout(0.3));
    } catch (const lh::Failure &failure) {
        timed_out = std::string(failure.what()).find("timed out") != std::string::npos;
    }
    LH_CHECK(timed_out);
    LH_CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    const pid_t child = std::stoi(lh::read_file(pid_file));
    bool gone = false;
    for (int i = 0; i < 200 && !gone; ++i) {
        gone = kill(child, 0) != 0;
        if (!gone) usleep(10000);
    }
    LH_CHECK_MSG(gone, "a process started by the timed-out program is still running");
}

LH_TEST(missing_program_fails_with_its_name) {
    bool reported = false;
    try {
        t.run(t.host("/nonexistent/program").timeout(5));
    } catch (const lh::Failure &failure) {
        reported = std::string(failure.what()).find("/nonexistent/program") != std::string::npos;
    }
    LH_CHECK(reported);
}

LH_TEST(byte_difference_names_the_offset) {
    const std::string report = lh::describe_bytes_diff("abcd", "abXd");
    LH_CONTAINS(report, "first difference at byte 2");
}

LH_TEST(text_difference_names_the_line) {
    const std::string report = lh::describe_text_diff("a\nb\n", "a\nc\n");
    LH_CONTAINS(report, "first differing line 2");
}

LH_TEST(sha256_matches_known_vectors) {
    LH_EQ(lh::sha256(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    LH_EQ(lh::sha256("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

// A Release flag set must not compile assert() out of the harnesses.
LH_TEST(harness_asserts_are_enabled) {
    auto result = t.run(t.harness("assert_guard").timeout(30));
    LH_CHECK_MSG(result.rc != 0, "assert(0) did not stop the harness; NDEBUG reached it");
}

LH_TEST(windows_targets_get_drive_letter_paths) {
    LH_EQ(lh::internal::to_windows_path("/tmp/a b/c.bin"), std::string("Z:\\tmp\\a b\\c.bin"));
    LH_EQ(lh::internal::to_windows_path("relative/c.bin"), std::string("relative/c.bin"));
}
