#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/helpers.h"

namespace esphome {
namespace api {

namespace enums {

enum SupportsResponse : uint32_t {
  SUPPORTS_RESPONSE_NONE = 0,
};

enum ServiceArgType : uint32_t {
  SERVICE_ARG_TYPE_STRING = 0,
};

}  // namespace enums

template<typename T> class RepeatedVector : public std::vector<T> {
 public:
  void init(size_t) {}

  T &emplace_back() {
    std::vector<T>::emplace_back();
    return this->back();
  }
};

struct ListEntitiesServicesArgument {
  StringRef name;
  uint32_t type{enums::SERVICE_ARG_TYPE_STRING};
};

struct ListEntitiesServicesResponse {
  StringRef name;
  uint32_t key{0};
  uint32_t supports_response{enums::SUPPORTS_RESPONSE_NONE};
  RepeatedVector<ListEntitiesServicesArgument> args;
};

struct ExecuteServiceArgument {
  StringRef string_;
};

struct ExecuteServiceRequest {
  uint32_t key{0};
  RepeatedVector<ExecuteServiceArgument> args;
};

class UserServiceDescriptor {
 public:
  virtual ~UserServiceDescriptor() = default;
  virtual ListEntitiesServicesResponse encode_list_service_response() = 0;
  virtual bool execute_service(const ExecuteServiceRequest &request) = 0;
#ifdef USE_API_USER_DEFINED_ACTION_RESPONSES
  virtual bool execute_service(const ExecuteServiceRequest &request, uint32_t response_id) = 0;
#endif
};

struct HomeassistantEvent {
  std::string event_type;
  std::map<std::string, std::string> data;
};

class APIServer {
 public:
  void register_user_service(UserServiceDescriptor *descriptor) { this->user_services_.push_back(descriptor); }

  bool is_connected() const { return this->connected_; }
  void set_connected(bool connected) { this->connected_ = connected; }

  void reset() {
    this->user_services_.clear();
    this->events_.clear();
    this->connected_ = true;
  }

  std::vector<UserServiceDescriptor *> user_services_;
  std::vector<HomeassistantEvent> events_;

 private:
  bool connected_{true};
};

inline APIServer *global_api_server = nullptr;

/// Test helper: installs `server` as global_api_server for the scope's lifetime and restores
/// the previous value on destruction — including on an ASSERT_*-triggered early return, which
/// still unwinds the stack and runs destructors. A bare `global_api_server = &server;` at the
/// top of a TEST() left the pointer dangling into that test's own stack frame once it
/// returned, so a later test with no api_server of its own would read freed stack memory
/// through the global (caught by ASan as stack-use-after-return).
class ScopedGlobalApiServer {
 public:
  explicit ScopedGlobalApiServer(APIServer &server) : previous_(global_api_server) { global_api_server = &server; }
  ~ScopedGlobalApiServer() { global_api_server = previous_; }
  ScopedGlobalApiServer(const ScopedGlobalApiServer &) = delete;
  ScopedGlobalApiServer &operator=(const ScopedGlobalApiServer &) = delete;

 private:
  APIServer *previous_;
};

class CustomAPIDevice {
 public:
  virtual ~CustomAPIDevice() = default;

  bool is_connected() const { return global_api_server != nullptr && global_api_server->is_connected(); }

  void fire_homeassistant_event(const std::string &event_type, const std::map<std::string, std::string> &data) {
    if (global_api_server == nullptr)
      return;
    global_api_server->events_.push_back(HomeassistantEvent{event_type, data});
  }
};

}  // namespace api
}  // namespace esphome