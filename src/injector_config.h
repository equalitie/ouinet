#pragma once

namespace ouinet {

class InjectorConfig {
public:
    InjectorConfig() = default;
    InjectorConfig(const InjectorConfig&) = default;
    InjectorConfig(InjectorConfig&&) = default;
    InjectorConfig& operator=(const InjectorConfig&) = default;
    InjectorConfig& operator=(InjectorConfig&&) = default;

    // May thow on error.
    InjectorConfig(int argc, const char** argv);

    bool is_help() const
    { return _is_help; }

    boost::optional<size_t> open_file_limit() const
    { return _open_file_limit; }

    boost::filesystem::path repo_root() const
    { return _repo_root; }

    bool listen_on_i2p() const
    { return _listen_on_i2p; }

    boost::optional<asio::ip::tcp::endpoint> tcp_endpoint() const
    { return _tcp_endpoint; }

    static boost::program_options::options_description
    options_description();

private:
    bool _is_help = false;
    boost::filesystem::path _repo_root;
    boost::optional<size_t> _open_file_limit;
    bool _listen_on_i2p = false;
    boost::optional<asio::ip::tcp::endpoint> _tcp_endpoint;
    boost::filesystem::path OUINET_CONF_FILE = "ouinet-injector.conf";
};

inline
boost::program_options::options_description
InjectorConfig::options_description()
{
    namespace po = boost::program_options;
    using std::string;

    po::options_description desc("\nOptions");

    desc.add_options()
        ("help", "Produce this help message")
        ("repo", po::value<string>(), "Path to the repository root")
        ("listen-on-tcp", po::value<string>(), "IP:PORT endpoint on which we'll listen")
        ("listen-on-i2p",
         po::value<string>(),
         "Whether we should be listening on I2P (true/false)")
        ("open-file-limit"
         , po::value<unsigned int>()
         , "To increase the maximum number of open files")
        ;

    return desc;
}

inline
InjectorConfig::InjectorConfig(int argc, const char**argv)
{
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;
    using std::string;

    auto desc = options_description();

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        _is_help = true;
        return;
    }

    if (!vm.count("repo")) {
        throw std::runtime_error("The 'repo' argument is missing");
    }

    _repo_root = vm["repo"].as<string>();

    if (!exists(_repo_root) || !is_directory(_repo_root)) {
        throw std::runtime_error(util::str(
            "The path ", _repo_root, " either doesn't exist or"
            " isn't a directory."));
    }

    fs::path ouinet_conf_path = _repo_root/OUINET_CONF_FILE;

    if (!fs::is_regular_file(ouinet_conf_path)) {
        throw std::runtime_error(util::str(
            "The path ", _repo_root, " does not contain the "
            , OUINET_CONF_FILE, " configuration file."));
    }

    std::ifstream ouinet_conf(ouinet_conf_path.native());

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (vm.count("open-file-limit")) {
        _open_file_limit = vm["open-file-limit"].as<unsigned int>();
    }

    // Unfortunately, Boost.ProgramOptions doesn't support arguments without
    // values in config files. Thus we need to force the 'listen-on-i2p' arg
    // to have one of the strings values "true" or "false".
    if (vm.count("listen-on-i2p")) {
        auto value = vm["listen-on-i2p"].as<string>();

        if (value != "" && value != "true" && value != "false") {
            throw std::runtime_error(
                "The listen-on-i2p parameter may be either 'true' or 'false'");
        }

        _listen_on_i2p = (value == "true");
    }

    if (vm.count("listen-on-tcp")) {
        _tcp_endpoint = util::parse_endpoint(vm["listen-on-tcp"].as<string>());
    }
}

} // ouinet namespace
