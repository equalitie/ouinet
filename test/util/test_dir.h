#pragma once

#include <boost/filesystem.hpp>
#include "namespaces.h"

namespace ouinet {

class TestDir {
public:
    TestDir()
        : _tempdir(fs::temp_directory_path() / "ouinet-cpp-tests" / suite_name() / test_name() / fs::unique_path())
    {
        fs::create_directories(_tempdir);
    }

    TestDir(fs::path deterministic_path)
        : _tempdir(std::move(deterministic_path))
    {
        fs::create_directories(_tempdir);
    }

    const fs::path make_subdir(const std::string& name) const {
        fs::path path = _tempdir / name;
        fs::create_directory(path);
        return path;
    }

    const fs::path& path() const {
        return _tempdir;
    }

    ~TestDir() {
        if (_delete_on_exit) {
            fs::remove_all(_tempdir);
        }
    }

    bool delete_on_exit() const {
        return _delete_on_exit;
    }

    void delete_on_exit(bool value) {
        _delete_on_exit = value;
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
    fs::path _tempdir;
    bool _delete_on_exit = true;
};

} // namespace ouinet
