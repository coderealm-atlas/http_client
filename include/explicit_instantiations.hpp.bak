#pragma  once


#define INSTANTIATE_CLIENTPOOL_HTTP_REQUEST(REQ, RESP)                                \
  template void client_async::ClientPoolSsl::http_request<REQ, RESP>(                \
      const urls::url_view&,                                                         \
      http::request<REQ, http::basic_fields<std::allocator<char>>>&&,               \
      std::function<void(std::optional<http::response<RESP,                          \
                          http::basic_fields<std::allocator<char>>>>&&, int)>&&,    \
      HttpClientRequestParams&&,                                                     \
      const std::optional<ProxySetting>&);


#define EXTERN_CLIENTPOOL_HTTP_REQUEST(REQ, RESP)                                             \
  extern template void client_async::ClientPoolSsl::http_request<REQ, RESP>(                  \
      const urls::url_view&,                                                                  \
      http::request<REQ, http::basic_fields<std::allocator<char>>>&&,                        \
      std::function<void(std::optional<http::response<RESP, http::basic_fields<std::allocator<char>>>>&&, int)>&&, \
      HttpClientRequestParams&&, const std::optional<ProxySetting>&);
      

#define INSTANTIATE_HTTP_SESSION(REQ, RESP)                                            \
  template class client_async::session_ssl<REQ, RESP, std::allocator<char>>;          \
  template class client_async::session_plain<REQ, RESP, std::allocator<char>>;        \
  template class client_async::session<client_async::session_ssl<REQ, RESP, std::allocator<char>>, \
                                       REQ, RESP, std::allocator<char>>;              \
  template class client_async::session<client_async::session_plain<REQ, RESP, std::allocator<char>>, \
                                       REQ, RESP, std::allocator<char>>;

#define EXTERN_HTTP_SESSION(REQ, RESP)                                                   \
  extern template class client_async::session_ssl<REQ, RESP, std::allocator<char>>;      \
  extern template class client_async::session_plain<REQ, RESP, std::allocator<char>>;    \
  extern template class client_async::session<client_async::session_ssl<REQ, RESP, std::allocator<char>>, \
                                              REQ, RESP, std::allocator<char>>;          \
  extern template class client_async::session<client_async::session_plain<REQ, RESP, std::allocator<char>>, \
                                              REQ, RESP, std::allocator<char>>;