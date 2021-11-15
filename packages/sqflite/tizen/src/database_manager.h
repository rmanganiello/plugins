#ifndef DATABASE_MANAGER_H_
#define DATABASE_MANAGER_H_

#include <flutter/standard_method_codec.h>

#include "list"
#include "sqlite3.h"
#include "string"

struct DatabaseError : public std::runtime_error {
  DatabaseError(int code, const char *msg)
      : std::runtime_error(std::string(msg) + " (code " + std::to_string(code) +
                           ")") {}
};

class DatabaseManager {
 public:
  sqlite3 *sqliteDatabase;
  std::map<std::string, sqlite3_stmt *> stmtCache;
  bool singleInstance;
  std::string path;
  int id;
  int logLevel;

  DatabaseManager(std::string aPath, int aId, bool aSingleInstance,
                  int aLogLevel);
  virtual ~DatabaseManager();

  typedef std::variant<int64_t, std::string, double, std::vector<uint8_t>,
                       std::nullptr_t>
      resultvalue;
  typedef std::vector<resultvalue> result;
  typedef std::vector<result> resultset;
  typedef std::vector<std::string> columns;
  typedef flutter::EncodableList parameters;

  void open();
  void openReadOnly();
  const char *getErrorMsg();
  int getErrorCode();
  sqlite3 *getWritableDatabase();
  sqlite3 *getReadableDatabase();
  void execute(std::string sql, parameters params = parameters());
  std::pair<DatabaseManager::columns, DatabaseManager::resultset> query(
      std::string sql, parameters params = parameters());

 private:
  typedef sqlite3_stmt *statement;

  void init();
  void close();
  void prepareStmt(statement stmt, std::string sql);
  void bindStmtParams(statement stmt, parameters params);
  void executeStmt(statement stmt);
  std::pair<DatabaseManager::columns, DatabaseManager::resultset> queryStmt(
      statement stmt);
  void finalizeStmt(statement stmt);
  sqlite3_stmt *prepareStmt(std::string sql);
  int getStmtColumnsCount(statement stmt);
  int getColumnType(statement stmt, int iCol);
  const char *getColumnName(statement stmt, int iCol);
};
#endif  // DATABASE_MANAGER_H_
