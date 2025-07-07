#include <fmt/format.h>
#include <gtest/gtest.h>  // Add this line

#include <boost/beast/http.hpp>
#include <boost/url.hpp>

#include "apikey_util.hpp"
#include "app_service.hpp"
#include "content_filter.hpp"
// #include "db_manager.hpp"
#include "header_level_handler.hpp"
#include "login_page_util.hpp"
#include "user_util.hpp"
#include "website_util.hpp"

namespace urls = boost::urls;
namespace http = boost::beast::http;

TEST(HttpServerUtilTest, loginpage) {
  db_manager::initDB("./testdb");
  urls::url_view req_url_view_{"/?redirect=/login-page"};
  cjj365::meta::User user;
  auto jr = bbserver::loginpage::create_http_session(user, req_url_view_, false);
  ASSERT_TRUE(jr.headers[http::field::set_cookie].find("cjj365") !=
              std::string::npos);
  db_manager::closeDB();
}
TEST(HttpServerUtilTest, contentfilter) {
  std::string s1 = "123456";
  s1.erase(1, 2);
  ASSERT_EQ(s1, "1456");
  std::string content =
      R"(<html><head></head><body><figure><p>x-cjj365-cc --out-tag figure --to-hides hhhiden li;;;</p></figure><li><p class="hhhiden">hide</p></li></body></html>)";
  std::string processed_content = bbserver::process_trigger_phrase("", content);
  ASSERT_EQ(processed_content, "<html><head></head><body></body></html>");
}

static bool status_is_error(json::value& jv) {
  return jv.as_object()["status"].as_int64() == 1;
}

static cjj365::meta::User demo_user(bool clear_keys = true) {
  std::string email = "t@aa.bb";
  std::string password = "password";
  auto user = db_manager::user_by_email(email);
  if (user.has_value()) {
    if (user->apikeys_size() > 0) {
      if (clear_keys) {
        user->clear_apikeys();
        db_manager::update_user(*user);
      }
    }
    return *user;
  } else {
    return db_manager::register_user(email, password);
  }
}

TEST(HttpServerUtilTest, apikeyCreate) {
  bbserver::Cleanupper cleanupper;
  db_manager::initDB("./testdb");
  auto user = demo_user();
  http::request<http::string_body> req;
  req.method(http::verb::post);
  req.target("/apiv1/users/" + user.id() + "/apikeys");
  boost::urls::url_view req_url_view_(req.target());
  std::string user_id = user.id();
  auto jr = bbserver::apikeys::handle_request(req, req_url_view_, user_id,
                                              {{"name", "tk1"}});
  std::cout << jr.jv << std::endl;
  int code = jr.jv.as_object()["code"].as_int64();
  ASSERT_FALSE(status_is_error(jr.jv));
  ASSERT_EQ(demo_user(false).apikeys_size(), 1) << "should be one apikey.";
  bbserver::apikeys::handle_request(req, req_url_view_, user_id, {{"name", "tk1"}});
  ASSERT_EQ(demo_user(false).apikeys_size(), 2) << "should be 2 apikeys.";

  http::request<http::empty_body> req0;
  req0.method(http::verb::delete_);
  req0.target("/apiv1/users/" + user.id() + "/apikeys/0");
  boost::urls::url_view req_url1_(req0.target());
  std::cout << "start delete" << std::endl;
  jr = bbserver::apikeys::handle_request(req0, req_url1_, user_id);
  ASSERT_FALSE(status_is_error(jr.jv));
  ASSERT_EQ(demo_user(false).apikeys_size(), 1) << "should be 1 apikeys.";
  bbserver::apikeys::handle_request(req0, req_url1_, user_id);
  ASSERT_EQ(demo_user(false).apikeys_size(), 0) << "should be 0 apikeys.";
  cleanupper();
}

TEST(HttpServerUtilTest, apikeyUpdate) {
  bbserver::Cleanupper cleanupper;
  db_manager::initDB("./testdb");
  auto user = demo_user();
  http::request<http::string_body> req;
  req.method(http::verb::post);
  req.target("/apiv1/users/" + user.id() + "/apikeys");
  boost::urls::url_view req_url_view_(req.target());
  std::string user_id = user.id();
  auto jr = bbserver::apikeys::handle_request(req, req_url_view_, user_id,
                                              {{"name", "tk1"}});
  std::cout << jr.jv << std::endl;
  int code = jr.jv.as_object()["code"].as_int64();
  ASSERT_FALSE(status_is_error(jr.jv));
  ASSERT_EQ(demo_user(false).apikeys_size(), 1) << "should be one apikey.";
  ASSERT_FALSE(demo_user(false).apikeys(0).disabled());

  http::request<http::string_body> req0;
  req0.method(http::verb::put);
  req0.target("/apiv1/users/" + user.id() + "/apikeys/0");
  boost::urls::url_view req_url1_(req0.target());
  std::cout << "start update" << std::endl;
  jr = bbserver::apikeys::handle_request(req0, req_url1_, user_id,
                                         {{"disabled", true}, {"name", "tk2"}});
  ASSERT_FALSE(status_is_error(jr.jv));
  ASSERT_TRUE(demo_user(false).apikeys(0).disabled());
  ASSERT_EQ(demo_user(false).apikeys(0).name(), "tk2");
  cleanupper();
}

TEST(HttpServerUtilTest, websiteCreate) {
  bbserver::Cleanupper cleanupper;
  db_manager::initDB("./testdb");
  auto user = demo_user();
  cjj365::AppConfigCommon& app_config =
      cjj365::AppConfigCommon::getInstance("gtest/server_config_debug_local.json");
  http::request<http::string_body> req;
  req.method(http::verb::post);
  req.target("/apiv1/users/" + user.id() + "/websites");
  boost::urls::url_view req_url_view_(req.target());
  std::string user_id = user.id();
  auto websites = db_manager::get_websites(user.id(), 0, 1000);
  if (websites.size() > 0) {
    for (auto website : websites) {
      db_manager::delete_website(website.id());
    }
  }
  auto jr = bbserver::websites::handle_request(app_config, req, req_url_view_,
                                               user_id, {{"name", "tk1"}});
  std::cout << jr.jv << std::endl;
  ASSERT_FALSE(status_is_error(jr.jv));
  websites = db_manager::get_websites(user.id(), 0, 1000);
  ASSERT_EQ(websites.size(), 1) << "should be one website.";
  bbserver::websites::handle_request(app_config, req, req_url_view_, user_id,
                                     {{"name", "tk1"}});
  websites = db_manager::get_websites(user.id(), 0, 1000);
  ASSERT_EQ(websites.size(), 2) << "should be 2 websites.";

  http::request<http::empty_body> req0;
  req0.method(http::verb::delete_);
  req0.target("/apiv1/users/" + user.id() + "/websites/" + websites[0].id());
  boost::urls::url_view req_url1_(req0.target());
  std::cout << "start delete id: " << websites[0].id() << std::endl;
  jr = bbserver::websites::handle_request(app_config, req0, req_url1_, user_id);
  ASSERT_FALSE(status_is_error(jr.jv));
  websites = db_manager::get_websites(user.id(), 0, 1000);
  ASSERT_EQ(websites.size(), 1) << "should be 1 website.";

  req0.target("/apiv1/users/" + user.id() + "/websites/" + websites[0].id());
  boost::urls::url_view req_url2_(req0.target());
  bbserver::websites::handle_request(app_config, req0, req_url2_, user_id);
  websites = db_manager::get_websites(user.id(), 0, 1000);
  ASSERT_EQ(websites.size(), 0) << "should be 0 websites.";
  cleanupper();
}

// TEST(HttpServerUtilTest, userCreate) {
//   bbserver::Cleanupper cleanupper;
//   db_manager::initDB("./testdb");
//   auto users = db_manager::get_users(true, {0, 1000});
//   for (auto user : users) {
//     db_manager::delete_user(user.id());
//   }
//   http::request<http::string_body> req;
//   req.method(http::verb::post);
//   req.target("/apiv1/users");
//   boost::urls::url_view req_url_view_(req.target());
//   bbserver::AppConfig& app_config =
//       bbserver::AppConfig::getInstance("gtest/server_config_debug_local.json");
//   auto jr = bbserver::users::handle_request(app_config, req, req_url_view_, "");
//   std::cout << jr.jv << std::endl;
//   ASSERT_FALSE(status_is_error(jr.jv));
//   users = db_manager::get_users(true, {0, 1000});
//   ASSERT_EQ(users.size(), 1) << "should be one user.";
//   bbserver::users::handle_request(app_config, req, req_url_view_, "");
//   users = db_manager::get_users(true, {0, 1000});
//   ASSERT_EQ(users.size(), 2) << "should be 2 users.";

//   http::request<http::empty_body> req0;
//   req0.method(http::verb::delete_);
//   req0.target("/apiv1/users/" + users[0].id());
//   boost::urls::url_view req_url1_(req0.target());
//   std::cout << "start delete id: " << users[0].id() << std::endl;
//   jr = bbserver::users::handle_request(app_config, req0, req_url1_, "");
//   ASSERT_FALSE(status_is_error(jr.jv));
//   users = db_manager::get_users(true, {0, 1000});
//   ASSERT_EQ(users.size(), 1) << "should be 1 user.";

//   req0.target("/apiv1/users/" + users[0].id());
//   boost::urls::url_view req_url2_(req0.target());
//   bbserver::users::handle_request(app_config, req0, req_url2_, "");
//   users = db_manager::get_users(true, {0, 1000});
//   ASSERT_EQ(users.size(), 0) << "should be 0 users.";
//   cleanupper();
// }

TEST(HttpServerUtilTest, listUsers) {
  bbserver::Cleanupper cleanupper;
  db_manager::initDB("./testdb");
  auto users = db_manager::get_users(
      cjj365::meta::User_Status::User_Status_UNKNOWN, {0, 1000});
  for (auto user : users) {
    db_manager::delete_user(user.id());
  }
  http::request<http::string_body> req;
  req.method(http::verb::post);
  req.target("/apiv1/users");
  boost::urls::url_view req_url_view_(req.target());
  cjj365::AppConfigCommon& app_config =
      cjj365::AppConfigCommon::getInstance("gtest/server_config_debug_local.json");
  cjj365::meta::User user;
  user.set_status(cjj365::meta::User_Status_ACTIVE);
  user.set_name("user1");
  json::value jv = cjj365::user_to_json(user);
  auto jr = bbserver::users::handle_request(app_config, req, req_url_view_, "",
                                            std::move(jv));
  std::cout << jr.jv << std::endl;
  ASSERT_FALSE(status_is_error(jr.jv));
  users = db_manager::get_users(cjj365::meta::User_Status::User_Status_UNKNOWN,
                                {0, 1000});
  ASSERT_EQ(users.size(), 1) << "should be one user.";
  user.set_name("user2");
  user.set_status(cjj365::meta::User_Status_INACTIVE);
  jv = cjj365::user_to_json(user);
  bbserver::users::handle_request(app_config, req, req_url_view_, "", std::move(jv));
  users = db_manager::get_users(cjj365::meta::User_Status::User_Status_UNKNOWN,
                                {0, 1000});
  ASSERT_EQ(users.size(), 2) << "should be 2 users.";

  http::request<http::empty_body> req0;
  req0.method(http::verb::delete_);
  req0.target("/apiv1/users/" + users[0].id());
  boost::urls::url_view req_url1_(req0.target());
  std::cout << "start delete id: " << users[0].id() << std::endl;
  jr = bbserver::users::handle_request(app_config, req0, req_url1_, "");
  ASSERT_FALSE(status_is_error(jr.jv));
  users = db_manager::get_users(cjj365::meta::User_Status::User_Status_UNKNOWN,
                                {0, 1000});
  ASSERT_EQ(users.size(), 1) << "should be 1 user.";

  req0.target("/apiv1/users/" + users[0].id());
  boost::urls::url_view req_url2_(req0.target());
  bbserver::users::handle_request(app_config, req0, req_url2_, "");
  users = db_manager::get_users(cjj365::meta::User_Status::User_Status_UNKNOWN,
                                {0, 1000});
  ASSERT_EQ(users.size(), 0) << "should be 0 users.";
  cleanupper();
}