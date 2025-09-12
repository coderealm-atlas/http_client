# HTTP Client

A monadic wrapper around Boost.Beast HTTP library.

## Usage

```cpp
#include "http_client_monad.hpp"

using namespace monad;

// Create HTTP client
cjj365::AppProperties app_properties{config_sources()};
auto http_client_config_provider = 
    std::make_shared<cjj365::HttpclientConfigProviderFile>(
        app_properties, config_sources());
cjj365::ClientSSLContext client_ssl_ctx(*http_client_config_provider);
auto http_client = std::make_unique<client_async::HttpClientManager>(
    client_ssl_ctx, *http_client_config_provider);

// Make request
http_io<GetStatusTag>("https://example.com")
    .map([&](auto ex) {
        ex->request.set(http::field::authorization, "Bearer token");
        ex->set_query_param("name", "world");
        return ex;
    })
    .then(http_request_io<GetStatusTag>(*http_client))
    .map([](auto ex) { return ex->response->result_int(); })
    .run([&](auto result) {
        if (result.is_ok()) {
            std::cout << "Status: " << result.value() << std::endl;
        }
    });
```

## Build

```bash
./build.sh
```

## Tests

```bash
./build/tests/httpclient_test
```
