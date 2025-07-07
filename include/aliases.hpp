#pragma once

#include <boost/asio.hpp>      // IWYU pragma: keep
#include <boost/asio/ssl.hpp>  // IWYU pragma: keep
#include <boost/beast.hpp>     // IWYU pragma: keep
#include <boost/json.hpp>      // IWYU pragma: keep
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/url.hpp>  // IWYU pragma: keep

namespace beast = boost::beast;    // from <boost/beast.hpp>
namespace http = beast::http;      // from <boost/beast/http.hpp>
namespace net = boost::asio;       // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;  // from <boost/asio/ssl.hpp>
namespace urls = boost::urls;      // from <boost/url.hpp>
namespace logging = boost::log;
namespace trivial = logging::trivial;
namespace logsrc = boost::log::sources;
namespace json = boost::json;
using tcp = net::ip::tcp;