#pragma once

#include <boost/json.hpp>  // IWYU pragma: keep

namespace json = boost::json;
namespace resp {

struct DataMeta {
  uint64_t total;
  uint64_t offset;
  uint64_t limit;

  friend void tag_invoke(const json::value_from_tag&, json::value& jv,
                         const DataMeta& meta) {
    auto jo = json::object{};
    jo["total"] = meta.total;
    jo["offset"] = meta.offset;
    jo["limit"] = meta.limit;
    jv = std::move(jo);
  }

  friend DataMeta tag_invoke(const json::value_to_tag<DataMeta>&,
                             const json::value& jv) {
    DataMeta meta;
    if (auto* jo_p = jv.if_object()) {
      if (auto* total_p = jo_p->if_contains("total")) {
        meta.total = total_p->to_number<uint64_t>();
      }
      if (auto* offset_p = jo_p->if_contains("offset")) {
        meta.offset = offset_p->to_number<uint64_t>();
      }
      if (auto* limit_p = jo_p->if_contains("limit")) {
        meta.limit = limit_p->to_number<uint64_t>();
      }
    }
    return meta;
  }
};

template <typename T>
struct ListResult {
  std::vector<T> data;
  DataMeta meta;

  ListResult() = default;
  ListResult(std::vector<T>&& data, uint64_t total, uint64_t offset,
             uint64_t limit)
      : data(std::move(data)), meta({total, offset, limit}) {}
  ListResult(std::vector<T>&& data_)
      : data(std::move(data_)), meta({data.size(), 0, data.size()}) {}

  json::value data_json() const { return json::value_from(data); }
  json::value meta_json() const { return json::value_from(meta); }

  bool empty() const { return data.empty(); }
  size_t size() const { return data.size(); }
};
}  // namespace resp