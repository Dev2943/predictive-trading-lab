#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "ptl/auth/credentials.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::auth;

namespace {

std::filesystem::path temp_file(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

TEST_CASE("a secret never renders its value", "[auth][credentials]") {
    // A secret that reaches a log line by accident must print as a length, not
    // as the key. This is the difference between a near miss and an incident.
    const Secret s{"PKTEST1234567890ABCDEF"};
    REQUIRE(s.redacted() == "<redacted:22>");
    REQUIRE(s.redacted().find("PKTEST") == std::string::npos);
    REQUIRE(Secret{}.redacted() == "<unset>");
    // reveal() is the single named door, so every use is greppable.
    REQUIRE(s.reveal() == "PKTEST1234567890ABCDEF");
}

TEST_CASE("static credentials resolve a key pair", "[auth][credentials]") {
    const StaticCredentials src{{{"ALPACA_KEY_ID", "PKABC"}, {"ALPACA_SECRET_KEY", "secretvalue"}}};
    auto c = resolve(src, "ALPACA_KEY_ID", "ALPACA_SECRET_KEY");
    REQUIRE(c.has_value());
    REQUIRE(c->key_id == "PKABC");  // an id, not a secret: safe in the manifest
    REQUIRE(c->secret.reveal() == "secretvalue");
    REQUIRE(c->source == "static");
}

TEST_CASE("a missing credential names the variables to set and echoes nothing",
          "[auth][credentials]") {
    // "authentication failed" costs the reader twenty minutes. Naming the
    // variable costs nothing.
    const StaticCredentials only_id{{{"ALPACA_KEY_ID", "PKABC"}}};
    auto r = resolve(only_id, "ALPACA_KEY_ID", "ALPACA_SECRET_KEY");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("ALPACA_SECRET_KEY") != std::string::npos);
    REQUIRE(r.error().message.find("PKABC") == std::string::npos);
    // And it must steer the reader away from the committed config directory.
    REQUIRE(r.error().message.find("config/*.toml") != std::string::npos);

    const StaticCredentials neither{{}};
    auto both = resolve(neither, "ALPACA_KEY_ID", "ALPACA_SECRET_KEY");
    REQUIRE_FALSE(both.has_value());
    REQUIRE(both.error().message.find("ALPACA_KEY_ID") != std::string::npos);
    REQUIRE(both.error().message.find("ALPACA_SECRET_KEY") != std::string::npos);
}

TEST_CASE("environment credentials read the process environment", "[auth][credentials]") {
    ::setenv("PTL_TEST_KEY_ID", "envkey", 1);
    ::setenv("PTL_TEST_SECRET", "envsecret", 1);
    const EnvironmentCredentials env;
    auto c = resolve(env, "PTL_TEST_KEY_ID", "PTL_TEST_SECRET");
    REQUIRE(c.has_value());
    REQUIRE(c->key_id == "envkey");

    // An empty variable is absent, not present-and-blank. A blank key would
    // otherwise produce a confusing 401 rather than a clear startup failure.
    ::setenv("PTL_TEST_SECRET", "", 1);
    REQUIRE_FALSE(env.lookup("PTL_TEST_SECRET").has_value());
    REQUIRE_FALSE(env.lookup("PTL_TEST_DEFINITELY_UNSET").has_value());

    ::unsetenv("PTL_TEST_KEY_ID");
    ::unsetenv("PTL_TEST_SECRET");
}

TEST_CASE("a credential file must not be readable by others", "[auth][credentials][security]") {
    // A credential file anyone on the machine can read is not a credential
    // file. Refusing is more useful than warning: a warning in a build log is
    // a warning nobody reads.
    const auto path = temp_file("ptl_creds_open.env");
    {
        std::ofstream out{path};
        out << "ALPACA_KEY_ID=abc\n";
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write |
                                           std::filesystem::perms::group_read);
    auto r = FileCredentials::load(path.string());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("chmod 600") != std::string::npos);

    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    REQUIRE(FileCredentials::load(path.string()).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("a credential file parses dotenv forms", "[auth][credentials]") {
    const auto path = temp_file("ptl_creds_ok.env");
    {
        std::ofstream out{path};
        out << "# a comment\n\n";
        out << "ALPACA_KEY_ID=PKABC\n";
        out << "export ALPACA_SECRET_KEY=\"quoted secret\"\n";
        out << "  SPACED  =  value  \n";
    }
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    auto fc = FileCredentials::load(path.string());
    REQUIRE(fc.has_value());
    REQUIRE(fc->lookup("ALPACA_KEY_ID")->reveal() == "PKABC");
    // The export prefix is stripped so the same file can be sourced by a shell.
    REQUIRE(fc->lookup("ALPACA_SECRET_KEY")->reveal() == "quoted secret");
    REQUIRE(fc->lookup("SPACED")->reveal() == "value");
    REQUIRE_FALSE(fc->lookup("ABSENT").has_value());
    std::filesystem::remove(path);
}

TEST_CASE("a malformed credential file is rejected with a line number", "[auth][credentials]") {
    const auto path = temp_file("ptl_creds_bad.env");
    {
        std::ofstream out{path};
        out << "GOOD=1\n";
        out << "this line has no equals sign\n";
    }
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    auto r = FileCredentials::load(path.string());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("line 2") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("chained sources resolve in order and report which answered", "[auth][credentials]") {
    // "Which credential did this run use?" must have an answer, and the answer
    // must never be the value.
    auto first = std::make_shared<StaticCredentials>(
        std::map<std::string, std::string, std::less<>>{{"KEY", "from_first"}});
    auto second =
        std::make_shared<StaticCredentials>(std::map<std::string, std::string, std::less<>>{
            {"KEY", "from_second"}, {"OTHER", "only_second"}});
    ChainedCredentials chain;
    chain.add(first);
    chain.add(second);

    REQUIRE(chain.lookup("KEY")->reveal() == "from_first");
    REQUIRE(chain.lookup("OTHER")->reveal() == "only_second");
    REQUIRE_FALSE(chain.lookup("NEITHER").has_value());
    REQUIRE(chain.source_of("KEY") == "static");
    REQUIRE(chain.source_of("NEITHER") == "(none)");
}
