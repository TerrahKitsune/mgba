/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/script/sqlite.h>

#include <mgba/core/config.h>
#include <mgba/script/macros.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#include <sqlite3.h>

#define SQLITE_NAME_MAX 128

enum mScriptSQLiteStep {
	mSCRIPT_SQLITE_STEP_ERROR = -1,
	mSCRIPT_SQLITE_STEP_DONE = 0,
	mSCRIPT_SQLITE_STEP_ROW = 1,
};

struct mScriptSQLiteContext {
	char directory[PATH_MAX];
	char* lastError;
};

struct mScriptSQLite {
	sqlite3* db;
	sqlite3_stmt* stmt;
	char* lastError;
};

mSCRIPT_DECLARE_STRUCT(mScriptSQLiteContext);
mSCRIPT_DECLARE_STRUCT(mScriptSQLite);

static void _setError(char** lastError, const char* message) {
	free(*lastError);
	*lastError = message ? strdup(message) : NULL;
}

static void _setDbError(struct mScriptSQLite* conn) {
	_setError(&conn->lastError, sqlite3_errmsg(conn->db));
}

static bool _isAllowedNameChar(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	       c == ' ' || c == '_' || c == '-' || c == '.';
}

// Keeps only the last path component, appends ".db" if missing and rejects anything outside a small character set,
// so every name maps to a file directly inside the databases directory.
static bool _sanitizeName(const char* name, char* out, size_t outSize, const char** error) {
	const char* base = name;
	const char* c;
	for (c = name; *c; ++c) {
		if (*c == '/' || *c == '\\') {
			base = c + 1;
		}
	}
	size_t length = strlen(base);
	if (!length) {
		*error = "database name is empty";
		return false;
	}
	if (base[0] == '.') {
		*error = "database name must not start with '.'";
		return false;
	}
	for (c = base; *c; ++c) {
		if (!_isAllowedNameChar(*c)) {
			*error = "database name may only contain letters, digits, space, '_', '-' and '.'";
			return false;
		}
	}
	bool hasExtension = length >= 3 && strcasecmp(&base[length - 3], ".db") == 0;
	if (length + (hasExtension ? 0 : 3) >= outSize) {
		*error = "database name is too long";
		return false;
	}
	snprintf(out, outSize, "%s%s", base, hasExtension ? "" : ".db");
	return true;
}

static bool _isEmptyDatabase(sqlite3* db) {
	sqlite3_stmt* stmt;
	bool empty = false;
	if (sqlite3_prepare_v2(db, "PRAGMA page_count;", -1, &stmt, NULL) == SQLITE_OK) {
		empty = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 0;
		sqlite3_finalize(stmt);
	}
	return empty;
}

static struct mScriptValue* _openConnection(struct mScriptSQLiteContext* context, const char* path, bool isFile) {
	sqlite3* db = NULL;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
		_setError(&context->lastError, db ? sqlite3_errmsg(db) : "out of memory");
		sqlite3_close(db);
		return NULL;
	}
	// An existing database keeps its journal mode; a new one starts in WAL.
	if (isFile && _isEmptyDatabase(db)) {
		sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	}
	sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

	struct mScriptValue* value = mScriptValueAlloc(mSCRIPT_TYPE_MS_S(mScriptSQLite));
	struct mScriptSQLite* conn = calloc(1, sizeof(*conn));
	conn->db = db;
	value->value.opaque = conn;
	value->flags = mSCRIPT_VALUE_FLAG_FREE_BUFFER;
	_setError(&context->lastError, NULL);
	return value;
}

static struct mScriptValue* _mScriptSQLiteOpen(struct mScriptSQLiteContext* context, const char* name) {
	char fileName[SQLITE_NAME_MAX];
	const char* error;
	if (!_sanitizeName(name, fileName, sizeof(fileName), &error)) {
		_setError(&context->lastError, error);
		return NULL;
	}
	if (!VDirCreate(context->directory)) {
		_setError(&context->lastError, "cannot create the databases directory");
		return NULL;
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s" PATH_SEP "%s", context->directory, fileName);
	return _openConnection(context, path, true);
}

static struct mScriptValue* _mScriptSQLiteOpenMemory(struct mScriptSQLiteContext* context) {
	return _openConnection(context, ":memory:", false);
}

static const char* _mScriptSQLiteContextError(struct mScriptSQLiteContext* context) {
	return context->lastError;
}

static void _mScriptSQLiteContextDeinit(struct mScriptSQLiteContext* context) {
	_setError(&context->lastError, NULL);
}

static void _mScriptSQLiteFinish(struct mScriptSQLite* conn) {
	if (conn->stmt) {
		sqlite3_finalize(conn->stmt);
		conn->stmt = NULL;
	}
}

static void _mScriptSQLiteClose(struct mScriptSQLite* conn) {
	_mScriptSQLiteFinish(conn);
	if (conn->db) {
		sqlite3_close(conn->db);
		conn->db = NULL;
	}
	_setError(&conn->lastError, NULL);
}

static bool _requireOpen(struct mScriptSQLite* conn) {
	if (!conn->db) {
		_setError(&conn->lastError, "database is closed");
		return false;
	}
	return true;
}

// Prepares the first statement in `sql`; any further statements are ignored.
static bool _mScriptSQLitePrepare(struct mScriptSQLite* conn, struct mScriptString* sql) {
	_mScriptSQLiteFinish(conn);
	if (!_requireOpen(conn)) {
		return false;
	}
	if (sqlite3_prepare_v2(conn->db, sql->buffer, (int) sql->size, &conn->stmt, NULL) != SQLITE_OK) {
		_setDbError(conn);
		conn->stmt = NULL;
		return false;
	}
	return true;
}

static int32_t _mScriptSQLiteParamCount(struct mScriptSQLite* conn) {
	return conn->stmt ? sqlite3_bind_parameter_count(conn->stmt) : 0;
}

// Name without its ':', '@' or '$' prefix; nil for a nameless '?' parameter.
static const char* _mScriptSQLiteParamName(struct mScriptSQLite* conn, int32_t index) {
	if (!conn->stmt) {
		return NULL;
	}
	const char* name = sqlite3_bind_parameter_name(conn->stmt, index);
	return name ? name + 1 : NULL;
}

static bool _checkBind(struct mScriptSQLite* conn, int result) {
	if (result != SQLITE_OK) {
		_setDbError(conn);
		return false;
	}
	return true;
}

static bool _mScriptSQLiteBindNull(struct mScriptSQLite* conn, int32_t index) {
	return conn->stmt && _checkBind(conn, sqlite3_bind_null(conn->stmt, index));
}

static bool _mScriptSQLiteBindInt(struct mScriptSQLite* conn, int32_t index, int64_t value) {
	return conn->stmt && _checkBind(conn, sqlite3_bind_int64(conn->stmt, index, value));
}

static bool _mScriptSQLiteBindFloat(struct mScriptSQLite* conn, int32_t index, double value) {
	return conn->stmt && _checkBind(conn, sqlite3_bind_double(conn->stmt, index, value));
}

static bool _mScriptSQLiteBindText(struct mScriptSQLite* conn, int32_t index, struct mScriptString* value) {
	return conn->stmt && _checkBind(conn, sqlite3_bind_text(conn->stmt, index, value->buffer, (int) value->size, SQLITE_TRANSIENT));
}

// Advances to the next row. The statement is finalized once it is done or has failed.
static int32_t _mScriptSQLiteStep(struct mScriptSQLite* conn) {
	if (!conn->stmt) {
		return _requireOpen(conn) ? mSCRIPT_SQLITE_STEP_DONE : mSCRIPT_SQLITE_STEP_ERROR;
	}
	switch (sqlite3_step(conn->stmt)) {
	case SQLITE_ROW:
		return mSCRIPT_SQLITE_STEP_ROW;
	case SQLITE_DONE:
		_mScriptSQLiteFinish(conn);
		return mSCRIPT_SQLITE_STEP_DONE;
	default:
		_setDbError(conn);
		_mScriptSQLiteFinish(conn);
		return mSCRIPT_SQLITE_STEP_ERROR;
	}
}

static int32_t _mScriptSQLiteColumnCount(struct mScriptSQLite* conn) {
	return conn->stmt ? sqlite3_column_count(conn->stmt) : 0;
}

static bool _isColumnIndex(struct mScriptSQLite* conn, int32_t index) {
	return conn->stmt && index >= 1 && index <= sqlite3_column_count(conn->stmt);
}

// Columns are 1-based, as in Lua.
static const char* _mScriptSQLiteColumnName(struct mScriptSQLite* conn, int32_t index) {
	return _isColumnIndex(conn, index) ? sqlite3_column_name(conn->stmt, index - 1) : NULL;
}

static struct mScriptValue* _mScriptSQLiteColumn(struct mScriptSQLite* conn, int32_t index) {
	if (!_isColumnIndex(conn, index)) {
		return NULL;
	}
	int column = index - 1;
	struct mScriptValue* value;
	switch (sqlite3_column_type(conn->stmt, column)) {
	case SQLITE_INTEGER:
		value = mScriptValueAlloc(mSCRIPT_TYPE_MS_S64);
		value->value.s64 = sqlite3_column_int64(conn->stmt, column);
		return value;
	case SQLITE_FLOAT:
		value = mScriptValueAlloc(mSCRIPT_TYPE_MS_F64);
		value->value.f64 = sqlite3_column_double(conn->stmt, column);
		return value;
	case SQLITE_TEXT:
		return mScriptStringCreateFromBytes(sqlite3_column_text(conn->stmt, column), sqlite3_column_bytes(conn->stmt, column));
	case SQLITE_BLOB:
		return mScriptStringCreateFromBytes(sqlite3_column_blob(conn->stmt, column), sqlite3_column_bytes(conn->stmt, column));
	default:
		return NULL;
	}
}

static const char* _mScriptSQLiteError(struct mScriptSQLite* conn) {
	return conn->lastError;
}

mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mScriptSQLite, close, _mScriptSQLiteClose, 0);
mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mScriptSQLite, finish, _mScriptSQLiteFinish, 0);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, BOOL, prepare, _mScriptSQLitePrepare, 1, STR, sql);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, S32, paramCount, _mScriptSQLiteParamCount, 0);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, CHARP, paramName, _mScriptSQLiteParamName, 1, S32, index);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, BOOL, bindNull, _mScriptSQLiteBindNull, 1, S32, index);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, BOOL, bindInt, _mScriptSQLiteBindInt, 2, S32, index, S64, value);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, BOOL, bindFloat, _mScriptSQLiteBindFloat, 2, S32, index, F64, value);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, BOOL, bindText, _mScriptSQLiteBindText, 2, S32, index, STR, value);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, S32, step, _mScriptSQLiteStep, 0);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, S32, columnCount, _mScriptSQLiteColumnCount, 0);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, CHARP, columnName, _mScriptSQLiteColumnName, 1, S32, index);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, WRAPPER, column, _mScriptSQLiteColumn, 1, S32, index);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLite, CHARP, error, _mScriptSQLiteError, 0);

mSCRIPT_DEFINE_STRUCT(mScriptSQLite)
	mSCRIPT_DEFINE_INTERNAL
	mSCRIPT_DEFINE_CLASS_DOCSTRING("An internal SQLite connection; scripts use the sqlite table instead.")
	mSCRIPT_DEFINE_STRUCT_DEINIT_NAMED(mScriptSQLite, close)
	mSCRIPT_DEFINE_DOCSTRING("Finalizes any statement and closes the connection.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, close)
	mSCRIPT_DEFINE_DOCSTRING("Finalizes the current statement, if any.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, finish)
	mSCRIPT_DEFINE_DOCSTRING("Finalizes any current statement and prepares the first statement in the SQL text.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, prepare)
	mSCRIPT_DEFINE_DOCSTRING("Number of parameters in the current statement.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, paramCount)
	mSCRIPT_DEFINE_DOCSTRING("Name of a 1-based parameter without its prefix, or nil for a nameless parameter.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, paramName)
	mSCRIPT_DEFINE_DOCSTRING("Binds NULL to a 1-based parameter.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, bindNull)
	mSCRIPT_DEFINE_DOCSTRING("Binds an integer to a 1-based parameter.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, bindInt)
	mSCRIPT_DEFINE_DOCSTRING("Binds a floating-point number to a 1-based parameter.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, bindFloat)
	mSCRIPT_DEFINE_DOCSTRING("Binds a string to a 1-based parameter.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, bindText)
	mSCRIPT_DEFINE_DOCSTRING("Advances the current statement: 1 for a row, 0 when done, -1 on error.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, step)
	mSCRIPT_DEFINE_DOCSTRING("Number of columns in the current statement.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, columnCount)
	mSCRIPT_DEFINE_DOCSTRING("Name of a 1-based column.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, columnName)
	mSCRIPT_DEFINE_DOCSTRING("Value of a 1-based column in the current row, or nil for NULL.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, column)
	mSCRIPT_DEFINE_DOCSTRING("Message of the last error on this connection.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLite, error)
mSCRIPT_DEFINE_END;

mSCRIPT_DECLARE_STRUCT_VOID_METHOD(mScriptSQLiteContext, _deinit, _mScriptSQLiteContextDeinit, 0);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLiteContext, W(mScriptSQLite), open, _mScriptSQLiteOpen, 1, CHARP, name);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLiteContext, W(mScriptSQLite), openMemory, _mScriptSQLiteOpenMemory, 0);
mSCRIPT_DECLARE_STRUCT_METHOD(mScriptSQLiteContext, CHARP, error, _mScriptSQLiteContextError, 0);

mSCRIPT_DEFINE_STRUCT(mScriptSQLiteContext)
	mSCRIPT_DEFINE_INTERNAL
	mSCRIPT_DEFINE_CLASS_DOCSTRING("Internal SQLite entry point; scripts use the sqlite table instead.")
	mSCRIPT_DEFINE_STRUCT_DEINIT(mScriptSQLiteContext)
	mSCRIPT_DEFINE_DOCSTRING("Opens or creates a named database in the databases directory, or returns nil on error.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLiteContext, open)
	mSCRIPT_DEFINE_DOCSTRING("Opens a new in-memory database, or returns nil on error.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLiteContext, openMemory)
	mSCRIPT_DEFINE_DOCSTRING("Message of the last failed open.")
	mSCRIPT_DEFINE_STRUCT_METHOD(mScriptSQLiteContext, error)
mSCRIPT_DEFINE_END;

void mScriptContextAttachSQLite(struct mScriptContext* context) {
	struct mScriptSQLiteContext* sqliteContext = calloc(1, sizeof(*sqliteContext));
	mCoreConfigDirectory(sqliteContext->directory, sizeof(sqliteContext->directory));
	strncat(sqliteContext->directory, PATH_SEP "databases", sizeof(sqliteContext->directory) - strlen(sqliteContext->directory) - 1);

	struct mScriptValue* value = mScriptValueAlloc(mSCRIPT_TYPE_MS_S(mScriptSQLiteContext));
	value->flags = mSCRIPT_VALUE_FLAG_FREE_BUFFER;
	value->value.opaque = sqliteContext;

	mScriptContextSetGlobal(context, "_sqlite", value);
	mScriptContextSetDocstring(context, "_sqlite", "Internal SQLite entry point; scripts use the sqlite table instead");
}
