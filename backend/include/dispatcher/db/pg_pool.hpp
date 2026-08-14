#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <pqxx/pqxx>

namespace dispatcher::db {

class PgPool {
 public:
  explicit PgPool(std::string connection_string);

  [[nodiscard]] std::unique_ptr<pqxx::connection> acquire();
  [[nodiscard]] bool healthy();

 private:
  std::string connection_string_;
  std::mutex mutex_;
};

}  // namespace dispatcher::db
