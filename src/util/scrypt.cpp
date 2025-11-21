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
        ScryptParams params)
{
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_SCRYPT, NULL);

    auto on_exit = defer([pctx] {
        EVP_PKEY_CTX_free(pctx);
    });

    if (EVP_PKEY_derive_init(pctx) <= 0) {
        throw std::runtime_error("Failure in Scrypt: EVP_PKEY_derive_init");
    }
    if (EVP_PKEY_CTX_set_scrypt_N(pctx, params.N) <= 0) {
        throw std::runtime_error("Failure in Scrypt: EVP_PKEY_CTX_set_scrypt_N");
    }
    if (EVP_PKEY_CTX_set_scrypt_r(pctx, params.r) <= 0) {
        throw std::runtime_error("Failure in Scrypt: EVP_PKEY_CTX_set_scrypt_r");
    }
    if (EVP_PKEY_CTX_set_scrypt_p(pctx, params.p) <= 0) {
        throw std::runtime_error("Failure in Scrypt: EVP_PKEY_CTX_set_scrypt_p");
    }
    if (EVP_PKEY_CTX_set1_pbe_pass(pctx, password.data(), password.size()) <= 0) {
        throw std::runtime_error("Failure in Scrypt: EVP_PKEY_CTX_set1_pbe_pass");
    }
    if (EVP_PKEY_CTX_set1_scrypt_salt(pctx, (unsigned char*) salt.data(), salt.size()) <= 0) {
        throw std::runtime_error("Failure in Scrypt: EVP_PKEY_CTX_set1_scrypt_salt");
    }
    if (EVP_PKEY_derive(pctx, out_data, &out_size) <= 0) {
        throw std::runtime_error("Failure in Scrypt: EVP_PKEY_derive");
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
        asio::yield_context yield)
{
    async_initiate<asio::yield_context, void(std::exception_ptr)> (
        [
            impl = _impl,
            password,
            salt,
            params,
            output_data,
            output_size,
            exec = yield.get_executor()
        ] (auto completion_handler) mutable {
            asio::post(impl->thread_exec,
            [ password,
              salt,
              params,
              output_data,
              output_size,
              handler = std::move(completion_handler),
              exec = std::move(exec)
            ] () mutable {
                std::exception_ptr e;
                try {
                    scrypt_derive(password, salt, output_data, output_size, params);
                } catch (...) {
                    e = std::current_exception();
                }
                asio::post(exec, [h = std::move(handler), e] () mutable { h(e); });
            });
        },
        yield
    );
}

} // namespace ouinet::util
