#include "cell.h"
#include "cJSON.h"
#include "ezxml.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

static void silent_log(void *pArg, int iErrCode, const char *zMsg) {
	// Do nothing
}

int cell_open(const char *filename, const char *password, sqlite3 **db) {
	static int config_done = 0;
	if (!config_done) {
		sqlite3_config(SQLITE_CONFIG_LOG, silent_log, NULL);
		config_done = 1;
	}

	int rc = sqlite3_open(filename, db);
	if (rc != SQLITE_OK) {
		return rc;
	}

	// Attempt to suppress SQLCipher specific logging
	sqlite3_exec(*db, "PRAGMA cipher_log_level = 0;", NULL, NULL, NULL);

	if (password && strlen(password) > 0) {
		rc = sqlite3_key(*db, password, strlen(password));
		if (rc != SQLITE_OK) {
			sqlite3_close(*db);
			return rc;
		}
	}

	// Verify the key by attempting a simple operation
	rc = sqlite3_exec(*db, "SELECT count(*) FROM sqlite_master;", NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
		sqlite3_close(*db);
		return rc;
	}

	return SQLITE_OK;
}

int cell_init(sqlite3 *db) {
	const char *sql_notes =
		"CREATE TABLE IF NOT EXISTS notes ("
		"    id INTEGER PRIMARY KEY,"
		"    title TEXT NOT NULL,"
		"    content TEXT NOT NULL,"
		"    created_at INTEGER NOT NULL,"
		"    updated_at INTEGER NOT NULL"
		");";

	const char *sql_files =
		"CREATE TABLE IF NOT EXISTS files ("
		"    id INTEGER PRIMARY KEY,"
		"    name TEXT NOT NULL,"
		"    mime_type TEXT,"
		"    data BLOB NOT NULL,"
		"    created_at INTEGER NOT NULL,"
		"    updated_at INTEGER"
		");";

	char *err_msg = NULL;
	int rc = sqlite3_exec(db, sql_notes, NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		return rc;
	}

	rc = sqlite3_exec(db, sql_files, NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		return rc;
	}

	// Migration: Add updated_at to files if not exists
	sqlite3_exec(db, "ALTER TABLE files ADD COLUMN updated_at INTEGER;", NULL, NULL, NULL);
	// Fill updated_at with created_at for older entries
	sqlite3_exec(db, "UPDATE files SET updated_at = created_at WHERE updated_at IS NULL;", NULL, NULL, NULL);

	const char *sql_meta =
		"CREATE TABLE IF NOT EXISTS metadata ("
		"    key TEXT PRIMARY KEY,"
		"    value TEXT"
		");";
	rc = sqlite3_exec(db, sql_meta, NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL error: %s\n", err_msg);
		sqlite3_free(err_msg);
		return rc;
	}

	// Insert created_at if not present
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO metadata (key, value) VALUES ('created_at', ?);", -1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		char now_str[32];
		snprintf(now_str, sizeof(now_str), "%lld", (long long)time(NULL));
		sqlite3_bind_text(stmt, 1, now_str, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

	return SQLITE_OK;
}

int cell_add_note(sqlite3 *db, const char *title, const char *content) {
	const char *sql = "INSERT INTO notes (title, content, created_at, updated_at) VALUES (?, ?, ?, ?);";
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	time_t now = time(NULL);
	sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, (sqlite3_int64)now);
	sqlite3_bind_int64(stmt, 4, (sqlite3_int64)now);

	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int cell_add_file(sqlite3 *db, const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return SQLITE_IOERR;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	unsigned char *buffer = malloc(size);
	if (!buffer) {
		fclose(f);
		return SQLITE_NOMEM;
	}
	fread(buffer, 1, size, f);
	fclose(f);

	char *path_copy = strdup(path);
	char *name = basename(path_copy);

	const char *sql = "INSERT INTO files (name, data, created_at, updated_at) VALUES (?, ?, ?, ?);";
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		free(buffer);
		free(path_copy);
		return rc;
	}

	time_t now = time(NULL);
	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_blob(stmt, 2, buffer, size, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, (sqlite3_int64)now);
	sqlite3_bind_int64(stmt, 4, (sqlite3_int64)now);

	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	free(buffer);
	free(path_copy);

	return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int cell_export_file(sqlite3 *db, int id, const char *output_path) {
	const char *sql = "SELECT data FROM files WHERE id = ?;";
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	sqlite3_bind_int(stmt, 1, id);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const void *blob = sqlite3_column_blob(stmt, 0);
		int size = sqlite3_column_bytes(stmt, 0);

		FILE *f = fopen(output_path, "wb");
		if (!f) {
			sqlite3_finalize(stmt);
			return SQLITE_IOERR;
		}
		fwrite(blob, 1, size, f);
		fclose(f);
		rc = SQLITE_OK;
	} else {
		rc = SQLITE_NOTFOUND;
	}

	sqlite3_finalize(stmt);
	return rc;
}

int cell_update_note(sqlite3 *db, int id, const char *content) {
	const char *sql = "UPDATE notes SET content = ?, updated_at = ? WHERE id = ?;";
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	time_t now = time(NULL);
	sqlite3_bind_text(stmt, 1, content, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, (sqlite3_int64)now);
	sqlite3_bind_int(stmt, 3, id);

	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int cell_rename_item(sqlite3 *db, int id, int is_file, const char *new_name) {
	const char *sql_note = "UPDATE notes SET title = ?, updated_at = ? WHERE id = ?;";
	const char *sql_file = "UPDATE files SET name = ?, updated_at = ? WHERE id = ?;";
	const char *sql = is_file ? sql_file : sql_note;

	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	time_t now = time(NULL);
	sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 2, (sqlite3_int64)now);
	sqlite3_bind_int(stmt, 3, id);

	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

int cell_delete_item(sqlite3 *db, int id, int is_file) {
	const char *sql_note = "DELETE FROM notes WHERE id = ?;";
	const char *sql_file = "DELETE FROM files WHERE id = ?;";
	const char *sql = is_file ? sql_file : sql_note;

	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	sqlite3_bind_int(stmt, 1, id);

	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

static void format_size(long long bytes, char *buf, size_t buf_size) {
	const char *units[] = {"B", "KB", "MB", "GB", "TB"};
	int i = 0;
	double d_bytes = (double)bytes;
	while (d_bytes >= 1024 && i < 4) {
		d_bytes /= 1024;
		i++;
	}
	if (i == 0) {
		snprintf(buf, buf_size, "%lld %s", bytes, units[i]);
	} else if (d_bytes == (long long)d_bytes) {
		snprintf(buf, buf_size, "%.0f %s", d_bytes, units[i]);
	} else {
		snprintf(buf, buf_size, "%.1f %s", d_bytes, units[i]);
	}
}

int cell_list_notes(sqlite3 *db) {
	const char *sql =
		"SELECT id, title, 'Note' as type, created_at, updated_at, length(content) as size FROM notes "
		"UNION ALL "
		"SELECT id, name, 'File' as type, created_at, updated_at, length(data) as size FROM files "
		"ORDER BY updated_at DESC;";

	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	printf("%-4s | %-20s | %-10s | %-5s | %-19s | %-19s\n", "ID", "Name/Title", "Size", "Type", "Created", "Last Modified");
	printf("-----------------------------------------------------------------------------------------------\n");

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int id = sqlite3_column_int(stmt, 0);
		const unsigned char *name = sqlite3_column_text(stmt, 1);
		const unsigned char *type = sqlite3_column_text(stmt, 2);
		time_t created = (time_t)sqlite3_column_int64(stmt, 3);
		time_t modified = (time_t)sqlite3_column_int64(stmt, 4);
		long long size = sqlite3_column_int64(stmt, 5);

		char c_time_str[20], m_time_str[20], size_str[16];
		struct tm *tm_c = localtime(&created);
		strftime(c_time_str, 20, "%Y-%m-%d %H:%M:%S", tm_c);
		struct tm *tm_m = localtime(&modified);
		strftime(m_time_str, 20, "%Y-%m-%d %H:%M:%S", tm_m);

		format_size(size, size_str, sizeof(size_str));

		printf("%-4d | %-20.20s | %-10s | %-5s | %-19s | %-19s\n", id, name, size_str, type, c_time_str, m_time_str);
	}

	sqlite3_finalize(stmt);
	return SQLITE_OK;
}

int cell_list_notes_json(sqlite3 *db) {
	const char *sql =
		"SELECT id, title, 'Note' as type, created_at, updated_at, length(content) as size FROM notes "
		"UNION ALL "
		"SELECT id, name, 'File' as type, created_at, updated_at, length(data) as size FROM files "
		"ORDER BY updated_at DESC;";

	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	cJSON *root = cJSON_CreateArray();

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		cJSON *item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "id", sqlite3_column_int(stmt, 0));
		cJSON_AddStringToObject(item, "name", (const char*)sqlite3_column_text(stmt, 1));
		cJSON_AddStringToObject(item, "type", (const char*)sqlite3_column_text(stmt, 2));
		cJSON_AddNumberToObject(item, "created_at", (double)sqlite3_column_int64(stmt, 3));
		cJSON_AddNumberToObject(item, "updated_at", (double)sqlite3_column_int64(stmt, 4));
		cJSON_AddNumberToObject(item, "size", (double)sqlite3_column_int64(stmt, 5));
		cJSON_AddItemToArray(root, item);
	}

	char *json_str = cJSON_Print(root);
	if (json_str) {
		printf("%s\n", json_str);
		free(json_str);
	}

	cJSON_Delete(root);
	sqlite3_finalize(stmt);
	return SQLITE_OK;
}

int cell_list_notes_xml(sqlite3 *db) {
	const char *sql =
		"SELECT id, title, 'Note' as type, created_at, updated_at, length(content) as size FROM notes "
		"UNION ALL "
		"SELECT id, name, 'File' as type, created_at, updated_at, length(data) as size FROM files "
		"ORDER BY updated_at DESC;";

	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	ezxml_t root = ezxml_new("cell");

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ezxml_t item = ezxml_add_child(root, "item", 0);
		char buf[64];

		snprintf(buf, sizeof(buf), "%d", sqlite3_column_int(stmt, 0));
		ezxml_set_attr_d(item, "id", buf);
		ezxml_set_attr_d(item, "type", (const char*)sqlite3_column_text(stmt, 2));

		ezxml_set_txt_d(ezxml_add_child(item, "name", 0), (const char*)sqlite3_column_text(stmt, 1));

		snprintf(buf, sizeof(buf), "%lld", (long long)sqlite3_column_int64(stmt, 5));
		ezxml_set_txt_d(ezxml_add_child(item, "size", 0), buf);

		snprintf(buf, sizeof(buf), "%lld", (long long)sqlite3_column_int64(stmt, 3));
		ezxml_set_txt_d(ezxml_add_child(item, "created_at", 0), buf);

		snprintf(buf, sizeof(buf), "%lld", (long long)sqlite3_column_int64(stmt, 4));
		ezxml_set_txt_d(ezxml_add_child(item, "updated_at", 0), buf);
	}

	char *xml_str = ezxml_toxml(root);
	if (xml_str) {
		printf("%s\n", xml_str);
		free(xml_str);
	}

	ezxml_free(root);
	sqlite3_finalize(stmt);
	return SQLITE_OK;
}

int cell_get_note_json(sqlite3 *db, int id) {
	const char *sql_note = "SELECT id, title, content, created_at, updated_at FROM notes WHERE id = ?;";
	const char *sql_file = "SELECT id, name, mime_type, length(data) as size, created_at, updated_at FROM files WHERE id = ?;";

	sqlite3_stmt *stmt;
	cJSON *item = cJSON_CreateObject();
	int found = 0;

	// Try notes
	if (sqlite3_prepare_v2(db, sql_note, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, id);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			cJSON_AddNumberToObject(item, "id", sqlite3_column_int(stmt, 0));
			cJSON_AddStringToObject(item, "name", (const char*)sqlite3_column_text(stmt, 1));
			cJSON_AddStringToObject(item, "type", "Note");
			cJSON_AddStringToObject(item, "content", (const char*)sqlite3_column_text(stmt, 2));
			cJSON_AddNumberToObject(item, "created_at", (double)sqlite3_column_int64(stmt, 3));
			cJSON_AddNumberToObject(item, "updated_at", (double)sqlite3_column_int64(stmt, 4));
			found = 1;
		}
		sqlite3_finalize(stmt);
	}

	if (!found) {
		// Try files
		if (sqlite3_prepare_v2(db, sql_file, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, id);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				cJSON_AddNumberToObject(item, "id", sqlite3_column_int(stmt, 0));
				cJSON_AddStringToObject(item, "name", (const char*)sqlite3_column_text(stmt, 1));
				cJSON_AddStringToObject(item, "type", "File");
				cJSON_AddStringToObject(item, "mime_type", (const char*)sqlite3_column_text(stmt, 2));
				cJSON_AddNumberToObject(item, "size", (double)sqlite3_column_int64(stmt, 3));
				cJSON_AddNumberToObject(item, "created_at", (double)sqlite3_column_int64(stmt, 4));
				cJSON_AddNumberToObject(item, "updated_at", (double)sqlite3_column_int64(stmt, 5));
				found = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

	if (found) {
		char *json_str = cJSON_Print(item);
		if (json_str) {
			printf("%s\n", json_str);
			free(json_str);
		}
		cJSON_Delete(item);
		return SQLITE_OK;
	} else {
		cJSON_Delete(item);
		return SQLITE_NOTFOUND;
	}
}

int cell_get_note_xml(sqlite3 *db, int id) {
	const char *sql_note = "SELECT id, title, content, created_at, updated_at FROM notes WHERE id = ?;";
	const char *sql_file = "SELECT id, name, mime_type, length(data) as size, created_at, updated_at FROM files WHERE id = ?;";

	sqlite3_stmt *stmt;
	ezxml_t item = ezxml_new("item");
	int found = 0;
	char buf[64];

	// Try notes
	if (sqlite3_prepare_v2(db, sql_note, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, id);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			snprintf(buf, sizeof(buf), "%d", sqlite3_column_int(stmt, 0));
			ezxml_set_attr_d(item, "id", buf);
			ezxml_set_attr_d(item, "type", "Note");

			ezxml_set_txt_d(ezxml_add_child(item, "name", 0), (const char*)sqlite3_column_text(stmt, 1));
			ezxml_set_txt_d(ezxml_add_child(item, "content", 0), (const char*)sqlite3_column_text(stmt, 2));

			snprintf(buf, sizeof(buf), "%lld", (long long)sqlite3_column_int64(stmt, 3));
			ezxml_set_txt_d(ezxml_add_child(item, "created_at", 0), buf);

			snprintf(buf, sizeof(buf), "%lld", (long long)sqlite3_column_int64(stmt, 4));
			ezxml_set_txt_d(ezxml_add_child(item, "updated_at", 0), buf);
			found = 1;
		}
		sqlite3_finalize(stmt);
	}

	if (!found) {
		// Try files
		if (sqlite3_prepare_v2(db, sql_file, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_int(stmt, 1, id);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				snprintf(buf, sizeof(buf), "%d", sqlite3_column_int(stmt, 0));
				ezxml_set_attr_d(item, "id", buf);
				ezxml_set_attr_d(item, "type", "File");

				ezxml_set_txt_d(ezxml_add_child(item, "name", 0), (const char*)sqlite3_column_text(stmt, 1));
				ezxml_set_txt_d(ezxml_add_child(item, "mime_type", 0), (const char*)sqlite3_column_text(stmt, 2));

				snprintf(buf, sizeof(buf), "%lld", (long long)sqlite3_column_int64(stmt, 3));
				ezxml_set_txt_d(ezxml_add_child(item, "size", 0), buf);

				snprintf(buf, sizeof(buf), "%lld", (long long)sqlite3_column_int64(stmt, 4));
				ezxml_set_txt_d(ezxml_add_child(item, "created_at", 0), buf);

				snprintf(buf, sizeof(buf), "%lld", (long long)sqlite3_column_int64(stmt, 5));
				ezxml_set_txt_d(ezxml_add_child(item, "updated_at", 0), buf);
				found = 1;
			}
			sqlite3_finalize(stmt);
		}
	}

	if (found) {
		char *xml_str = ezxml_toxml(item);
		if (xml_str) {
			printf("%s\n", xml_str);
			free(xml_str);
		}
		ezxml_free(item);
		return SQLITE_OK;
	} else {
		ezxml_free(item);
		return SQLITE_NOTFOUND;
	}
}


int cell_get_items(sqlite3 *db, cell_item_t **items, int *count) {
	const char *sql =
		"SELECT id, title, 0 as is_file, created_at, updated_at, length(content) as size FROM notes "
		"UNION ALL "
		"SELECT id, name, 1 as is_file, created_at, updated_at, length(data) as size FROM files "
		"ORDER BY updated_at DESC;";
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	int capacity = 10;
	*items = malloc(sizeof(cell_item_t) * capacity);
	*count = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		if (*count >= capacity) {
			capacity *= 2;
			*items = realloc(*items, sizeof(cell_item_t) * capacity);
		}
		(*items)[*count].id = sqlite3_column_int(stmt, 0);
		strncpy((*items)[*count].title, (const char*)sqlite3_column_text(stmt, 1), 255);
		(*items)[*count].title[255] = '\0';
		(*items)[*count].is_file = sqlite3_column_int(stmt, 2);
		(*items)[*count].created_at = sqlite3_column_int64(stmt, 3);
		(*items)[*count].updated_at = sqlite3_column_int64(stmt, 4);
		(*items)[*count].size = sqlite3_column_int64(stmt, 5);
		(*count)++;
	}

	sqlite3_finalize(stmt);
	return SQLITE_OK;
}

int cell_get_note_content(sqlite3 *db, int id, char **content) {
	const char *sql = "SELECT content FROM notes WHERE id = ?;";
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	sqlite3_bind_int(stmt, 1, id);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *text = (const char*)sqlite3_column_text(stmt, 0);
		*content = strdup(text ? text : "");
		rc = SQLITE_OK;
	} else {
		rc = SQLITE_NOTFOUND;
	}

	sqlite3_finalize(stmt);
	return rc;
}

int cell_get_file_info(sqlite3 *db, int id, int *size, char **mime_type) {
	const char *sql = "SELECT length(data), mime_type FROM files WHERE id = ?;";
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) return rc;

	sqlite3_bind_int(stmt, 1, id);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		*size = sqlite3_column_int(stmt, 0);
		const char *mime = (const char*)sqlite3_column_text(stmt, 1);
		*mime_type = strdup(mime ? mime : "application/octet-stream");
		rc = SQLITE_OK;
	} else {
		rc = SQLITE_NOTFOUND;
	}

	sqlite3_finalize(stmt);
	return rc;
}

long long cell_get_creation_time(sqlite3 *db) {
	const char *sql = "SELECT value FROM metadata WHERE key = 'created_at';";
	sqlite3_stmt *stmt;
	long long created_at = 0;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			created_at = atoll((const char*)sqlite3_column_text(stmt, 0));
		}
		sqlite3_finalize(stmt);
	}
	return created_at;
}
