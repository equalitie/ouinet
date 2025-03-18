#include <task.h>
#include <namespaces.h>
#include <boost/asio/buffer.hpp>
#include "record_processor.h"

namespace ouinet::metrics::bridge {

// Called from rust to pass record data to the use provided `async_callback`.
void CxxRecordProcessor::execute( rust::String report_name
                                , rust::Vec<rust::u8> report_content
                                , rust::Box<bridge::CxxOneShotSender> on_finish) const
{
    task::spawn_detached(executor
            , [ async_callback = async_callback
              , report_name = std::move(report_name)
              , report_content = std::move(report_content)
              , on_finish = std::move(on_finish)
              ] (asio::yield_context yield)
    {
        std::string_view report_name_view(report_name.data(), report_name.size());
        asio::const_buffer report_content_view(report_content.data(), report_content.size());

        try {
            async_callback(report_name_view, report_content_view, yield);
        }
        catch (std::exception& e) {
            on_finish->send(false);
            return;
        }

        on_finish->send(true);
    });
}

} // namespace
