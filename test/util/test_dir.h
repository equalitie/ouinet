#pragma once

#include <boost/filesystem.hpp>
#include "namespaces.h"

namespace ouinet {

class TestDir {
public:
    struct Builder {
        bool _delete_if_exists = false;

        Builder delete_if_exists(bool value) {
            _delete_if_exists = value;
            return *this;
        }

        TestDir build(fs::path path) const {
            return TestDir(path, *this);
        }
    };

public:
#ifdef BOOST_TEST_MODULE
    TestDir()
        : _tempdir(fs::temp_directory_path() / "ouinet-cpp-tests" / suite_name() / test_name() / fs::unique_path())
    {
        fs::create_directories(_tempdir);
    }
#endif

    TestDir(fs::path path, std::optional<Builder> builder = {})
        : _tempdir(std::move(path))
    {
        if (builder) {
            if (builder->_delete_if_exists && fs::exists(_tempdir)) {
                fs::remove_all(_tempdir);
            }
        }

        fs::create_directories(_tempdir);
    }

    TestDir make_subdir(const std::string& name) const {
        fs::path path = _tempdir / name;
        fs::create_directory(path);
        auto dir = TestDir(path);
        dir.delete_on_exit(false);
        return dir;
    }

    const fs::path& path() const {
        return _tempdir;
    }

    const std::string string() const {
        return _tempdir.string();
    }

    void delete_content() const {
        auto begin = fs::directory_iterator(_tempdir);
        auto end = fs::directory_iterator();
        sys::error_code ec;
        for (auto i = begin; i != end; ++i) {
            try {
                fs::remove_all(*i);
            } catch (const std::exception& e) {
                std::cout << "Failed to remove " << *i << ": " << e.what() << "\n";
                throw;
            }
        }
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
#ifdef BOOST_TEST_MODULE
    static auto const& current_test_case() {
        return boost::unit_test::framework::current_test_case();
    }

    static std::string test_name() {
        return current_test_case().p_name;
    }

    static std::string suite_name() {
        return boost::unit_test::framework::get<boost::unit_test::test_suite>(current_test_case().p_parent_id).p_name;
    }
#endif

private:
    fs::path _tempdir;
    bool _delete_on_exit = true;
};

} // namespace ouinet
