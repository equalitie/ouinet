#pragma once
#include <string>
#include <vector>

#if defined(__clang__)
#define SHARED_EXPORT __attribute__((visibility("default")))
#define SHARED_LOCAL __attribute__((visibility("hidden")))
#endif

#if defined(IS_BUILDING_SHARED)
#define API SHARED_EXPORT
#else
#define API
#endif

class API NativeLib
{
public:
  int getClientState();
  std::string helloOuinet();
  void startClient(const std::vector<std::string>& args);
  void stopClient();
  std::string getProxyEndpoint() const noexcept;
  std::string getFrontendEndpoint() const noexcept;
};
