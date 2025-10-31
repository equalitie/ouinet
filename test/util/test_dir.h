#pragma once

#include <boost/filesystem.hpp>
#include "namespaces.h"

namespace ouinet {

class TestDir {
public:
    TestDir()
        : tempdir(fs::temp_directory_path() / "ouinet-cpp-tests" / suite_name() / test_name() / fs::unique_path())
    {
        fs::create_directories(tempdir);
    }

    const fs::path& path() const {
        return tempdir;
    }

    ~TestDir() {
        fs::remove_all(tempdir);
    }

private:
    static auto const& current_test_case() {
        return boost::unit_test::framework::current_test_case();
    }

    static std::string test_name() {
        return current_test_case().p_name;
    }

    static std::string suite_name() {
        return boost::unit_test::framework::get<boost::unit_test::test_suite>(current_test_case().p_parent_id).p_name;
    }

private:
    fs::path tempdir;
};

} // namespace ouinet
