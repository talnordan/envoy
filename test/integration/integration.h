#pragma once

#include <functional>
#include <list>
#include <string>
#include <vector>

#include "common/http/codec_client.h"

#include "test/config/utility.h"
#include "test/integration/fake_upstream.h"
#include "test/integration/server.h"
#include "test/integration/utility.h"
#include "test/mocks/buffer/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/printers.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/test_time.h"
#include "common/api/api_impl.h"
#include "common/event/dispatcher_impl.h"
#include "test/integration/autonomous_upstream.h"
#include "test/test_common/network_utility.h"

#include "spdlog/spdlog.h"

namespace Envoy {

/**
 * Stream decoder wrapper used during integration testing.
 */
class IntegrationStreamDecoder : public Http::StreamDecoder, public Http::StreamCallbacks {
public:
  IntegrationStreamDecoder(Event::Dispatcher& dispatcher);

  const std::string& body() { return body_; }
  bool complete() { return saw_end_stream_; }
  bool reset() { return saw_reset_; }
  Http::StreamResetReason reset_reason() { return reset_reason_; }
  const Http::HeaderMap* continue_headers() { return continue_headers_.get(); }
  const Http::HeaderMap& headers() { return *headers_; }
  const Http::HeaderMapPtr& trailers() { return trailers_; }
  void waitForContinueHeaders();
  void waitForHeaders();
  void waitForBodyData(uint64_t size);
  void waitForEndStream();
  void waitForReset();

  // Http::StreamDecoder
  void decode100ContinueHeaders(Http::HeaderMapPtr&& headers) override;
  void decodeHeaders(Http::HeaderMapPtr&& headers, bool end_stream) override;
  void decodeData(Buffer::Instance& data, bool end_stream) override;
  void decodeTrailers(Http::HeaderMapPtr&& trailers) override;
  void decodeMetadata(Http::MetadataMapPtr&&) override {}

  // Http::StreamCallbacks
  void onResetStream(Http::StreamResetReason reason) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

private:
  Event::Dispatcher& dispatcher_;
  Http::HeaderMapPtr continue_headers_;
  Http::HeaderMapPtr headers_;
  Http::HeaderMapPtr trailers_;
  bool waiting_for_end_stream_{};
  bool saw_end_stream_{};
  std::string body_;
  uint64_t body_data_waiting_length_{};
  bool waiting_for_reset_{};
  bool waiting_for_continue_headers_{};
  bool waiting_for_headers_{};
  bool saw_reset_{};
  Http::StreamResetReason reset_reason_{};
};

typedef std::unique_ptr<IntegrationStreamDecoder> IntegrationStreamDecoderPtr;

/**
 * TCP client used during integration testing.
 */
class IntegrationTcpClient {
public:
  IntegrationTcpClient(Event::Dispatcher& dispatcher, MockBufferFactory& factory, uint32_t port,
                       Network::Address::IpVersion version, bool enable_half_close = false);

  void close();
  void waitForData(const std::string& data, bool exact_match = true);
  void waitForDisconnect(bool ignore_spurious_events = false);
  void waitForHalfClose();
  void readDisable(bool disabled);
  void write(const std::string& data, bool end_stream = false, bool verify = true);
  const std::string& data() { return payload_reader_->data(); }
  bool connected() const { return !disconnected_; }

private:
  struct ConnectionCallbacks : public Network::ConnectionCallbacks {
    ConnectionCallbacks(IntegrationTcpClient& parent) : parent_(parent) {}

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override;
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    IntegrationTcpClient& parent_;
  };

  std::shared_ptr<WaitForPayloadReader> payload_reader_;
  std::shared_ptr<ConnectionCallbacks> callbacks_;
  Network::ClientConnectionPtr connection_;
  bool disconnected_{};
  MockWatermarkBuffer* client_write_buffer_;
};

typedef std::unique_ptr<IntegrationTcpClient> IntegrationTcpClientPtr;

struct ApiFilesystemConfig {
  std::string bootstrap_path_;
  std::string cds_path_;
  std::string eds_path_;
  std::string lds_path_;
  std::string rds_path_;
};

/**
 * Test fixture for all integration tests.
 */
template<typename TimeSystemType>
class BaseIntegrationTest : Logger::Loggable<Logger::Id::testing> {
  static_assert(std::is_base_of<Event::TestTimeSystem, TimeSystemType>::value,
  "TimeSystemType must be a subclass of Event::TestTimeSystem");

public:
  using TestTimeSystemPtr = std::unique_ptr<Event::TestTimeSystem>;

  BaseIntegrationTest(Network::Address::IpVersion version,
                      const std::string& config = ConfigHelper::HTTP_PROXY_CONFIG)
                          : api_(new Api::Impl(std::chrono::milliseconds(10000))),
      mock_buffer_factory_(new testing::NiceMock<MockBufferFactory>), time_system_(std::make_unique<TimeSystemType>()),
      dispatcher_(new Event::DispatcherImpl(*time_system_,
                                            Buffer::WatermarkFactoryPtr{mock_buffer_factory_})),
      version_(version), config_helper_(version, config),
      default_log_level_(TestEnvironment::getOptions().logLevel()) {
  // This is a hack, but there are situations where we disconnect fake upstream connections and
  // then we expect the server connection pool to get the disconnect before the next test starts.
  // This does not always happen. This pause should allow the server to pick up the disconnect
  // notification and clear the pool connection if necessary. A real fix would require adding fairly
  // complex test hooks to the server and/or spin waiting on stats, neither of which I think are
  // necessary right now.
  time_system_->sleep(std::chrono::milliseconds(10));
  ON_CALL(*mock_buffer_factory_, create_(_, _))
      .WillByDefault(Invoke([](std::function<void()> below_low,
                               std::function<void()> above_high) -> Buffer::Instance* {
        return new Buffer::WatermarkBuffer(below_low, above_high);
      }));
}

  virtual ~BaseIntegrationTest() {}

  /**
   * Helper function to create a simulated time integration test during construction.
   */
  static TestTimeSystemPtr simTime() { return std::make_unique<Event::SimulatedTimeSystem>(); }

  /**
   * Helper function to create a wall-clock time integration test during construction.
   */
  static TestTimeSystemPtr realTime() { return std::make_unique<Event::TestRealTimeSystem>(); }

  // Initialize the basic proto configuration, create fake upstreams, and start Envoy.
  virtual void initialize()
   {
  RELEASE_ASSERT(!initialized_, "");
  RELEASE_ASSERT(Event::Libevent::Global::initialized(), "");
  initialized_ = true;

  createUpstreams();
  createEnvoy();
}
  // Set up the fake upstream connections. This is called by initialize() and
  // is virtual to allow subclass overrides.
  virtual void createUpstreams() {
  for (uint32_t i = 0; i < fake_upstreams_count_; ++i) {
    if (autonomous_upstream_) {
      fake_upstreams_.emplace_back(
          new AutonomousUpstream(0, upstream_protocol_, version_, *time_system_));
    } else {
      fake_upstreams_.emplace_back(
          new FakeUpstream(0, upstream_protocol_, version_, *time_system_, enable_half_close_));
    }
  }
}
  // Finalize the config and spin up an Envoy instance.
  virtual void createEnvoy() {
  std::vector<uint32_t> ports;
  for (auto& upstream : fake_upstreams_) {
    if (upstream->localAddress()->ip()) {
      ports.push_back(upstream->localAddress()->ip()->port());
    }
  }
  config_helper_.finalize(ports);

  ENVOY_LOG_MISC(debug, "Running Envoy with configuration {}",
                 config_helper_.bootstrap().DebugString());

  const std::string bootstrap_path = TestEnvironment::writeStringToFileForTest(
      "bootstrap.json", MessageUtil::getJsonStringFromMessage(config_helper_.bootstrap()));

  std::vector<std::string> named_ports;
  const auto& static_resources = config_helper_.bootstrap().static_resources();
  for (int i = 0; i < static_resources.listeners_size(); ++i) {
    named_ports.push_back(static_resources.listeners(i).name());
  }
  createGeneratedApiTestServer(bootstrap_path, named_ports);
}
  // Sets upstream_protocol_ and alters the upstream protocol in the config_helper_
  void setUpstreamProtocol(FakeHttpConnection::Type protocol) {
  upstream_protocol_ = protocol;
  if (upstream_protocol_ == FakeHttpConnection::Type::HTTP2) {
    config_helper_.addConfigModifier(
        [&](envoy::config::bootstrap::v2::Bootstrap& bootstrap) -> void {
          RELEASE_ASSERT(bootstrap.mutable_static_resources()->clusters_size() >= 1, "");
          auto* cluster = bootstrap.mutable_static_resources()->mutable_clusters(0);
          cluster->mutable_http2_protocol_options();
        });
  } else {
    RELEASE_ASSERT(protocol == FakeHttpConnection::Type::HTTP1, "");
  }
}
  // Sets fake_upstreams_count_ and alters the upstream protocol in the config_helper_
  void setUpstreamCount(uint32_t count) { fake_upstreams_count_ = count; }
  // Make test more deterministic by using a fixed RNG value.
  void setDeterministic() { deterministic_ = true; }

  FakeHttpConnection::Type upstreamProtocol() const { return upstream_protocol_; }

  IntegrationTcpClientPtr makeTcpConnection(uint32_t port) {
  return std::make_unique<IntegrationTcpClient>(*dispatcher_, *mock_buffer_factory_, port, version_,
                                                enable_half_close_);
}

  // Test-wide port map.
  void registerPort(const std::string& key, uint32_t port) {
  port_map_[key] = port;
}
  uint32_t lookupPort(const std::string& key) {
  auto it = port_map_.find(key);
  if (it != port_map_.end()) {
    return it->second;
  }
  RELEASE_ASSERT(false, "");
}


  // Set the endpoint's socket address to point at upstream at given index.
  void setUpstreamAddress(uint32_t upstream_index,
                          envoy::api::v2::endpoint::LbEndpoint& endpoint) const {
  auto* socket_address = endpoint.mutable_endpoint()->mutable_address()->mutable_socket_address();
  socket_address->set_address(Network::Test::getLoopbackAddressString(version_));
  socket_address->set_port_value(fake_upstreams_[upstream_index]->localAddress()->ip()->port());
}


  Network::ClientConnectionPtr makeClientConnection(uint32_t port) {
  Network::ClientConnectionPtr connection(dispatcher_->createClientConnection(
      Network::Utility::resolveUrl(
          fmt::format("tcp://{}:{}", Network::Test::getLoopbackAddressUrlString(version_), port)),
      Network::Address::InstanceConstSharedPtr(), Network::Test::createRawBufferSocket(), nullptr));

  connection->enableHalfClose(enable_half_close_);
  return connection;
}

  void registerTestServerPorts(const std::vector<std::string>& port_names) {
  auto port_it = port_names.cbegin();
  auto listeners = test_server_->server().listenerManager().listeners();
  auto listener_it = listeners.cbegin();
  for (; port_it != port_names.end() && listener_it != listeners.end(); ++port_it, ++listener_it) {
    const auto listen_addr = listener_it->get().socket().localAddress();
    if (listen_addr->type() == Network::Address::Type::Ip) {
      registerPort(*port_it, listen_addr->ip()->port());
    }
  }
  const auto admin_addr = test_server_->server().admin().socket().localAddress();
  if (admin_addr->type() == Network::Address::Type::Ip) {
    registerPort("admin", admin_addr->ip()->port());
  }
}
  void createTestServer(const std::string& json_path, const std::vector<std::string>& port_names) {
  test_server_ = createIntegrationTestServer(
      TestEnvironment::temporaryFileSubstitute(json_path, port_map_, version_), nullptr,
      *time_system_);
  registerTestServerPorts(port_names);
}
  void createGeneratedApiTestServer(const std::string& bootstrap_path,
                                    const std::vector<std::string>& port_names) {
  test_server_ = IntegrationTestServer::create(
      bootstrap_path, version_, pre_worker_start_test_steps_, deterministic_, *time_system_, *api_);
  if (config_helper_.bootstrap().static_resources().listeners_size() > 0) {
    // Wait for listeners to be created before invoking registerTestServerPorts() below, as that
    // needs to know about the bound listener ports.
    test_server_->waitForCounterGe("listener_manager.listener_create_success", 1);
    registerTestServerPorts(port_names);
  }
}
  void createApiTestServer(const ApiFilesystemConfig& api_filesystem_config,
                           const std::vector<std::string>& port_names) {
  const std::string eds_path = TestEnvironment::temporaryFileSubstitute(
      api_filesystem_config.eds_path_, port_map_, version_);
  const std::string cds_path = TestEnvironment::temporaryFileSubstitute(
      api_filesystem_config.cds_path_, {{"eds_json_path", eds_path}}, port_map_, version_);
  const std::string rds_path = TestEnvironment::temporaryFileSubstitute(
      api_filesystem_config.rds_path_, port_map_, version_);
  const std::string lds_path = TestEnvironment::temporaryFileSubstitute(
      api_filesystem_config.lds_path_, {{"rds_json_path", rds_path}}, port_map_, version_);
  createGeneratedApiTestServer(TestEnvironment::temporaryFileSubstitute(
                                   api_filesystem_config.bootstrap_path_,
                                   {{"cds_json_path", cds_path}, {"lds_json_path", lds_path}},
                                   port_map_, version_),
                               port_names);
}

  Event::TestTimeSystem& timeSystem() { return *time_system_; }

  Api::ApiPtr api_;
  MockBufferFactory* mock_buffer_factory_; // Will point to the dispatcher's factory.
private:
  TestTimeSystemPtr time_system_;

public:
  Event::DispatcherPtr dispatcher_;

  /**
   * Open a connection to Envoy, send a series of bytes, and return the
   * response. This function will continue reading response bytes until Envoy
   * closes the connection (as a part of error handling) or (if configured true)
   * the complete headers are read.
   *
   * @param port the port to connect to.
   * @param raw_http the data to send.
   * @param response the response data will be sent here
   * @param if the connection should be terminated onece '\r\n\r\n' has been read.
   **/
  void sendRawHttpAndWaitForResponse(int port, const char* raw_http, std::string* response,
                                     bool disconnect_after_headers_complete = false) {
  Buffer::OwnedImpl buffer(raw_http);
  RawConnectionDriver connection(
      port, buffer,
      [&](Network::ClientConnection& client, const Buffer::Instance& data) -> void {
        response->append(data.toString());
        if (disconnect_after_headers_complete && response->find("\r\n\r\n") != std::string::npos) {
          client.close(Network::ConnectionCloseType::NoFlush);
        }
      },
      version_);

  connection.run();
}

protected:
  // Create the envoy server in another thread and start it.
  // Will not return until that server is listening.
  virtual IntegrationTestServerPtr
  createIntegrationTestServer(const std::string& bootstrap_path,
                              std::function<void()> pre_worker_start_steps,
                              Event::TestTimeSystem& time_system) {
  return IntegrationTestServer::create(bootstrap_path, version_, pre_worker_start_steps,
                                       deterministic_, time_system, *api_);
}

  bool initialized() const { return initialized_; }

  // The IpVersion (IPv4, IPv6) to use.
  Network::Address::IpVersion version_;
  // The config for envoy start-up.
  ConfigHelper config_helper_;
  // Steps that should be done prior to the workers starting. E.g., xDS pre-init.
  std::function<void()> pre_worker_start_test_steps_;

  std::vector<std::unique_ptr<FakeUpstream>> fake_upstreams_;
  // Target number of upstreams.
  uint32_t fake_upstreams_count_{1};
  spdlog::level::level_enum default_log_level_;
  IntegrationTestServerPtr test_server_;
  // A map of keys to port names. Generally the names are pulled from the v2 listener name
  // but if a listener is created via ADS, it will be from whatever key is used with registerPort.
  TestEnvironment::PortMap port_map_;

  // If true, use AutonomousUpstream for fake upstreams.
  bool autonomous_upstream_{false};

  bool enable_half_close_{false};

  // True if test will use a fixed RNG value.
  bool deterministic_{};

private:
  // The type for the Envoy-to-backend connection
  FakeHttpConnection::Type upstream_protocol_{FakeHttpConnection::Type::HTTP1};
  // True if initialized() has been called.
  bool initialized_{};
};

} // namespace Envoy
