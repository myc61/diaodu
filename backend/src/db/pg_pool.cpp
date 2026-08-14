#include "dispatcher/db/pg_pool.hpp"

namespace dispatcher::db {

PgPool::PgPool(std::string connection_string)
    : connection_string_(std::move(connection_string)) {}

std::unique_ptr<pqxx::connection> PgPool::acquire() {
  std::lock_guard lock(mutex_);
  return std::make_unique<pqxx::connection>(connection_string_);
}

bool PgPool::healthy() {
  try {
    auto connection = acquire();
    pqxx::work tx(*connection);
    tx.exec("SELECT 1");
    tx.commit();
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace dispatcher::db
