#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "result_monad.hpp"
using monad::Result;

struct TestError {
  int code{};
  std::string what;
  friend bool operator==(const TestError& a, const TestError& b) {
    return a.code == b.code && a.what == b.what;
  }
};

TEST(ResultV2, Basics_Construction_And_Introspection) {
  using R = Result<int, TestError>;

  R ok = R::Ok(5);
  EXPECT_TRUE(ok.is_ok());
  EXPECT_TRUE(static_cast<bool>(ok));
  EXPECT_FALSE(ok.is_err());
  EXPECT_EQ(ok.value(), 5);

  R err = R::Err(TestError{1, "boom"});
  EXPECT_TRUE(err.is_err());
  EXPECT_FALSE(static_cast<bool>(err));
  EXPECT_EQ(err.error().code, 1);
  EXPECT_EQ(err.error().what, "boom");

  EXPECT_NE(ok.ok_ptr(), nullptr);
  EXPECT_EQ(ok.err_ptr(), nullptr);
  EXPECT_EQ(err.ok_ptr(), nullptr);
  EXPECT_NE(err.err_ptr(), nullptr);
}

TEST(ResultV2, Map_Copy_And_Move) {
  using R = Result<int, TestError>;
  R ok = R::Ok(7);

  auto r2 = ok.map([](const int& x) { return x + 1; });
  static_assert(std::is_same_v<decltype(r2), Result<int, TestError>>);
  ASSERT_TRUE(r2);
  EXPECT_EQ(r2.value(), 8);

  using RS = Result<std::string, TestError>;
  RS s = RS::Ok(std::string{"hi"});
  auto r3 = std::move(s).map([](std::string&& x) { return x + "!"; });
  ASSERT_TRUE(r3);
  EXPECT_EQ(r3.value(), "hi!");
}

TEST(ResultV2, AndThen_Success_And_Error) {
  using R = Result<int, TestError>;
  R ok = R::Ok(3);
  R er = R::Err(TestError{2, "e"});

  auto doubled =
      ok.and_then([](int x) { return Result<int, TestError>::Ok(x * 2); });
  ASSERT_TRUE(doubled);
  EXPECT_EQ(doubled.value(), 6);

  auto still_err =
      er.and_then([](int) { return Result<int, TestError>::Ok(0); });
  ASSERT_TRUE(still_err.is_err());
  EXPECT_EQ(still_err.error().code, 2);
}

TEST(ResultV2, MapErr_And_CatchThen_Change_Error_Type) {
  Result<int, TestError> er = Result<int, TestError>::Err(TestError{3, "x"});
  auto mapped =
      er.map_err([](const TestError& e) { return std::string{"E:"} + e.what; });
  static_assert(std::is_same_v<decltype(mapped), Result<int, std::string>>);
  ASSERT_TRUE(mapped.is_err());
  EXPECT_EQ(mapped.error(), "E:x");

  // catch_then converts error type; value type must remain the same
  Result<int, TestError> ok = Result<int, TestError>::Ok(9);
  auto kept_ok = ok.catch_then(
      [](const TestError&) { return Result<int, std::string>::Err("no"); });
  ASSERT_TRUE(kept_ok);
  EXPECT_EQ(kept_ok.value(), 9);

  auto converted = er.catch_then([](const TestError& e) {
    return Result<int, std::string>::Err(std::to_string(e.code));
  });
  ASSERT_TRUE(converted.is_err());
  EXPECT_EQ(converted.error(), "3");
}

TEST(ResultV2, OrElse_And_ValueOr) {
  using R = Result<int, TestError>;
  R ok = R::Ok(4);
  R er = R::Err(TestError{5, "bad"});

  auto kept = ok.or_else([](const TestError&) { return R::Ok(0); });
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept.value(), 4);

  auto recovered = er.or_else([](const TestError&) { return R::Ok(42); });
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered.value(), 42);

  EXPECT_EQ(ok.value_or(1), 4);
  EXPECT_EQ(er.value_or(1), 1);
  EXPECT_EQ(er.value_or_else([] { return 7; }), 7);
}

TEST(ResultV2, Visit_And_Equality_Swap) {
  using R = Result<int, TestError>;
  R a = R::Ok(10);
  R b = R::Ok(10);
  EXPECT_TRUE(a == b);

  int sum = 0;
  a.visit([&](auto&& x) {
    using X = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<X, int>) sum += x;  // ok branch
  });
  EXPECT_EQ(sum, 10);

  R c = R::Err(TestError{11, "no"});
  using std::swap;
  swap(a, c);
  EXPECT_TRUE(a.is_err());
  EXPECT_TRUE(c);
}

TEST(ResultV2, Optional_And_MoveOnly_Value) {
  using R = Result<std::unique_ptr<int>, TestError>;
  R r = R::Ok(std::make_unique<int>(3));
  auto opt = std::move(r).as_optional();
  ASSERT_TRUE(opt.has_value());
  ASSERT_TRUE(opt->get());
  EXPECT_EQ(*opt->get(), 3);
}

TEST(ResultV2, Void_Specialization_Flows) {
  using RV = Result<void, TestError>;
  using RI = Result<int, TestError>;

  RV ok = RV::Ok();
  auto next = ok.and_then([] { return RI::Ok(1); });
  ASSERT_TRUE(next);
  EXPECT_EQ(next.value(), 1);

  RV er = RV::Err(TestError{6, "v"});
  auto recovered_void =
      er.catch_then([](const TestError&) { return RV::Ok(); });
  ASSERT_TRUE(recovered_void.is_ok());

  auto mapped_err =
      er.map_err([](const TestError& e) { return std::string{"v:"} + e.what; });
  static_assert(
      std::is_same_v<decltype(mapped_err), Result<void, std::string>>);
  ASSERT_TRUE(mapped_err.is_err());
  EXPECT_EQ(mapped_err.error(), "v:v");
}

TEST(ResultV2, AllOk_ZipResults_NonVoid) {
  using R1 = Result<int, TestError>;
  using R2 = Result<std::string, TestError>;
  using R3 = Result<double, TestError>;

  auto t = monad::zip_results<TestError>(R1::Ok(1), R2::Ok("a"), R3::Ok(2.5));
  ASSERT_TRUE(t);
  auto [i, s, d] = t.value();
  EXPECT_EQ(i, 1);
  EXPECT_EQ(s, "a");
  EXPECT_DOUBLE_EQ(d, 2.5);

  auto err_first =
      monad::zip_results<TestError>(R1::Err(TestError{7, "e1"}), R2::Ok("x"));
  ASSERT_TRUE(err_first.is_err());
  EXPECT_EQ(err_first.error().code, 7);

  auto err_second =
      monad::zip_results<TestError>(R1::Ok(2), R2::Err(TestError{8, "e2"}));
  ASSERT_TRUE(err_second.is_err());
  EXPECT_EQ(err_second.error().code, 8);
}

TEST(ResultV2, ZipResults_Rvalue_Moves) {
  using RU = Result<std::unique_ptr<int>, TestError>;
  auto r = monad::zip_results<TestError>(RU::Ok(std::make_unique<int>(9)),
                                         RU::Ok(std::make_unique<int>(10)));
  ASSERT_TRUE(r);
  auto [p, q] = std::move(r).value();
  ASSERT_TRUE(p && q);
  EXPECT_EQ(*p, 9);
  EXPECT_EQ(*q, 10);
}

TEST(ResultV2, ZipResults_SkipVoid_Mixed) {
  using RI = Result<int, TestError>;
  using RS = Result<std::string, TestError>;
  using RV = Result<void, TestError>;

  auto z =
      monad::zip_results_skip_void<TestError>(RI::Ok(3), RV::Ok(), RS::Ok("z"));
  ASSERT_TRUE(z);
  auto [i, s] = z.value();
  EXPECT_EQ(i, 3);
  EXPECT_EQ(s, "z");
}

TEST(ResultV2, CollectResults_Vector_And_InitList) {
  using R = Result<int, TestError>;
  std::vector<R> items = {R::Ok(1), R::Ok(2), R::Ok(3)};
  auto coll = monad::collect_results<int, TestError>(items);
  ASSERT_TRUE(coll);
  EXPECT_EQ(coll.value().size(), 3u);
  EXPECT_EQ(coll.value()[0], 1);

  auto coll2 = monad::collect_results<int, TestError>({R::Ok(4), R::Ok(5)});
  ASSERT_TRUE(coll2);
  EXPECT_EQ(coll2.value().size(), 2u);

  auto coll_move = monad::collect_results<int, TestError>(std::move(items));
  ASSERT_TRUE(coll_move);
  EXPECT_EQ(coll_move.value().size(), 3u);

  auto fail = monad::collect_results<int, TestError>(
      {R::Ok(1), R::Err(TestError{9, "n"})});
  ASSERT_TRUE(fail.is_err());
  EXPECT_EQ(fail.error().code, 9);
}
