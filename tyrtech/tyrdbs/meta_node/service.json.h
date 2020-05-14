#pragma once


#include <io/channel.h>
#include <net/server_exception.h>
#include <net/service.json.h>

#include <tyrdbs/meta_node/log_module.json.h>


namespace tyrtech::tyrdbs::meta_node {


template<
    typename logImpl
>
struct service : private tyrtech::disallow_copy
{
    log::module<logImpl> log;

    service(
        logImpl* log
    )
      : log(log)
    {
    }

    struct context : private tyrtech::disallow_copy
    {
        typename logImpl::context log_ctx;

        context(
            typename logImpl::context&& log_ctx
        )
          : log_ctx(std::move(log_ctx))
        {
        }
    };

    context create_context(const std::shared_ptr<tyrtech::io::channel>& remote)
    {
        return context(
            log.create_context(remote)
        );
    }

    void process_message(const tyrtech::net::service::request_parser& service_request,
                         tyrtech::net::service::response_builder* service_response,
                         context* ctx)
    {
        switch (service_request.module())
        {
            case log::id:
            {
                log.process_message(service_request, service_response, &ctx->log_ctx);

                break;
            }
            default:
            {
                throw tyrtech::net::unknown_module_exception("#{}: unknown module", service_request.function());
            }
        }
    }
};

}
