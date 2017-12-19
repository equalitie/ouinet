#include "request_routing.h"
#include "error.h"

using namespace ouinet;

using Request = http::request<http::string_body>;

//------------------------------------------------------------------------------
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

    // Use the following configured mechanism and prepare for the next one.
    return *(req_mech++);
}


//------------------------------------------------------------------------------
namespace ouinet {

namespace reqexpr {

class ReqExpr {
    public:
        virtual ~ReqExpr() { }

        virtual bool match(const http::request<http::string_body>&) const = 0;
};

class RegexReqExpr : public ReqExpr {  // can match a request field against a regular expression
    private:
        const field_getter& get_field;
        const boost::regex regexp;

    public:
        RegexReqExpr(const field_getter& gf, const boost::regex& rx)
            : get_field(gf), regexp(rx) { };

        bool match(const http::request<http::string_body>& req) const {
            return boost::regex_match(get_field(req).to_string(), regexp);
        }
};

class TrueReqExpr : public ReqExpr {  // matches all requests
    public:
        bool match(const http::request<http::string_body>& req) const {
            return true;
        }
};

class FalseReqExpr : public ReqExpr {  // matches no request
    public:
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
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<enum request_mechanism>& rmechs)
{
    return std::make_unique<SimpleRequestRouter>(req, rmechs);
}

//------------------------------------------------------------------------------
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<std::pair<const reqexpr::reqex&, const std::vector<enum request_mechanism>&>>& matches
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

} // ouinet namespace
