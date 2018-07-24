#include "request_routing.h"
#include "error.h"

using namespace ouinet;

using Request = http::request<http::string_body>;

//------------------------------------------------------------------------------
namespace ouinet {

namespace reqexpr {

class ReqExpr {
    public:
        ReqExpr() = default;
        virtual ~ReqExpr() = default;
        ReqExpr(ReqExpr&&) = default;
        ReqExpr& operator=(ReqExpr&&) = default;
        ReqExpr(const ReqExpr&) = default;
        ReqExpr& operator=(const ReqExpr&) = default;

        virtual bool match(const http::request<http::string_body>&) const = 0;
};

class RegexReqExpr : public ReqExpr {  // can match a request field against a regular expression
    private:
        const field_getter& get_field;
        const boost::regex regexp;

    public:
        RegexReqExpr(const field_getter& gf, const boost::regex& rx)
            : get_field(gf), regexp(rx) { };

        bool match(const http::request<http::string_body>& req) const override {
            return boost::regex_match(get_field(req).to_string(), regexp);
        }
};

class TrueReqExpr : public ReqExpr {  // matches all requests
    public:
        bool match(const http::request<http::string_body>& req) const override {
            return true;
        }
};

class FalseReqExpr : public ReqExpr {  // matches no request
    public:
        bool match(const http::request<http::string_body>& req) const override {
            return false;
        }
};

class NotReqExpr : public ReqExpr {  // negates match of subexpr
    private:
        const std::shared_ptr<ReqExpr> child;

    public:
        NotReqExpr(const std::shared_ptr<ReqExpr> sub)
            : child(sub) { }

        bool match(const http::request<http::string_body>& req) const override {
            return !(child->match(req));
        }
};

class AndReqExpr : public ReqExpr {  // a shortcircuit logical AND of two subexprs
    private:
        const std::shared_ptr<ReqExpr> left, right;

    public:
        AndReqExpr(const std::shared_ptr<ReqExpr> left_, const std::shared_ptr<ReqExpr> right_)
            : left(left_), right(right_) { }

        bool match(const http::request<http::string_body>& req) const override {
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

        bool match(const http::request<http::string_body>& req) const override {
            if (left->match(req))
                return true;
            return right->match(req);
        }
};

bool
reqex::match(const http::request<http::string_body>& req) const {
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
const Config&
route_choose_config( const http::request<http::string_body>& req
                   , const std::vector<std::pair<const reqexpr::reqex, const Config>>& matches
                   , const Config& default_config )
{
    // Delegate to a simple router
    // with the mechanisms associated with the matching expression (if any),
    // or with `default_config` if none does.
    for (auto mit = matches.begin(); mit != matches.end(); ++mit)
        if (mit->first.match(req))
            return mit->second;
    return default_config;
}

std::ostream& operator<<(std::ostream& os, responder r)
{
    switch (r) {
        case responder::origin:     return os << "origin";
        case responder::proxy:      return os << "proxy";
        case responder::injector:   return os << "injector";
        case responder::_front_end: return os << "_front_end";
    }
    return os << "?";
}

std::ostream& operator<<(std::ostream& os, const Config& c)
{
    os << "Config{enable_cache:"
       << (c.enable_cache ? "true" : "false");

    os << ", responders:[";

    if (c.responders.empty()) return os << "]}";

    auto q = c.responders;

    while (!q.empty()) {
        auto r = q.front();
        q.pop();
        os << r;
        if (!q.empty()) os << ", ";
    }

    return os << "]}";
}

} // request_route namespace
} // ouinet namespace
