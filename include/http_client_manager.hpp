#pragma once

#include <boost/asio.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "beast_connection_pool.hpp"
#include "client_ssl_ctx.hpp"
#include "http_client_config_provider.hpp"
#include "http_session.hpp"
#include "http_session_pooled.hpp"
#include "proxy_pool.hpp"

namespace asio = boost::asio;

namespace client_async {

class HttpClientManager {
 private:
  std::unique_ptr<asio::io_context> ioc;
  cjj365::ClientSSLContext& client_ssl_ctx;
  int threads_{0};
  std::vector<std::thread> thread_pool;
  std::unique_ptr<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_guard;
  std::unique_ptr<beast_pool::ConnectionPool> pool_;
  std::atomic<bool> stopped_{false};
  std::unique_ptr<ProxyPool> proxy_pool_;
  std::string profile_name_;

 public:
  HttpClientManager(cjj365::ClientSSLContext& ctx,
                    cjj365::IHttpclientConfigProvider& config_provider,
                    std::string_view profile = {})
      : client_ssl_ctx(ctx) {
    profile_name_ = profile.empty()
                        ? std::string(config_provider.default_name())
                        : std::string(profile);
    const auto& cfg = config_provider.get(profile_name_);
    threads_ = cfg.get_threads_num();
    ioc = std::make_unique<asio::io_context>(threads_);
    work_guard = std::make_unique<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(*ioc));
    // Initialize a shared connection pool (defaults are fine; can be extended)
    pool_ = std::make_unique<beast_pool::ConnectionPool>(
        *ioc, beast_pool::PoolConfig{}, &client_ssl_ctx.context());
    proxy_pool_ = std::make_unique<ProxyPool>(config_provider, profile_name_);
    for (size_t i = 0; i < threads_; ++i) {
      thread_pool.emplace_back([this] { ioc->run(); });
    }
  }

  ~HttpClientManager() { stop(); }

  void stop() {
    if (stopped_.exchange(true)) return;  // already stopped
    work_guard->reset();
    ioc->stop();
    for (auto& t : thread_pool) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  const cjj365::ProxySetting* borrow_proxy() {
    if (!proxy_pool_) {
      return nullptr;
    }
    return proxy_pool_->next();
  }

  void blacklist_proxy(const cjj365::ProxySetting& proxy,
                       std::chrono::seconds timeout =
                           std::chrono::seconds{300}) {
    if (proxy_pool_) {
      proxy_pool_->blacklist(proxy, timeout);
    }
  }

  void reset_proxy_blacklist() {
    if (proxy_pool_) {
      proxy_pool_->reset_blacklist();
    }
  }

  bool has_proxy_pool() const {
    return proxy_pool_ && !proxy_pool_->empty();
  }

  std::string_view profile_name() const { return profile_name_; }

   private:
    static bool is_redirect_status(int status_code) {
      return status_code == 301 || status_code == 302 || status_code == 303 ||
             status_code == 307 || status_code == 308;
    }

    static std::optional<urls::url> resolve_redirect_url(
        const urls::url& base, std::string_view location) {
      if (location.empty()) {
        return std::nullopt;
      }

      auto parse_abs = [](std::string_view s) -> std::optional<urls::url> {
        urls::result<urls::url> parsed = urls::parse_uri(std::string(s));
        if (!parsed) {
          return std::nullopt;
        }
        return parsed.value();
      };

      // Absolute URI
      if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return parse_abs(location);
      }

      // Scheme-relative URI: //host/path
      if (location.rfind("//", 0) == 0) {
        std::string full;
        full.reserve(base.scheme().size() + 1 + location.size());
        full.append(base.scheme());
        full.push_back(':');
        full.append(location);
        return parse_abs(full);
      }

      // Build base origin: scheme://host[:port]
      std::string origin;
      origin.reserve(base.scheme().size() + 3 + base.host().size() + 8);
      origin.append(base.scheme());
      origin.append("://");
      origin.append(base.host());
      if (base.has_port()) {
        origin.push_back(':');
        origin.append(base.port());
      }

      // Absolute-path reference
      if (!location.empty() && location.front() == '/') {
        std::string full;
        full.reserve(origin.size() + location.size());
        full.append(origin);
        full.append(location);
        return parse_abs(full);
      }

      // Relative-path reference (best-effort): base directory + location
      std::string base_path = std::string(base.path());
      auto slash = base_path.rfind('/');
      std::string dir = (slash == std::string::npos) ? std::string("/")
                                                     : base_path.substr(0, slash + 1);
      std::string full;
      full.reserve(origin.size() + dir.size() + location.size() + 1);
      full.append(origin);
      if (dir.empty() || dir.front() != '/') {
        full.push_back('/');
      }
      full.append(dir);
      full.append(location);
      return parse_abs(full);
    }

    template <class RequestBody>
    static void update_request_target_for_url(
        http::request<RequestBody, http::basic_fields<std::allocator<char>>>& req,
        const urls::url& url) {
      // Request target should be origin-form: path[?query]
      std::string target;
      // Use encoded components to preserve exact bytes (important for
      // signed URLs like GitHub release assets).
      const auto enc_path = url.encoded_path();
      const auto enc_query = url.encoded_query();
      target.reserve(enc_path.size() + (!enc_query.empty() ? 1 + enc_query.size() : 0));
      if (enc_path.empty()) {
        target.push_back('/');
      } else {
        target.append(enc_path);
      }
      if (!enc_query.empty()) {
        target.push_back('?');
        target.append(enc_query);
      }
      req.target(target);
    }

   public:
    template <class RequestBody, class ResponseBody>
  void http_request(
      const urls::url_view& url_input,
      http::request<RequestBody, http::basic_fields<std::allocator<char>>>&&
          req,
      std::function<
          void(std::optional<http::response<
                   ResponseBody, http::basic_fields<std::allocator<char>>>>&&,
               int)>&& callback,
      HttpClientRequestParams&& params = {},
      const cjj365::ProxySetting* proxy_setting = nullptr) {
    struct RedirectState {
      urls::url url;
      http::request<RequestBody, http::basic_fields<std::allocator<char>>>
          req_template;
      HttpClientRequestParams params;
      const cjj365::ProxySetting* proxy_setting{nullptr};
      int redirects_left{5};
      std::shared_ptr<std::function<void()>> step;
      std::function<void(
          std::optional<http::response<ResponseBody,
                                       http::basic_fields<std::allocator<char>>>>&&,
          int)>
          user_cb;
    };

    auto st = std::make_shared<RedirectState>(RedirectState{
        .url = urls::url(url_input),
        .req_template = std::move(req),
        .params = std::move(params),
        .proxy_setting = proxy_setting,
        .redirects_left = 5,
        .step = nullptr,
        .user_cb = std::move(callback),
    });

    auto step = std::make_shared<std::function<void()>>();
    st->step = step;
    std::weak_ptr<RedirectState> st_weak = st;

    *step = [this, st_weak]() {
      auto st = st_weak.lock();
      if (!st) return;
      auto req_one = st->req_template;
      update_request_target_for_url(req_one, st->url);

      auto cb = [st](std::optional<http::response<
                               ResponseBody,
                               http::basic_fields<std::allocator<char>>>>&& resp,
                           int ec) mutable {
        if (ec != 0 || !resp.has_value() || !st->params.follow_redirect ||
            st->redirects_left <= 0) {
          st->user_cb(std::move(resp), ec);
          return;
        }

        // Follow redirects only for GET/HEAD to avoid method/body semantics.
        if (st->req_template.method() != http::verb::get &&
            st->req_template.method() != http::verb::head) {
          st->user_cb(std::move(resp), ec);
          return;
        }

        int status = resp->result_int();
        if (!is_redirect_status(status)) {
          st->user_cb(std::move(resp), ec);
          return;
        }

        auto it = resp->find(http::field::location);
        if (it == resp->end()) {
          st->user_cb(std::move(resp), ec);
          return;
        }

        auto next = resolve_redirect_url(st->url, std::string_view(it->value()));
        if (!next.has_value()) {
          st->user_cb(std::move(resp), ec);
          return;
        }

        st->url = std::move(*next);
        st->redirects_left -= 1;
        if (st->step) {
          (*st->step)();
        }
      };

      urls::url url_local = st->url;
      if (url_local.scheme() == "https") {
        auto session = std::make_shared<
            session_ssl<RequestBody, ResponseBody, std::allocator<char>>>(
            *(this->ioc), this->client_ssl_ctx.context(), std::move(url_local),
            HttpClientRequestParams{st->params}, std::move(cb), st->proxy_setting);
        session->set_req(std::move(req_one));
        session->run();
      } else {
        auto session = std::make_shared<
            session_plain<RequestBody, ResponseBody, std::allocator<char>>>(
            *(this->ioc), std::move(url_local), HttpClientRequestParams{st->params},
            std::move(cb), st->proxy_setting);
        session->set_req(std::move(req_one));
        session->run();
      }
    };

    (*step)();
  }

  // New: pooled variant (keeps existing APIs intact)
  template <class RequestBody, class ResponseBody>
  void http_request_pooled(
      const urls::url_view& url_input,
      http::request<RequestBody, http::basic_fields<std::allocator<char>>>&&
          req,
      std::function<
          void(std::optional<http::response<
                   ResponseBody, http::basic_fields<std::allocator<char>>>>&&,
               int)>&& callback,
      HttpClientRequestParams&& params = {},
      const cjj365::ProxySetting* proxy_setting = nullptr) {
    if (params.follow_redirect) {
      // Reuse the non-pooled implementation for redirect chains.
      return http_request<RequestBody, ResponseBody>(
          url_input, std::move(req), std::move(callback), std::move(params),
          proxy_setting);
    }
    // Build origin for pool acquisition
    urls::url url(url_input);
    beast_pool::Origin origin;
    origin.scheme = std::string(url.scheme());
    origin.host = std::string(url.host());
    if (url.has_port()) {
      origin.port = static_cast<std::uint16_t>(url.port_number());
    } else {
      origin.port = (origin.scheme == "https") ? 443 : 80;
    }

    std::optional<typename client_async::http_session_pooled<
        RequestBody, ResponseBody, std::allocator<char>>::ProxySetting>
        proxy;
    if (proxy_setting) {
      proxy = {proxy_setting->host, proxy_setting->port,
               proxy_setting->username, proxy_setting->password};
    }

    using Pooled = client_async::http_session_pooled<RequestBody, ResponseBody,
                                                     std::allocator<char>>;
    auto session =
        std::make_shared<Pooled>(*pool_, std::move(origin), std::move(proxy));
    session->set_request(std::move(req));
    session->run(std::move(callback));
  }
};

// EXTERN_CLIENTPOOL_HTTP_REQUEST(http::string_body, http::string_body)
// EXTERN_CLIENTPOOL_HTTP_REQUEST(http::empty_body, http::string_body)
// EXTERN_CLIENTPOOL_HTTP_REQUEST(http::file_body, http::empty_body)

}  // namespace client_async
