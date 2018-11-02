#pragma once

#include <boost/optional.hpp>
#include <boost/utility/string_view.hpp>

namespace ouinet {

// https://semver.org

class Semver {
public:
    Semver() {}

    Semver(unsigned major, unsigned minor, unsigned patch)
        : _major(major)
        , _minor(minor)
        , _patch(patch)
    {}

    Semver(unsigned major, unsigned minor, unsigned patch,
            std::list<std::string> pre_release)
        : _major(major)
        , _minor(minor)
        , _patch(patch)
        , _pre_release(std::move(pre_release))
    {}

    Semver(unsigned major, unsigned minor, unsigned patch,
            std::list<std::string> pre_release,
            std::list<std::string> metadata)
        : _major(major)
        , _minor(minor)
        , _patch(patch)
        , _pre_release(std::move(pre_release))
        , _metadata(std::move(metadata))
    {}

    Semver(unsigned major, unsigned minor, unsigned patch,
            boost::string_view pre_release)
        : _major(major)
        , _minor(minor)
        , _patch(patch)
    {
        auto parts = consume_parts(pre_release);
        if (!parts) {
            _major = _minor = _patch = 0;
            return;
        }
        _pre_release = std::move(*parts);
    }

    Semver(unsigned major, unsigned minor, unsigned patch,
            boost::string_view pre_release,
            boost::string_view metadata)
        : _major(major)
        , _minor(minor)
        , _patch(patch)
    {
        auto opt_pre_release = consume_parts(pre_release);
        if (!opt_pre_release) {
            _major = _minor = _patch = 0;
            return;
        }

        auto opt_metadata = consume_parts(metadata);
        if (!opt_metadata) {
            _major = _minor = _patch = 0;
            return;
        }

        _pre_release = std::move(*opt_pre_release);
        _metadata = std::move(*opt_metadata);
    }

    Semver(const Semver& v) = default;

    unsigned major() const { return _major; }
    unsigned minor() const { return _minor; }
    unsigned patch() const { return _patch; }

    static boost::optional<Semver> parse(boost::string_view);

    bool operator==(const Semver& other) const {
        return std::tie(_major, _minor, _patch, _pre_release)
            == std::tie( other._major, other._minor, other._patch, other._pre_release);
    }

private:
    static boost::optional<unsigned> consume_int(boost::string_view& s);
    static boost::string_view consume_part(boost::string_view& s);
    static boost::optional<std::list<std::string>> consume_parts(boost::string_view& s);

private:
    friend std::ostream& ::ouinet::operator<<(std::ostream&, const Semver);

    unsigned _major = 0;
    unsigned _minor = 0;
    unsigned _patch = 0;
    std::list<std::string> _pre_release;
    std::list<std::string> _metadata;
};

inline
std::ostream& operator<<(std::ostream& os, const Semver v)
{
    os << v.major() << "." << v.minor() << "." << v.patch(); 
    if (!v._pre_release.empty()) {
        os << "-";
        for (auto i = v._pre_release.begin(); i != v._pre_release.end();) {
            os << *i++;
            if (i != v._pre_release.end()) os << '.';
        }
    }
    if (!v._metadata.empty()) {
        os << "+";
        for (auto i = v._metadata.begin(); i != v._metadata.end();) {
            os << *i++;
            if (i != v._metadata.end()) os << '.';
        }
    }
    return os;
}

inline
boost::optional<unsigned> Semver::consume_int(boost::string_view& s)
{
    size_t endpos = 0;

    while (endpos < s.size() && ('0' <= s[endpos] && s[endpos] <= '9')) {
        ++endpos;
    }

    if (endpos == 0) return boost::none;

    unsigned r = 0;
    unsigned m = 1;

    for (size_t i = 0; i < endpos; ++i) {
        unsigned c = (unsigned char) s[endpos-i-1];
        r += m * (c - '0');
        m *= 10;
    }

    s.remove_prefix(endpos);
    return r;
}

inline
boost::string_view Semver::consume_part(boost::string_view& s)
{
    size_t endpos = 0;

    bool isnum = true;

    while (endpos < s.size()) {
        char c = s[endpos];
        bool isnum_ = ('0' <= c && c <= '9');
        if (!isnum_) isnum = false;
        if (!isnum_ && c != '-'
                    && !('a' <= c && c <= 'z')
                    && !('A' <= c && c <= 'Z')) {
            break;
        }

        ++endpos;
    }

    if (isnum && endpos) {
        if (s[0] == '0') return boost::string_view();
    }

    auto r = s.substr(0, endpos);
    s.remove_prefix(endpos);
    return r;
}

inline
boost::optional<std::list<std::string>>
Semver::consume_parts(boost::string_view& s)
{
    std::list<std::string> pre_release;

    while(true) {
        auto part = consume_part(s);
        if (part.empty()) return boost::none;
        pre_release.push_back(part.to_string());
        if (!s.starts_with('.')) break;
        s.remove_prefix(1);
    }

    return pre_release;
}

inline
boost::optional<Semver> Semver::parse(boost::string_view s)
{
    using boost::optional;

    while (s.starts_with(' ')) s.remove_prefix(1);
    while (s.ends_with(' '))   s.remove_suffix(1);

    auto major = Semver::consume_int(s);
    if (!major) return boost::none;
    if (!s.starts_with('.')) return boost::none;
    s.remove_prefix(1);

    auto minor = Semver::consume_int(s);
    if (!minor) return boost::none;
    if (!s.starts_with('.')) return boost::none;
    s.remove_prefix(1);

    auto patch = Semver::consume_int(s);
    if (!patch) return boost::none;

    if (s.empty()) return Semver{*major, *minor, *patch};

    if (!s.starts_with('-') && !s.starts_with('+')) return boost::none;

    boost::optional<std::list<std::string>> pre_release;

    if (s.starts_with('-')) {
        s.remove_prefix(1);
        pre_release = consume_parts(s);
        if (!pre_release) return boost::none;
    }

    if (s.empty())
        return Semver{ *major
                     , *minor
                     , *patch
                     , std::move(*pre_release)};

    if (!s.starts_with('+')) return boost::none;
    s.remove_prefix(1);

    boost::optional<std::list<std::string>> metadata;

    metadata = consume_parts(s);

    if (!metadata) return boost::none;

    return Semver{ *major
                 , *minor
                 , *patch
                 , std::move(*pre_release)
                 , std::move(*metadata)};

}

static const Semver INJECTOR_VERSION(1,0,0);

} // namespace
