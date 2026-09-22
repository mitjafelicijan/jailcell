#ifndef CELL_H
#define CELL_H

#include "sqlite3.h"

typedef struct {
	int id;
	char title[256];
	int is_file;
	long long created_at;
	long long updated_at;
	long long size;
} cell_item_t;

int cell_open(const char *filename, const char *password, sqlite3 **db);
int cell_init(sqlite3 *db);
int cell_add_note(sqlite3 *db, const char *title, const char *content);
int cell_add_file(sqlite3 *db, const char *path);
int cell_export_file(sqlite3 *db, int id, const char *output_path);
int cell_update_note(sqlite3 *db, int id, const char *content);
int cell_rename_item(sqlite3 *db, int id, int is_file, const char *new_name);
int cell_delete_item(sqlite3 *db, int id, int is_file);
int cell_list_notes(sqlite3 *db);
int cell_list_notes_json(sqlite3 *db);
int cell_list_notes_xml(sqlite3 *db);
int cell_get_note_json(sqlite3 *db, int id);
int cell_get_note_xml(sqlite3 *db, int id);
int cell_get_items(sqlite3 *db, cell_item_t **items, int *count);
int cell_get_note_content(sqlite3 *db, int id, char **content);
int cell_get_file_info(sqlite3 *db, int id, int *size, char **mime_type);
long long cell_get_creation_time(sqlite3 *db);

#endif
