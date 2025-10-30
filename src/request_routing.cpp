#include "request_routing.h"

using namespace ouinet;

using Request = http::request_header<>;

namespace ouinet {

//------------------------------------------------------------------------------
// Request expressions can tell whether they match a given request
// (much like regular expressions match strings).
namespace reqexpr {
class ReqExpr;

// The type of functions that retrieve a given field (as a string) from a request.
using field_getter = std::function<beast::string_view (const http::request_header<>&)>;

class reqex {
    friend reqex true_();
    friend reqex false_();
    friend reqex from_regex(const field_getter&, const boost::regex&);
    friend reqex operator!(const reqex&);
    friend reqex operator&&(const reqex&, const reqex&);
    friend reqex operator||(const reqex&, const reqex&);

    private:
        const std::shared_ptr<ReqExpr> impl;
        reqex(const std::shared_ptr<ReqExpr> impl_) : impl(impl_) { }

    public:
        // True when the request matches this expression.
        bool match(const http::request_header<>& req) const;
};

// Use the following functions to create request expressions,
// then call the ``match()`` method of the resulting object
// with the request that you want to check.

// Always matches, regardless of request content.
reqex true_();
// Never matches, regardless of request content.
reqex false_();
// Only matches when the extracted field matches the given (anchored) regular expression.
reqex from_regex(const field_getter&, const boost::regex&);
reqex from_regex(const field_getter&, const std::string&);

// Negates the matching of the given expression.
reqex operator!(const reqex&);

// Short-circuit AND and OR operations on the given expressions.
reqex operator&&(const reqex&, const reqex&);
reqex operator||(const reqex&, const reqex&);

class ReqExpr {
    public:
        ReqExpr() = default;
        virtual ~ReqExpr() = default;
        ReqExpr(ReqExpr&&) = default;
        ReqExpr& operator=(ReqExpr&&) = default;
        ReqExpr(const ReqExpr&) = default;
        ReqExpr& operator=(const ReqExpr&) = default;

        virtual bool match(const Request&) const = 0;
};

class RegexReqExpr : public ReqExpr {  // can match a request field against a regular expression
    private:
        const field_getter get_field;
        const boost::regex regexp;

    public:
        RegexReqExpr(const field_getter& gf, const boost::regex& rx)
            : get_field(gf), regexp(rx) { };

        bool match(const Request& req) const override {
            return boost::regex_match(std::string(get_field(req)), regexp);
        }
};

class TrueReqExpr : public ReqExpr {  // matches all requests
    public:
        bool match(const Request& req) const override {
            return true;
        }
};

class FalseReqExpr : public ReqExpr {  // matches no request
    public:
        bool match(const Request& req) const override {
            return false;
        }
};

class NotReqExpr : public ReqExpr {  // negates match of subexpr
    private:
        const std::shared_ptr<ReqExpr> child;

    public:
        NotReqExpr(const std::shared_ptr<ReqExpr> sub)
            : child(sub) { }

        bool match(const Request& req) const override {
            return !(child->match(req));
        }
};

class AndReqExpr : public ReqExpr {  // a shortcircuit logical AND of two subexprs
    private:
        const std::shared_ptr<ReqExpr> left, right;

    public:
        AndReqExpr(const std::shared_ptr<ReqExpr> left_, const std::shared_ptr<ReqExpr> right_)
            : left(left_), right(right_) { }

        bool match(const Request& req) const override {
            if (left->match(req))
              return right->match(req);
            return false;
        }
};

class OrReqExpr : public ReqExpr {  // a shortcircuit logical OR of two subexprs
    private:
        const std::shared_ptr<ReqExpr> left, right;

    public:
        OrReqExpr(const std::shared_ptr<ReqExpr> left_, const std::shared_ptr<ReqExpr> right_)
            : left(left_), right(right_) { }

        bool match(const Request& req) const override {
            if (left->match(req))
                return true;
            return right->match(req);
        }
};

bool
reqex::match(const Request& req) const {
    return impl->match(req);
}

reqex
true_()
{
    return reqex(std::make_shared<TrueReqExpr>());
}

reqex
false_()
{
    return reqex(std::make_shared<FalseReqExpr>());
}

reqex
from_regex(const field_getter& gf, const boost::regex& rx)
{
    return reqex(std::make_shared<RegexReqExpr>(gf, rx));
}

reqex
from_regex(const field_getter& gf, const std::string& rx)
{
    return from_regex(gf, boost::regex(rx));
}

reqex
operator!(const reqex& sub)
{
    return reqex(std::make_shared<NotReqExpr>(sub.impl));
}

reqex
operator&&(const reqex& left, const reqex& right)
{
    return reqex(std::make_shared<AndReqExpr>(left.impl, right.impl));
}

reqex
operator||(const reqex& left, const reqex& right)
{
    return reqex(std::make_shared<OrReqExpr>(left.impl, right.impl));
}

} // ouinet::reqexpr namespace

//------------------------------------------------------------------------------
namespace request_route {

Config
route_choose_config(const http::request_header<>& req, const ClientConfig& config)
{
    using std::deque;

    // This request router configuration will be used for requests by default.
    //
    // Looking up the cache when needed is allowed, while for fetching fresh
    // content:
    //
    //  - the origin is first contacted directly,
    //    for good overall speed and responsiveness
    //  - if not available, the injector is used to
    //    get the content and cache it for future accesses
    //
    // So enabling the Injector channel will result in caching content
    // when access to the origin is not possible.
    //
    // To also avoid getting content from the cache
    // (so that browsing looks like using a normal non-caching proxy)
    // the cache can be disabled.
    static const Config default_request_config
        { deque<fresh_channel>({ fresh_channel::origin
                               , fresh_channel::injector_or_dcache})};

    // This is the matching configuration for the one above,
    // but for uncacheable requests.
    static const Config nocache_request_config
        { deque<fresh_channel>({ fresh_channel::origin
                               , fresh_channel::proxy})};

    // Expressions to test the request against and configurations to be used.
    // TODO: Create once and reuse.
    using Match = std::pair<const ouinet::reqexpr::reqex, const Config>;

    auto method_override_getter([](const Request& r) {return r["X-HTTP-Method-Override"];});
    auto method_getter([](const Request& r) {return r.method_string();});
    auto host_getter([](const Request& r) {return r[http::field::host];});
    auto hostname_getter([](const Request& r) {return util::split_ep(r[http::field::host]).first;});
    auto x_private_getter([](const Request& r) {return r[http_::request_private_hdr];});
    auto target_getter([](const Request& r) {return r.target();});

    auto local_rx = util::str("https?://[^:/]+\\.", config.local_domain(), "(:[0-9]+)?/.*");

#ifdef NDEBUG // release
    const Config unrequested{deque<fresh_channel>({fresh_channel::origin})};
#else // debug
    // Don't request these in debug mode as they bring a lot of noise into the log
    const Config unrequested{deque<fresh_channel>()};
#endif

    // Flags for normal, case-insensitive regular expression.
    static const auto rx_icase = boost::regex::normal | boost::regex::icase;

    static const boost::regex localhost_exact_rx{"localhost", rx_icase};

    std::vector<Match> matches({
        // Please keep host-specific matches at a bare minimum
        // as they require curation and they may have undesired side-effects;
        // instead, use user agent-side mechanisms like browser settings and extensions when possible,
        // and only leave those that really break things and cannot be otherwise disabled.
        //
        // Also note that using the normal mechanisms for these may help users
        // keep their browsers up-to-date (by retrieving via the injector in case of interference),
        // and they may still not pollute the cache unless
        // the requests are explicitly marked for caching and announcement.

        // Disable cache and always go to origin for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://ident\\.me/.*")
        //     , {deque<fresh_channel>({fresh_channel::origin})} ),

        /* Requests which may be considered public but too noisy and of little value for caching
         * should be processed by something like browser extensions.
        // Google Search completion
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?google\\.com/complete/.*")
             , unrequested ),
        */

        /* To stop these requests in Firefox,
         * uncheck "Preferences / Privacy & Security / Deceptive Content and Dangerous Software Protection".
        // Safe Browsing API <https://developers.google.com/safe-browsing/>.
        // These should not be very frequent after start,
        // plus they use POST requests, so there is no risk of accidental injection.
        Match( reqexpr::from_regex(target_getter, "https://safebrowsing\\.googleapis\\.com/.*")
             , unrequested ),
        */

        /* These are used to retrieve add-ons and all kinds of minor security updates from Mozilla,
         * and they mostly happen on browser start only.
        // Disable cache and always go to origin for these mozilla sites.
        Match( reqexpr::from_regex(target_getter, "https?://content-signature\\.cdn\\.mozilla\\.net/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*services\\.mozilla\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*cdn\\.mozilla\\.net/.*")
             , unrequested ),
        */

        /* To stop these requests,
         * uncheck "Preferences / Add-ons / (gear icon) / Update Add-ons Automatically".
        // Firefox add-ons hotfix (auto-update)
        Match( reqexpr::from_regex(target_getter, "https?://services\\.addons\\.mozilla\\.org/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://versioncheck-bg\\.addons\\.mozilla\\.org/.*")
             , unrequested ),
        */

        /* To stop these requests,
         * uncheck all options from "Preferences / Privacy & Security / Firefox Data Collection and Use",
         * maybe clear `toolkit.telemetry.server` in `about:config`.
        // Firefox telemetry
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*telemetry\\.mozilla\\.net/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*telemetry\\.mozilla\\.org/.*")
             , unrequested ),
        */

        /* This should work as expected as long as Origin is enabled.
         * To stop these requests, set `network.captive-portal-service.enabled` to false in `about:config`.
        // Firefox' captive portal detection
        Match( reqexpr::from_regex(target_getter, "https?://detectportal\\.firefox\\.com/.*")
             , unrequested ),
        */

        /* To avoid these at the client, use some kind of ad blocker (like uBlock Origin).
        // Ads and tracking
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*google-analytics\\.com/.*")
             , unrequested ),

        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*googlesyndication\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*googletagservices\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*moatads\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*amazon-adsystem\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*adsafeprotected\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*ads-twitter\\.com/.*")
             , unrequested ),
        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*doubleclick\\.net/.*")
             , unrequested ),

        Match( reqexpr::from_regex(target_getter, "https?://([^/\\.]+\\.)*summerhamster\\.com/.*")
             , unrequested ),

        Match( reqexpr::from_regex(target_getter, "https?://ping.chartbeat.net/.*")
             , unrequested ),
        */

        // Handle requests to <http://localhost/> internally.
        Match( reqexpr::from_regex(host_getter, localhost_exact_rx)
             , {deque<fresh_channel>({fresh_channel::_front_end})} ),

        Match( reqexpr::from_regex(host_getter, util::str(config.front_end_endpoint()))
             , {deque<fresh_channel>({fresh_channel::_front_end})} ),

        // Other requests to the local host should not use the network
        // to avoid leaking internal services accessed through the client.
        Match( reqexpr::from_regex(hostname_getter, util::localhost_rx)
             , {deque<fresh_channel>({fresh_channel::origin})} ),

        // Access to sites under the local TLD are always accessible
        // with good connectivity, so always use the Origin channel
        // and never cache them.
        Match( reqexpr::from_regex(target_getter, local_rx)
             , {deque<fresh_channel>({fresh_channel::origin})} ),

        // Do not use caching for requests tagged as private with Ouinet headers.
        Match( reqexpr::from_regex( x_private_getter
                                  , boost::regex(http_::request_private_true, rx_icase))
             , nocache_request_config),

        // When to try to cache or not, depending on the request method:
        //
        //   - Unsafe methods (CONNECT, DELETE, PATCH, POST, PUT): do not cache
        //   - Safe but uncacheable methods (OPTIONS, TRACE): do not cache
        //   - Safe and cacheable (GET, HEAD): cache
        //
        // Thus the only remaining method that implies caching is GET.
        Match( !reqexpr::from_regex(method_getter, "(GET|HEAD)")
             , nocache_request_config),
        // Requests declaring a method override are checked by that method.
        // This is not a standard header,
        // but for instance Firefox uses it for Safe Browsing requests,
        // which according to this standard should actually be POST requests
        // (probably in the hopes of having more chances that requests get through,
        // in spite of using HTTPS).
        Match( !reqexpr::from_regex(method_override_getter, "(|GET)")
             , nocache_request_config),

        // Disable cache and always go to proxy for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://ifconfig\\.co/.*")
        //     , {deque<fresh_channel>({fresh_channel::proxy})} ),
        // Force cache and default channels for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example\\.com/.*")
        //     , {deque<fresh_channel>()} ),
        // Force cache and particular channels for this site.
        //Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example\\.net/.*")
        //     , {deque<fresh_channel>({fresh_channel::injector})} ),
    });
    // Requests to the private addresses should not use the network
    // to avoid leaking internal services accessed through the client,
    // unless the option `allow-private-targets` is set to true.
    if (!config.is_private_target_allowed())
        matches.push_back(Match(reqexpr::from_regex(hostname_getter, util::private_addr_rx)
                         , {deque<fresh_channel>({fresh_channel::origin})}));

    for (auto mit = matches.begin(); mit != matches.end(); ++mit)
        if (mit->first.match(req))
            return mit->second;

    return default_request_config;
}

} // request_route namespace
} // ouinet namespace
