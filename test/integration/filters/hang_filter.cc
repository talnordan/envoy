#include <future>

#include "envoy/registry/registry.h"

#include "extensions/filters/http/common/empty_http_filter_config.h"
#include "extensions/filters/http/common/pass_through_filter.h"

namespace Envoy {

// A test filter that hangs indefinitely.
class HangFilter : public Http::PassThroughDecoderFilter {
public:
  Http::FilterHeadersStatus decodeHeaders(Http::HeaderMap&, bool) override {
    hangIndefinitely();
    return Http::FilterHeadersStatus::Continue;
  }

private:
  static inline void hangIndefinitely() { std::promise<void>().get_future().wait(); }
};

class HangFilterConfig : public Extensions::HttpFilters::Common::EmptyHttpFilterConfig {
public:
  HangFilterConfig() : EmptyHttpFilterConfig("hang-filter") {}

  Http::FilterFactoryCb createFilter(const std::string&, Server::Configuration::FactoryContext&) {
    return [](Http::FilterChainFactoryCallbacks& callbacks) -> void {
      callbacks.addStreamDecoderFilter(std::make_shared<HangFilter>());
    };
  }
};

// perform static registration
static Registry::RegisterFactory<HangFilterConfig,
                                 Server::Configuration::NamedHttpFilterConfigFactory>
    register_;

} // namespace Envoy
