// Copyright 2021 Samsung Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <flutter/event_channel.h>
#include <flutter/event_sink.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>
#include <flutter/standard_method_codec.h>
#include <storage.h>
#ifndef TV_PROFILE
#include <privacy_privilege_manager.h>
#endif

#include <app_common.h>
#include <dlog.h>

#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "constants.h"
#include "database_manager.h"
#include "list"
#include "log.h"
#include "permission_manager.h"
#include "sqflite_plugin.h"

template <typename T>
bool GetValueFromEncodableMap(flutter::EncodableMap &map, std::string key,
                              T &out) {
  auto iter = map.find(flutter::EncodableValue(key));
  if (iter != map.end() && !iter->second.IsNull()) {
    if (auto pval = std::get_if<T>(&iter->second)) {
      out = *pval;
      return true;
    }
  }
  return false;
}

class SqflitePlugin : public flutter::Plugin {
  inline static std::map<std::string, int> singleInstancesByPath;
  inline static std::map<int, std::shared_ptr<DatabaseManager>> databaseMap;
  inline static std::string databasesPath;
  inline static bool queryAsMapList = false;
  inline static int databaseId = 0;  // incremental database id
  inline static int logLevel = DLOG_UNKNOWN;

 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar *registrar) {
    auto channel =
        std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            registrar->messenger(), PLUGIN_KEY,
            &flutter::StandardMethodCodec::GetInstance());

    auto plugin = std::make_unique<SqflitePlugin>(registrar);

    channel->SetMethodCallHandler(
        [plugin_pointer = plugin.get()](const auto &call, auto result) {
          plugin_pointer->HandleMethodCall(call, std::move(result));
        });

    registrar->AddPlugin(std::move(plugin));
  }
  SqflitePlugin(flutter::PluginRegistrar *registrar)
      : registrar_(registrar), pmm_(std::make_unique<PermissionManager>()) {}

  virtual ~SqflitePlugin() {}

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    LOG_DEBUG("HandleMethodCall: %s", method_call.method_name().c_str());
    try {
      CheckPermissionsOrError();
    } catch (const NotAllowedPermissionError &e) {
      result->Error("permission_not_allowed", e.what());
      return;
    }
    const std::string methodName = method_call.method_name();
    if (methodName == METHOD_OPEN_DATABASE) {
      OnOpenDatabaseCall(method_call, std::move(result));
    } else if (methodName == METHOD_CLOSE_DATABASE) {
      OnCloseDatabaseCall(method_call, std::move(result));
    } else if (methodName == METHOD_DELETE_DATABASE) {
      OnDeleteDatabase(method_call, std::move(result));
    } else if (methodName == METHOD_GET_DATABASES_PATH) {
      OnGetDatabasesPathCall(method_call, std::move(result));
    } else if (methodName == METHOD_OPTIONS) {
      OnOptionsCall(method_call, std::move(result));
    } else if (methodName == METHOD_EXECUTE) {
      OnExecuteCall(method_call, std::move(result));
    } else if (methodName == METHOD_QUERY) {
      OnQueryCall(method_call, std::move(result));
    } else if (methodName == METHOD_INSERT) {
      OnInsertCall(method_call, std::move(result));
    } else if (methodName == METHOD_UPDATE) {
      OnUpdateCall(method_call, std::move(result));
    } else if (methodName == METHOD_BATCH) {
      OnBatchCall(method_call, std::move(result));
    } else if (methodName == METHOD_DEBUG) {
      OnDebugCall(method_call, std::move(result));
    } else {
      result->NotImplemented();
    }
  }

  void CheckPermissionsOrError() {
#ifndef TV_PROFILE
    pmm_->RequestPermission(
        Permission::kMediastorage,
        []() { LOG_DEBUG("MediaStorage permission granted"); },
        [](const std::string &code, const std::string &message) {
          throw NotAllowedPermissionError(code, message);
        });
#else
#endif
  }

  static bool isInMemoryPath(std::string path) {
    return (path.empty() || path == MEMORY_DATABASE_PATH);
  }

 private:
  static int *getDatabaseId(std::string path) {
    int *result = nullptr;
    auto itr = singleInstancesByPath.find(path);
    if (itr != singleInstancesByPath.end()) {
      result = &itr->second;
    }
    return result;
  }
  static std::shared_ptr<DatabaseManager> getDatabase(int databaseId) {
    std::shared_ptr<DatabaseManager> result = nullptr;
    auto itr = databaseMap.find(databaseId);
    if (itr != databaseMap.end()) {
      result = itr->second;
    }
    return result;
  }

  static void handleQueryException(
      DatabaseError exception, std::string sql,
      DatabaseManager::parameters sqlParams,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap exceptionMap;
    exceptionMap.insert(
        std::pair<flutter::EncodableValue, flutter::EncodableValue>(
            flutter::EncodableValue(PARAM_SQL), flutter::EncodableValue(sql)));
    exceptionMap.insert(
        std::pair<flutter::EncodableValue, flutter::EncodableList>(
            flutter::EncodableValue(PARAM_SQL_ARGUMENTS), sqlParams));
    result->Error(ERROR_DATABASE, exception.what(),
                  flutter::EncodableValue(exceptionMap));
  }

  void OnDebugCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    std::string cmd;
    GetValueFromEncodableMap(arguments, PARAM_CMD, cmd);

    flutter::EncodableMap map;

    if (cmd == CMD_GET) {
      if (logLevel > DLOG_UNKNOWN) {
        map.insert(std::make_pair(flutter::EncodableValue(PARAM_LOG_LEVEL),
                                  flutter::EncodableValue(logLevel)));
      }
      if (databaseMap.size() > 0) {
        flutter::EncodableMap databasesInfo;
        for (auto entry : databaseMap) {
          auto [id, database] = entry;
          flutter::EncodableMap info;
          info.insert(std::make_pair(flutter::EncodableValue(PARAM_PATH),
                                     flutter::EncodableValue(database->path)));
          info.insert(std::make_pair(
              flutter::EncodableValue(PARAM_SINGLE_INSTANCE),
              flutter::EncodableValue(database->singleInstance)));
          if (database->logLevel > DLOG_UNKNOWN) {
            info.insert(
                std::make_pair(flutter::EncodableValue(PARAM_LOG_LEVEL),
                               flutter::EncodableValue(database->logLevel)));
          }
          databasesInfo.insert(
              std::make_pair(flutter::EncodableValue(id), info));
        }
        map.insert(std::make_pair(flutter::EncodableValue("databases"),
                                  databasesInfo));
      }
    }
    result->Success(flutter::EncodableValue(map));
  }

  void OnExecuteCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    int databaseId;
    std::string sql;
    DatabaseManager::parameters params;

    GetValueFromEncodableMap(arguments, PARAM_SQL_ARGUMENTS, params);
    GetValueFromEncodableMap(arguments, PARAM_SQL, sql);
    GetValueFromEncodableMap(arguments, PARAM_ID, databaseId);

    auto database = getDatabase(databaseId);
    if (database == nullptr) {
      result->Error(ERROR_DATABASE,
                    ERROR_DATABASE_CLOSED + " " + std::to_string(databaseId));
      return;
    }
    try {
      execute(database, sql, params);
    } catch (const DatabaseError &e) {
      result->Error(ERROR_DATABASE, e.what());
      return;
    }
    result->Success();
  }

  void execute(std::shared_ptr<DatabaseManager> database, std::string sql,
               DatabaseManager::parameters params) {
    database->execute(sql, params);
  }

  int64_t queryUpdateChanges(std::shared_ptr<DatabaseManager> database) {
    std::string changesSql = "SELECT changes();";
    auto [_, resultset] = database->query(changesSql);

    auto firstResult = resultset[0];
    return std::get<int64_t>(firstResult[0]);
  }

  std::pair<int64_t, int64_t> queryInsertChanges(
      std::shared_ptr<DatabaseManager> database) {
    std::string changesSql = "SELECT changes(), last_insert_rowid();";

    auto [_, resultset] = database->query(changesSql);
    auto firstResult = resultset[0];
    auto changes = std::get<int64_t>(firstResult[0]);
    int lastId = 0;
    if (changes > 0) {
      lastId = std::get<int64_t>(firstResult[1]);
    }
    return std::make_pair(changes, lastId);
  }

  flutter::EncodableValue update(std::shared_ptr<DatabaseManager> database,
                                 std::string sql,
                                 DatabaseManager::parameters params,
                                 bool noResult) {
    database->execute(sql, params);
    if (noResult) {
      LOG_DEBUG("ignoring insert result, 'noResult' is turned on");
      return flutter::EncodableValue();
    }

    auto changes = queryUpdateChanges(database);
    return flutter::EncodableValue(changes);
  }

  flutter::EncodableValue insert(std::shared_ptr<DatabaseManager> database,
                                 std::string sql,
                                 DatabaseManager::parameters params,
                                 bool noResult) {
    database->execute(sql, params);
    if (noResult) {
      LOG_DEBUG("ignoring insert result, 'noResult' is turned on");
      return flutter::EncodableValue();
    }

    auto [changes, lastId] = queryInsertChanges(database);

    if (changes == 0) {
      return flutter::EncodableValue();
    }
    return flutter::EncodableValue(lastId);
  }

  struct DBResultVisitor {
    flutter::EncodableValue operator()(int64_t val) {
      return flutter::EncodableValue(val);
    };
    flutter::EncodableValue operator()(std::string val) {
      return flutter::EncodableValue(val);
    };
    flutter::EncodableValue operator()(double val) {
      return flutter::EncodableValue(val);
    };
    flutter::EncodableValue operator()(std::vector<uint8_t> val) {
      return flutter::EncodableValue(val);
    };
    flutter::EncodableValue operator()(std::nullptr_t val) {
      return flutter::EncodableValue();
    };
  };

  flutter::EncodableValue query(std::shared_ptr<DatabaseManager> database,
                                std::string sql,
                                DatabaseManager::parameters params) {
    auto dbResultVisitor = DBResultVisitor{};
    auto [columns, resultset] = database->query(sql, params);
    if (queryAsMapList) {
      flutter::EncodableList response;
      if (resultset.size() == 0) {
        return flutter::EncodableValue(response);
      }
      for (auto row : resultset) {
        flutter::EncodableMap rowMap;
        for (size_t i = 0; i < row.size(); i++) {
          auto rowValue = std::visit(dbResultVisitor, row[i]);
          rowMap.insert(
              std::pair<flutter::EncodableValue, flutter::EncodableValue>(
                  flutter::EncodableValue(columns[i]), rowValue));
        }
        response.push_back(flutter::EncodableValue(rowMap));
      }
      return flutter::EncodableValue(response);
    } else {
      flutter::EncodableMap response;
      if (resultset.size() == 0) {
        return flutter::EncodableValue(response);
      }
      flutter::EncodableList colsResponse;
      flutter::EncodableList rowsResponse;
      for (auto col : columns) {
        colsResponse.push_back(flutter::EncodableValue(col));
      }
      for (auto row : resultset) {
        flutter::EncodableList rowList;
        for (auto col : row) {
          auto rowValue = std::visit(dbResultVisitor, col);
          rowList.push_back(rowValue);
        }
        rowsResponse.push_back(flutter::EncodableValue(rowList));
      }
      response.insert(
          std::pair<flutter::EncodableValue, flutter::EncodableValue>(
              flutter::EncodableValue(PARAM_COLUMNS),
              flutter::EncodableValue(colsResponse)));
      response.insert(
          std::pair<flutter::EncodableValue, flutter::EncodableValue>(
              flutter::EncodableValue(PARAM_ROWS),
              flutter::EncodableValue(rowsResponse)));
      return flutter::EncodableValue(response);
    }
  }

  void OnInsertCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    int databaseId;
    std::string sql;
    DatabaseManager::parameters params;
    bool noResult = false;

    GetValueFromEncodableMap(arguments, PARAM_SQL_ARGUMENTS, params);
    GetValueFromEncodableMap(arguments, PARAM_SQL, sql);
    GetValueFromEncodableMap(arguments, PARAM_ID, databaseId);
    GetValueFromEncodableMap(arguments, PARAM_NO_RESULT, noResult);

    auto database = getDatabase(databaseId);
    if (database == nullptr) {
      result->Error(ERROR_DATABASE,
                    ERROR_DATABASE_CLOSED + " " + std::to_string(databaseId));
      return;
    }
    flutter::EncodableValue response;
    try {
      response = insert(database, sql, params, noResult);
    } catch (const DatabaseError &e) {
      handleQueryException(e, sql, params, std::move(result));
      return;
    }
    result->Success(response);
  }

  void OnUpdateCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    int databaseId;
    std::string sql;
    DatabaseManager::parameters params;
    bool noResult = false;

    GetValueFromEncodableMap(arguments, PARAM_SQL_ARGUMENTS, params);
    GetValueFromEncodableMap(arguments, PARAM_SQL, sql);
    GetValueFromEncodableMap(arguments, PARAM_ID, databaseId);
    GetValueFromEncodableMap(arguments, PARAM_NO_RESULT, noResult);

    auto database = getDatabase(databaseId);
    if (database == nullptr) {
      result->Error(ERROR_DATABASE,
                    ERROR_DATABASE_CLOSED + " " + std::to_string(databaseId));
      return;
    }
    flutter::EncodableValue response;
    try {
      response = update(database, sql, params, noResult);
    } catch (const DatabaseError &e) {
      handleQueryException(e, sql, params, std::move(result));
      return;
    }
    result->Success(response);
  }

  void OnOptionsCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    bool paramsAsList = false;
    int logLevel = DLOG_UNKNOWN;

    GetValueFromEncodableMap(arguments, PARAM_QUERY_AS_MAP_LIST, paramsAsList);
    GetValueFromEncodableMap(arguments, PARAM_LOG_LEVEL, logLevel);

    queryAsMapList = paramsAsList;
    // TODO: Implement log level usage
    // TODO: Implement Thread Priority usage
    result->Success();
  }

  void OnQueryCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    int databaseId;
    std::string sql;
    DatabaseManager::parameters params;
    GetValueFromEncodableMap(arguments, PARAM_SQL_ARGUMENTS, params);
    GetValueFromEncodableMap(arguments, PARAM_SQL, sql);
    GetValueFromEncodableMap(arguments, PARAM_ID, databaseId);

    auto database = getDatabase(databaseId);
    if (database == nullptr) {
      result->Error(ERROR_DATABASE,
                    ERROR_DATABASE_CLOSED + " " + std::to_string(databaseId));
      return;
    }
    flutter::EncodableValue response;
    try {
      response = query(database, sql, params);
    } catch (const DatabaseError &e) {
      handleQueryException(e, sql, params, std::move(result));
      return;
    }
    result->Success(response);
  }

  void OnGetDatabasesPathCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    auto path = app_get_data_path();
    if (path == nullptr) {
      result->Error("storage_error", "not enough space to get data directory");
      return;
    }
    databasesPath = path;
    free(path);

    result->Success(flutter::EncodableValue(databasesPath));
  }

  void OnDeleteDatabase(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    std::string path;
    GetValueFromEncodableMap(arguments, PARAM_PATH, path);

    LOG_DEBUG("Trying to delete path %s", path.c_str());
    int *existingDatabaseId = getDatabaseId(path);
    if (existingDatabaseId) {
      LOG_DEBUG("db id exists: %d", *existingDatabaseId);
      auto dbm = getDatabase(*existingDatabaseId);
      if (dbm && dbm->sqliteDatabase) {
        databaseMap.erase(*existingDatabaseId);
        singleInstancesByPath.erase(path);
      }
    }
    // TODO: Safe check before delete.
    std::filesystem::remove(path);
    result->Success();
  }

  flutter::EncodableValue makeOpenResult(int databaseId,
                                         bool recoveredInTransaction) {
    flutter::EncodableMap response;
    response.insert(std::make_pair(flutter::EncodableValue("id"),
                                   flutter::EncodableValue(databaseId)));
    if (recoveredInTransaction) {
      response.insert(std::make_pair(
          flutter::EncodableValue(PARAM_RECOVERED_IN_TRANSACTION),
          flutter::EncodableValue(true)));
    }
    return flutter::EncodableValue(response);
  }

  void OnOpenDatabaseCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    std::string path;
    bool readOnly = false;
    bool singleInstance = false;

    GetValueFromEncodableMap(arguments, PARAM_PATH, path);
    GetValueFromEncodableMap(arguments, PARAM_READ_ONLY, readOnly);
    GetValueFromEncodableMap(arguments, PARAM_SINGLE_INSTANCE, singleInstance);

    const bool inMemory = isInMemoryPath(path);
    singleInstance = singleInstance && !inMemory;

    if (singleInstance) {
      int foundDatabaseId = 0;
      auto sit = singleInstancesByPath.find(path);
      if (sit != singleInstancesByPath.end()) {
        foundDatabaseId = sit->second;
      }
      if (foundDatabaseId) {
        auto dit = databaseMap.find(foundDatabaseId);
        if (dit != databaseMap.end()) {
          if (dit->second->sqliteDatabase) {
            auto response = makeOpenResult(foundDatabaseId, true);
            result->Success(response);
            return;
          }
        }
      }
    }
    // TODO: Protect with mutex
    const int newDatabaseId = ++databaseId;
    try {
      std::shared_ptr<DatabaseManager> databaseManager =
          std::make_shared<DatabaseManager>(path, newDatabaseId, singleInstance,
                                            0);
      if (!readOnly) {
        LOG_DEBUG("opening read-write database in path %s", path.c_str());
        databaseManager->open();
      } else {
        LOG_DEBUG("opening read only database in path %s", path.c_str());
        databaseManager->openReadOnly();
      }

      // Store dbid in internal map
      // TODO: Protect with mutex
      LOG_DEBUG("saving database id %d for path %s", databaseId, path.c_str());
      if (singleInstance) {
        singleInstancesByPath.insert(
            std::pair<std::string, int>(path, databaseId));
      }
      databaseMap.insert(std::pair<int, std::shared_ptr<DatabaseManager>>(
          databaseId, databaseManager));
    } catch (const DatabaseError &e) {
      LOG_DEBUG("ERROR: open db %s", e.what());
      result->Error(ERROR_DATABASE, ERROR_OPEN_FAILED + " " + path);
      return;
    }

    auto response = makeOpenResult(databaseId, false);
    result->Success(response);
  }

  void OnCloseDatabaseCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    int databaseId;
    GetValueFromEncodableMap(arguments, PARAM_ID, databaseId);

    auto database = getDatabase(databaseId);
    if (database == nullptr) {
      result->Error(ERROR_DATABASE,
                    ERROR_DATABASE_CLOSED + " " + std::to_string(databaseId));
      return;
    }

    auto path = database->path;

    try {
      LOG_DEBUG("closing database id %d in path %s", databaseId, path.c_str());
      // By erasing the entry from databaseMap, the destructor of
      // DatabaseManager is called, which finalizes all open statements and
      // closes the database.
      // TODO: Protect with mutex
      databaseMap.erase(databaseId);

      if (database->singleInstance) {
        singleInstancesByPath.erase(path);
      }
    } catch (const DatabaseError &e) {
      result->Error(ERROR_DATABASE, e.what());
      return;
    }

    result->Success();
  };

  flutter::EncodableValue buildSuccessBatchOperationResult(
      flutter::EncodableValue result) {
    flutter::EncodableMap operationResult;
    operationResult.insert(
        std::make_pair(flutter::EncodableValue(PARAM_RESULT), result));
    return flutter::EncodableValue(operationResult);
  }

  flutter::EncodableValue buildErrorBatchOperationResult(
      const DatabaseError &e, std::string sql,
      DatabaseManager::parameters params) {
    flutter::EncodableMap operationResult;
    flutter::EncodableMap operationErrorDetailResult;
    flutter::EncodableMap operationErrorDetailData;
    operationErrorDetailResult.insert(
        std::make_pair(flutter::EncodableValue(PARAM_ERROR_CODE),
                       flutter::EncodableValue(ERROR_DATABASE)));
    operationErrorDetailResult.insert(
        std::make_pair(flutter::EncodableValue(PARAM_ERROR_MESSAGE),
                       flutter::EncodableValue(e.what())));
    operationErrorDetailData.insert(std::make_pair(
        flutter::EncodableValue(PARAM_SQL), flutter::EncodableValue(sql)));
    operationErrorDetailData.insert(
        std::make_pair(flutter::EncodableValue(PARAM_SQL_ARGUMENTS),
                       flutter::EncodableValue(params)));
    operationErrorDetailResult.insert(
        std::make_pair(flutter::EncodableValue(PARAM_ERROR_DATA),
                       flutter::EncodableValue(operationErrorDetailData)));
    operationResult.insert(std::make_pair(flutter::EncodableValue(PARAM_ERROR),
                                          operationErrorDetailResult));
    return flutter::EncodableValue(operationResult);
  }

  void OnBatchCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    flutter::EncodableMap arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    int databaseId;
    bool continueOnError = false;
    bool noResult = false;
    flutter::EncodableList operations;
    flutter::EncodableList results;
    GetValueFromEncodableMap(arguments, PARAM_ID, databaseId);
    GetValueFromEncodableMap(arguments, PARAM_OPERATIONS, operations);
    GetValueFromEncodableMap(arguments, PARAM_CONTINUE_ON_ERROR,
                             continueOnError);
    GetValueFromEncodableMap(arguments, PARAM_NO_RESULT, noResult);

    auto database = getDatabase(databaseId);
    if (database == nullptr) {
      result->Error(ERROR_DATABASE,
                    ERROR_DATABASE_CLOSED + " " + std::to_string(databaseId));
      return;
    }

    for (auto item : operations) {
      auto itemMap = std::get<flutter::EncodableMap>(item);
      std::string method;
      std::string sql;
      DatabaseManager::parameters params;
      GetValueFromEncodableMap(itemMap, PARAM_METHOD, method);
      GetValueFromEncodableMap(itemMap, PARAM_SQL_ARGUMENTS, params);
      GetValueFromEncodableMap(itemMap, PARAM_SQL, sql);

      if (method == METHOD_EXECUTE) {
        try {
          execute(database, sql, params);
          if (!noResult) {
            auto operationResult =
                buildSuccessBatchOperationResult(flutter::EncodableValue());
            results.push_back(operationResult);
          }
        } catch (const DatabaseError &e) {
          if (!continueOnError) {
            handleQueryException(e, sql, params, std::move(result));
            return;
          } else {
            if (!noResult) {
              auto operationResult =
                  buildErrorBatchOperationResult(e, sql, params);
              results.push_back(operationResult);
            }
          }
        }
      } else if (method == METHOD_INSERT) {
        try {
          auto response = insert(database, sql, params, noResult);
          if (!noResult) {
            auto operationResult = buildSuccessBatchOperationResult(response);
            results.push_back(operationResult);
          }
        } catch (const DatabaseError &e) {
          if (!continueOnError) {
            handleQueryException(e, sql, params, std::move(result));
            return;
          } else {
            if (!noResult) {
              auto operationResult =
                  buildErrorBatchOperationResult(e, sql, params);
              results.push_back(operationResult);
            }
          }
        }
      } else if (method == METHOD_QUERY) {
        try {
          auto response = query(database, sql, params);
          if (!noResult) {
            auto operationResult = buildSuccessBatchOperationResult(response);
            results.push_back(operationResult);
          }
        } catch (const DatabaseError &e) {
          if (!continueOnError) {
            handleQueryException(e, sql, params, std::move(result));
            return;
          } else {
            if (!noResult) {
              auto operationResult =
                  buildErrorBatchOperationResult(e, sql, params);
              results.push_back(operationResult);
            }
          }
        }
      } else if (method == METHOD_UPDATE) {
        try {
          auto response = update(database, sql, params, noResult);
          if (!noResult) {
            auto operationResult = buildSuccessBatchOperationResult(response);
            results.push_back(operationResult);
          }
        } catch (const DatabaseError &e) {
          if (!continueOnError) {
            handleQueryException(e, sql, params, std::move(result));
            return;
          } else {
            if (!noResult) {
              auto operationResult =
                  buildErrorBatchOperationResult(e, sql, params);
              results.push_back(operationResult);
            }
          }
        }
      } else {
        result->NotImplemented();
        return;
      }
    }
    if (noResult) {
      result->Success();
    } else {
      result->Success(flutter::EncodableValue(results));
    }
  }

  flutter::PluginRegistrar *registrar_;
  std::unique_ptr<PermissionManager> pmm_;
};

void SqflitePluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  SqflitePlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrar>(registrar));
}
