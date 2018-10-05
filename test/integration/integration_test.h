#pragma once

#include "test/integration/http_integration.h"

#include "gtest/gtest.h"

namespace Envoy {
class IntegrationTest : public HttpIntegrationTest<Event::TestRealTimeSystem>,
                        public testing::TestWithParam<Network::Address::IpVersion> {
public:
  IntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {}
};
} // namespace Envoy
