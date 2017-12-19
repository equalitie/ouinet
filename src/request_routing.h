#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <boost/beast/core/string.hpp>
#include <boost/beast/http.hpp>
#include <boost/regex.hpp>

#include "namespaces.h"


namespace ouinet {

// The different mechanisms an HTTP request can be routed over.
enum request_mechanism {
    // These mechanisms may be configured by the user.
    origin,      // send request to the origin HTTP server
    proxy,       // send request to proxy ouiservice
    injector,    // send request to injector ouiservice
    cache,       // retrieve resource from the cache

    // The following entries are for internal use only.
    _unknown,    // used e.g. in case of errors
    _front_end,  // handle the request internally
};

// Request expressions can tell whether they match a given request
// (much like regular expressions match strings).
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

class AllReqExpr : public ReqExpr {  // a shortcut logical AND of all subexprs
    private:
        const std::vector<std::shared_ptr<ReqExpr>> children;

    public:
        AllReqExpr(const std::vector<std::shared_ptr<ReqExpr>>& subs)
            : children(subs.begin(), subs.end()) { }

        bool match(const http::request<http::string_body>& req) const {
            for (auto cit = children.cbegin(); cit != children.cend(); ++cit)
              if (!((*cit)->match(req)))
                return false;
            return true;
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

class AnyReqExpr : public ReqExpr {  // a shortcut logical OR of all subexprs
    private:
        const std::vector<std::shared_ptr<ReqExpr>> children;

    public:
        AnyReqExpr(const std::vector<std::shared_ptr<ReqExpr>>& subs)
            : children(subs.begin(), subs.end()) { }

        bool match(const http::request<http::string_body>& req) const {
            for (auto cit = children.cbegin(); cit != children.cend(); ++cit)
              if ((*cit)->match(req))
                return true;
            return false;
        }
};

namespace reqexpr {
class ReqExpr2 {
    friend ReqExpr2 true_();
    friend ReqExpr2 false_();
    friend ReqExpr2 from_regex(const RegexReqExpr::field_getter&, const boost::regex&);
    friend ReqExpr2 operator!(const ReqExpr2&);
    friend ReqExpr2 operator&&(const ReqExpr2&, const ReqExpr2&);
    friend ReqExpr2 operator||(const ReqExpr2&, const ReqExpr2&);

    private:
        const std::shared_ptr<ReqExpr> impl;
        ReqExpr2(const std::shared_ptr<ReqExpr> impl_) : impl(impl_) { }

    public:
        bool match(const http::request<http::string_body>& req) const {
            return impl->match(req);
        }
};

ReqExpr2 true_();
ReqExpr2 false_();
ReqExpr2 from_regex(const RegexReqExpr::field_getter&, const boost::regex&);
ReqExpr2 from_regex(const RegexReqExpr::field_getter&, const std::string&);

ReqExpr2 operator!(const ReqExpr2&);

ReqExpr2 operator&&(const ReqExpr2&, const ReqExpr2&);
ReqExpr2 operator||(const ReqExpr2&, const ReqExpr2&);
} // ouinet::reqexpr namespace

// A request router holds the context and rules to decide the different mechanisms
// a request should be routed to until it finally succeeds,
// considering previous attempts.
class RequestRouter {
    public:
        virtual ~RequestRouter() { }

        // Decide which access mechanism to use for the given request.
        // If no more mechanisms can be attempted, return `request_mechanism::unknown`
        // and set the error code to `error::no_more_routes`.
        virtual enum request_mechanism get_next_mechanism(sys::error_code&) = 0;
};

// Route the provided request according to the given list of mechanisms.
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<enum request_mechanism>& rmechs);

// Route the provided request according to the list of mechanisms associated
// with the first matching expression in the given list,
// otherwise route it according to the given list of default mechanisms.
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<std::pair<const reqexpr::ReqExpr2&, const std::vector<enum request_mechanism>&>>& matches
     , const std::vector<enum request_mechanism>& def_rmechs );

} // ouinet namespace
