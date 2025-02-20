#include <task.h>
#include <namespaces.h>
#include "record_processor.h"

namespace ouinet::metrics::bridge {

// Called from rust to pass record data to the use provided `async_callback`.
void CxxRecordProcessor::execute( rust::String report_name
                                , rust::String report_content
                                , rust::Box<bridge::CxxOneShotSender> on_finish) const
{
    task::spawn_detached(executor
            , [ async_callback = async_callback
              , report_name = move(report_name)
              , report_content = move(report_content)
              , on_finish = move(on_finish)
              ] (asio::yield_context yield)
    {
        string_view report_name_sv(report_name.data(), report_name.size());
        string_view report_content_sv(report_content.data(), report_content.size());

        try {
            async_callback(report_name_sv, report_content_sv, yield);
        }
        catch (std::exception& e) {
            on_finish->send(false);
            return;
        }

        on_finish->send(true);
    });
}

} // namespace
