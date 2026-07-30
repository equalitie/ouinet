#pragma once

#include "response_reader.h"

#include <expected>
#include <optional>
#include <memory>

namespace ouinet {

class Async;
class Session;
class CacheRequest;
namespace cache { class Client; }

// Implements `AbstractReader` while reading from `Session` and stores each
// `http_response::Part` that it reads into the cache FS storage.
class StoringReader : public http_response::AbstractReader {
public:
    using Part = http_response::Part;

    StoringReader(const CacheRequest&, Session, std::shared_ptr<cache::Client>);

    StoringReader(const StoringReader&) = delete;
    StoringReader& operator=(const StoringReader&) = delete;

    StoringReader(StoringReader&&);
    StoringReader& operator=(StoringReader&&);

    [[nodiscard]]
    std::expected<
        std::optional<Part>,
        sys::error_code
    >
    async_read_part(Async yield) override;

    bool is_done() const override;

    void close() override;

    asio::any_io_executor get_executor() override;

    virtual ~StoringReader();

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};

} // namespace
