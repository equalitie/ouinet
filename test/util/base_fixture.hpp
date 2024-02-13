#include <boost/test/included/unit_test.hpp>
#include <boost/filesystem.hpp>

namespace ut = boost::unit_test;

struct fixture_base {
    std::string test_name;
    std::string suite_name;
    std::string test_id;
    std::string suite_id;

    fixture_base() {
        auto date_time = get_date_time();
        test_name = ut::framework::current_test_case().p_name;
        suite_name = ut::framework::get<ut::test_suite>(ut::framework::current_test_case().p_parent_id).p_name;
        suite_id = date_time + "_" + suite_name;
        test_id = suite_id + "_" + test_name;
    }
    ~fixture_base(){
    }

    std::string get_date_time(){
        size_t buffer_size = 32;
        std::time_t now = std::time(nullptr);
        char testSuiteId[buffer_size];
        std::string dateTimeFormat = "%Y%m%d-%H%M%S";
        std::strftime(testSuiteId,
                      buffer_size,
                      dateTimeFormat.c_str(),
                      std::gmtime(&now));
        return testSuiteId;
    }

    struct temp_file {
    public:
        temp_file(std::string fileName){
            name = fileName;
        }
        ~temp_file(){
            if (boost::filesystem::exists(name)){
                boost::filesystem::remove(name);
            }
        }
        std::string get_name(){
            return name;
        }
    private:
        std::string name;
    };

};
