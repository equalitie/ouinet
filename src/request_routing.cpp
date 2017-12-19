#include "request_routing.h"
#include "error.h"

using namespace ouinet;

using Request = http::request<http::string_body>;

// Route the provided request according to the given list of mechanisms.
class SimpleRequestRouter : public RequestRouter {
    private:
        const Request req;
        const std::vector<enum request_mechanism>& req_mechs;
        std::vector<enum request_mechanism>::const_iterator req_mech;

    public:
        SimpleRequestRouter( const Request& r
                           , const std::vector<enum request_mechanism>& rmechs)
            : req(r), req_mechs(rmechs), req_mech(std::begin(req_mechs)) { }

        enum request_mechanism get_next_mechanism(sys::error_code&) override;
};

//------------------------------------------------------------------------------
enum request_mechanism
SimpleRequestRouter::get_next_mechanism(sys::error_code& ec)
{
    ec = sys::error_code();

    // Check whether possible routing mechanisms have been exhausted.
    if (req_mech == std::end(req_mechs)) {
        ec = error::make_error_code(error::no_more_routes);
        return request_mechanism::_unknown;
    }

    // Send non-safe HTTP method requests to the origin server
    if (req.method() != http::verb::get && req.method() != http::verb::head) {
        return request_mechanism::origin;
    }

    // Use the following configured mechanism and prepare for the next one.
    return *(req_mech++);
}


namespace ouinet {

class ReqExpr {
    public:
        virtual ~ReqExpr() { }

        virtual bool match(const http::request<http::string_body>&) const = 0;
};

class RegexReqExpr : public ReqExpr {  // can match a request field against a regular expression
    public:
        // The type of functions that retrieve a given field from a request.
        typedef typename std::function<beast::string_view (const http::request<http::string_body>&)> field_getter;

        static field_getter target_getter() {
            return [](const http::request<http::string_body>& r) {return r.target();};
        }
        static field_getter header_getter(const std::string& h) {
            return [=](const http::request<http::string_body>& r) {return r[h];};  // TODO check capture mode
        }
        static field_getter method_getter() {
            return [](const http::request<http::string_body>& r) {return r.method_string();};
        }

    private:
        const field_getter& get_field;
        const boost::regex regexp;

    public:
        RegexReqExpr(const field_getter& gf, const boost::regex& rx)
            : get_field(gf), regexp(rx) { };

        RegexReqExpr(const field_getter& gf, const std::string& rx)
            : RegexReqExpr(gf, boost::regex(rx)) { };

        bool match(const http::request<http::string_body>& req) const {
            return boost::regex_match(get_field(req).to_string(), regexp);
        }
};

class TrueReqExpr : public ReqExpr {  // matches all requests
        bool match(const http::request<http::string_body>& req) const {
            return true;
        }
};

class FalseReqExpr : public ReqExpr {  // matches no request
        bool match(const http::request<http::string_body>& req) const {
            return false;
        }
};

class NotReqExpr : public ReqExpr {  // negates match of subexpr
    private:
        const std::shared_ptr<ReqExpr> child;

    public:
        NotReqExpr(const std::shared_ptr<ReqExpr> sub)
            : child(sub) { }

        bool match(const http::request<http::string_body>& req) const {
            return !(child->match(req));
        }
};

class AndReqExpr : public ReqExpr {  // a shortcircuit logical AND of two subexprs
    private:
        const std::shared_ptr<ReqExpr> left, right;

    public:
        AndReqExpr(const std::shared_ptr<ReqExpr> left_, const std::shared_ptr<ReqExpr> right_)
            : left(left_), right(right_) { }

        bool match(const http::request<http::string_body>& req) const {
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

        bool match(const http::request<http::string_body>& req) const {
            if (left->match(req))
                return true;
            return right->match(req);
        }
};

//------------------------------------------------------------------------------
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<enum request_mechanism>& rmechs)
{
    return std::make_unique<SimpleRequestRouter>(req, rmechs);
}

//------------------------------------------------------------------------------
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<std::pair<const reqexpr::ReqExpr2&, const std::vector<enum request_mechanism>&>>& matches
     , const std::vector<enum request_mechanism>& def_rmechs )
{
    // Delegate to a simple router
    // with the mechanisms associated with the matching expression (if any),
    // or with `def_rmechs` if none does.
    for (auto mit = matches.begin(); mit != matches.end(); ++mit)
        if (mit->first.match(req))
            return std::make_unique<SimpleRequestRouter>(req, mit->second);
    return std::make_unique<SimpleRequestRouter>(req, def_rmechs);
}

namespace reqexpr {

bool
ReqExpr2::match(const http::request<http::string_body>& req) const {
    return impl->match(req);
}

ReqExpr2
true_()
{
    return ReqExpr2(std::make_shared<TrueReqExpr>());
}

ReqExpr2
false_()
{
    return ReqExpr2(std::make_shared<FalseReqExpr>());
}

ReqExpr2
from_regex(const field_getter& gf, const boost::regex& rx)
{
    return ReqExpr2(std::make_shared<RegexReqExpr>(gf, rx));
}

ReqExpr2
from_regex(const field_getter& gf, const std::string& rx)
{
    return from_regex(gf, boost::regex(rx));
}

ReqExpr2
operator!(const ReqExpr2& sub)
{
    return ReqExpr2(std::make_shared<NotReqExpr>(sub.impl));
}

ReqExpr2
operator&&(const ReqExpr2& left, const ReqExpr2& right)
{
    return ReqExpr2(std::make_shared<AndReqExpr>(left.impl, right.impl));
}

ReqExpr2
operator||(const ReqExpr2& left, const ReqExpr2& right)
{
    return ReqExpr2(std::make_shared<OrReqExpr>(left.impl, right.impl));
}

} // ouinet::reqexpr namespace

} // ouinet namespace
