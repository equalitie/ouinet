#include "scrypt.h"
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <iostream>
#include <thread>
#include <boost/asio/executor_work_guard.hpp>
#include "defer.h"

namespace ouinet::util {

static
void scrypt_derive(
        std::string_view password,
        std::string_view salt,
        uint8_t* out_data,
        size_t out_size,
        ScryptParams params,
        sys::error_code& ec_out)
{
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_SCRYPT, NULL);

    auto on_exit = defer([pctx] {
        EVP_PKEY_CTX_free(pctx);
    });

    if (EVP_PKEY_derive_init(pctx) <= 0) {
        ec_out = make_error_code(ScryptError::init);
        return;
    }
    if (EVP_PKEY_CTX_set_scrypt_N(pctx, params.N) <= 0) {
        ec_out = make_error_code(ScryptError::set_N);
        return;
    }
    if (EVP_PKEY_CTX_set_scrypt_r(pctx, params.r) <= 0) {
        ec_out = make_error_code(ScryptError::set_r);
        return;
    }
    if (EVP_PKEY_CTX_set_scrypt_p(pctx, params.p) <= 0) {
        ec_out = make_error_code(ScryptError::set_p);
        return;
    }
    if (EVP_PKEY_CTX_set1_pbe_pass(pctx, password.data(), password.size()) <= 0) {
        ec_out = make_error_code(ScryptError::set_pass);
        return;
    }
    if (EVP_PKEY_CTX_set1_scrypt_salt(pctx, (unsigned char*) salt.data(), salt.size()) <= 0) {
        ec_out = make_error_code(ScryptError::set_salt);
        return;
    }
    if (EVP_PKEY_derive(pctx, out_data, &out_size) <= 0) {
        ec_out = make_error_code(ScryptError::derive);
    }
}

ScryptWorker ScryptWorker::global_worker;

using WorkGuard = asio::executor_work_guard<asio::any_io_executor>;

struct ScryptWorker::Impl {
    asio::any_io_executor thread_exec;
    WorkGuard work_guard;
    std::thread thread;

    Impl(asio::any_io_executor thread_exec, WorkGuard work_guard, std::thread thread) :
        thread_exec(std::move(thread_exec)),
        work_guard(std::move(work_guard)),
        thread(std::move(thread))
    {}

    ~Impl() {
        work_guard.reset();
        thread.join();
    }
};

ScryptWorker::ScryptWorker() {
    auto ctx = std::make_unique<asio::io_context>();
    auto thread_exec = ctx->get_executor();
    WorkGuard work_guard(thread_exec);

    _impl = std::make_shared<Impl>(
        std::move(thread_exec),
        std::move(work_guard),
        std::thread([ctx = std::move(ctx)] () { ctx->run(); })
    );
}

void ScryptWorker::derive(
        std::string_view password,
        std::string_view salt,
        ScryptParams params,
        uint8_t* output_data,
        size_t output_size,
        YieldContext yield)
{
    asio::yield_context native_yield = yield.native();

    async_initiate<asio::yield_context, void(sys::error_code)> (
        [
            impl = _impl,
            password,
            salt,
            params,
            output_data,
            output_size,
            work = asio::make_work_guard(yield.get_executor())
        ] (auto completion_handler) mutable {
            asio::post(impl->thread_exec,
            [ password,
              salt,
              params,
              output_data,
              output_size,
              handler = std::move(completion_handler),
              work = std::move(work)
            ] () mutable {
                sys::error_code ec;
                scrypt_derive(password, salt, output_data, output_size, params, ec);
                asio::post(work.get_executor(), [h = std::move(handler), ec] () mutable { h(ec); });
            });
        },
        native_yield
    );
}

class ScryptErrorCategory: public sys::error_category {
public:
    const char * name() const noexcept {
        return "Scrypt (KDF) error";
    }

    std::string message( int ev ) const {
        char buffer[ 64 ];
        return this->message( ev, buffer, sizeof(buffer));
    }

    char const * message( int ev, char * buffer, std::size_t len ) const noexcept {
        switch(static_cast<ScryptError>(ev))
        {
            case ScryptError::success:  return "no error";
            case ScryptError::init:     return "init";
            case ScryptError::set_N:    return "set_N";
            case ScryptError::set_r:    return "set_r";
            case ScryptError::set_p:    return "set_p";
            case ScryptError::set_pass: return "set_pass";
            case ScryptError::set_salt: return "set_salt";
            case ScryptError::derive:   return "derive";
        }

        std::snprintf( buffer, len, "Unknown error %d", ev );
        return buffer;
    }
};

sys::error_category const& scrypt_error_category() {
    static const ScryptErrorCategory instance;
    return instance;
}

} // namespace ouinet::util
