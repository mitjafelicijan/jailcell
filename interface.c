#define _GNU_SOURCE
#define _XOPEN_SOURCE_EXTENDED
#include <ncurses.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "interface.h"
#include "cell.h"

typedef enum {
	MODE_NORMAL,
	MODE_INSERT,
	MODE_COMMAND,
	MODE_FINDER
} app_mode_t;

typedef struct {
	sqlite3 *db;
	cell_item_t *items;
	int items_count;
	int selected_idx;
	int running;
	app_mode_t mode;
	char cmd_buf[256];
	int cmd_len;
	char finder_query[256];
	int finder_len;
	int *finder_matches;
	int finder_matches_count;
	int finder_selected;
	char cell_name[256];
	char messageline[256];
	char **lines;
	int line_count;
	int scroll_x, scroll_y;
	int cursor_x, cursor_y;
	int is_dirty;
	int is_binary;
	int binary_size;
	char *binary_mime;
	int w, h;
	int status_y;
	int cmd_y;
	int finder_h;
} app_state_t;

// State Management

void refresh_items(app_state_t *state) {
	int current_id = -1;
	int current_is_file = 0;
	if (state->selected_idx >= 0 && state->selected_idx < state->items_count) {
		current_id = state->items[state->selected_idx].id;
		current_is_file = state->items[state->selected_idx].is_file;
	}

	if (state->items) free(state->items);
	cell_get_items(state->db, &state->items, &state->items_count);

	// Try to find the previously selected item
	state->selected_idx = -1;
	if (current_id != -1) {
		for (int i = 0; i < state->items_count; i++) {
			if (state->items[i].id == current_id && state->items[i].is_file == current_is_file) {
				state->selected_idx = i;
				break;
			}
		}
	}
}

void update_finder(app_state_t *state) {
	if (state->finder_matches) free(state->finder_matches);
	state->finder_matches = malloc(sizeof(int) * state->items_count);
	state->finder_matches_count = 0;

	for (int i = 0; i < state->items_count; i++) {
		if (state->finder_len == 0 || strcasestr(state->items[i].title, state->finder_query)) {
			state->finder_matches[state->finder_matches_count++] = i;
		}
	}
	if (state->finder_selected >= state->finder_matches_count) {
		state->finder_selected = state->finder_matches_count - 1;
	}
	if (state->finder_selected < 0 && state->finder_matches_count > 0) {
		state->finder_selected = 0;
	}
}

void free_content(app_state_t *state) {
	if (state->lines) {
		for (int i = 0; i < state->line_count; i++) {
			free(state->lines[i]);
		}
		free(state->lines);
		state->lines = NULL;
	}
	state->line_count = 0;
	state->is_binary = 0;
	state->is_dirty = 0;
	if (state->binary_mime) {
		free(state->binary_mime);
		state->binary_mime = NULL;
	}
	state->binary_size = 0;
}

void sync_view(app_state_t *state) {
	int main_h = state->status_y;

	if (state->cursor_y < state->scroll_y) {
		state->scroll_y = state->cursor_y;
	} else if (state->cursor_y >= state->scroll_y + main_h) {
		state->scroll_y = state->cursor_y - main_h + 1;
	}

	if (state->cursor_x < state->scroll_x) {
		state->scroll_x = state->cursor_x;
	} else if (state->cursor_x >= state->scroll_x + state->w) {
		state->scroll_x = state->cursor_x - state->w + 1;
	}
}

void load_content(app_state_t *state) {
	free_content(state);
	state->scroll_x = 0;
	state->scroll_y = 0;
	state->cursor_x = 0;
	state->cursor_y = 0;

	if (state->selected_idx < 0 || state->selected_idx >= state->items_count) return;

	if (state->items[state->selected_idx].is_file) {
		state->is_binary = 1;
		cell_get_file_info(state->db, state->items[state->selected_idx].id, &state->binary_size, &state->binary_mime);
		return;
	}

	char *raw_content = NULL;
	if (cell_get_note_content(state->db, state->items[state->selected_idx].id, &raw_content) != SQLITE_OK) {
		return;
	}

	if (!raw_content) {
		// Create at least one empty line
		state->lines = malloc(sizeof(char *));
		state->lines[0] = strdup("");
		state->line_count = 1;
		return;
	}

	// Simple line splitting
	int capacity = 10;
	state->lines = malloc(sizeof(char *) * capacity);
	state->line_count = 0;

	char *start = raw_content;
	char *end;
	while ((end = strchr(start, '\n')) != NULL) {
		if (state->line_count >= capacity) {
			capacity *= 2;
			state->lines = realloc(state->lines, sizeof(char *) * capacity);
		}
		int len = end - start;
		state->lines[state->line_count] = malloc(len + 1);
		strncpy(state->lines[state->line_count], start, len);
		state->lines[state->line_count][len] = '\0';
		state->line_count++;
		start = end + 1;
	}
	// Last line if not followed by \n
	if (*start != '\0' || state->line_count == 0) {
		if (state->line_count >= capacity) {
			capacity++;
			state->lines = realloc(state->lines, sizeof(char *) * capacity);
		}
		state->lines[state->line_count] = strdup(start);
		state->line_count++;
	}

	free(raw_content);
}

void save_content(app_state_t *state) {
	if (state->selected_idx < 0 || state->is_binary) return;

	// Calculate total size
	size_t total_size = 0;
	for (int i = 0; i < state->line_count; i++) {
		total_size += strlen(state->lines[i]) + 1; // +1 for \n or \0
	}

	char *buffer = malloc(total_size + 1);
	buffer[0] = '\0';
	for (int i = 0; i < state->line_count; i++) {
		strcat(buffer, state->lines[i]);
		if (i < state->line_count - 1) {
			strcat(buffer, "\n");
		}
	}

	cell_update_note(state->db, state->items[state->selected_idx].id, buffer);
	free(buffer);
	state->is_dirty = 0;
	refresh_items(state);
}

void insert_text(app_state_t *state, const char *text) {
	if (state->is_binary || !state->lines) return;

	int text_len = strlen(text);
	char *line = state->lines[state->cursor_y];
	int line_len = strlen(line);

	state->lines[state->cursor_y] = realloc(line, line_len + text_len + 1);
	line = state->lines[state->cursor_y];

	memmove(line + state->cursor_x + text_len, line + state->cursor_x, line_len - state->cursor_x + 1);
	memcpy(line + state->cursor_x, text, text_len);

	state->cursor_x += text_len;
	state->is_dirty = 1;
	sync_view(state);
}

void split_line(app_state_t *state) {
	if (state->is_binary || !state->lines) return;

	char *line = state->lines[state->cursor_y];
	char *remainder = strdup(line + state->cursor_x);
	line[state->cursor_x] = '\0';

	state->lines = realloc(state->lines, sizeof(char *) * (state->line_count + 1));
	memmove(state->lines + state->cursor_y + 2, state->lines + state->cursor_y + 1, sizeof(char *) * (state->line_count - state->cursor_y - 1));

	state->lines[state->cursor_y + 1] = remainder;
	state->line_count++;
	state->cursor_y++;
	state->cursor_x = 0;
	state->is_dirty = 1;
	sync_view(state);
}

void delete_backspace(app_state_t *state) {
	if (state->is_binary || !state->lines) return;

	if (state->cursor_x > 0) {
		char *line = state->lines[state->cursor_y];
		int line_len = strlen(line);
		memmove(line + state->cursor_x - 1, line + state->cursor_x, line_len - state->cursor_x + 1);
		state->cursor_x--;
		state->is_dirty = 1;
	} else if (state->cursor_y > 0) {
		char *current_line = state->lines[state->cursor_y];
		char *prev_line = state->lines[state->cursor_y - 1];
		int prev_len = strlen(prev_line);
		int curr_len = strlen(current_line);

		state->lines[state->cursor_y - 1] = realloc(prev_line, prev_len + curr_len + 1);
		strcat(state->lines[state->cursor_y - 1], current_line);

		free(current_line);
		memmove(state->lines + state->cursor_y, state->lines + state->cursor_y + 1, sizeof(char *) * (state->line_count - state->cursor_y - 1));

		state->line_count--;
		state->cursor_y--;
		state->cursor_x = prev_len;
		state->is_dirty = 1;
	}
	sync_view(state);
}

// Component Initialization

void init_interface() {
	setlocale(LC_ALL, "");
	initscr();
	set_escdelay(25);
	raw();
	keypad(stdscr, TRUE);
	noecho();
	curs_set(0);

	// Mouse support
	mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);

	// Standard xterm sequences for Ctrl+Arrows
	define_key("\033[1;5A", 543); // Ctrl+Up
	define_key("\033[1;5B", 544); // Ctrl+Down
	define_key("\033[1;5D", 545); // Ctrl+Left
	define_key("\033[1;5C", 546); // Ctrl+Right
}

void init_statusline() {
	if (has_colors()) {
		start_color();
		if (COLORS >= 256) {
			init_pair(1, COLOR_BLACK, 250);
		} else {
			init_pair(1, COLOR_BLACK, COLOR_CYAN);
		}
	}
}

void init_messageline(app_state_t *state) {
	state->messageline[0] = '\0';
}

// Component Rendering

void format_size(int bytes, char *out) {
	const char *units[] = {"B", "KB", "MB", "GB", "TB"};
	int i = 0;
	double size = bytes;
	while (size >= 1024 && i < 4) {
		size /= 1024;
		i++;
	}
	sprintf(out, "%.2f %s", size, units[i]);
}

void update_main_area(app_state_t *state) {
	if (state->is_binary) {
		char size_str[32];
		format_size(state->binary_size, size_str);
		mvprintw(0, 0, "[Binary Content]");
		mvprintw(1, 0, "Size: %s", size_str);
		mvprintw(2, 0, "Type: %s", state->binary_mime ? state->binary_mime : "unknown");
		return;
	}

	if (!state->lines) return;

	int main_h = state->status_y;
	for (int i = 0; i < main_h && (i + state->scroll_y) < state->line_count; i++) {
		char *line = state->lines[i + state->scroll_y];
		int len = strlen(line);
		if (state->scroll_x < len) {
			// Never word wrap: just draw the slice of the line that fits
			mvaddnstr(i, 0, line + state->scroll_x, state->w);
		}
	}
}

void update_statusline(app_state_t *state) {
	attron(COLOR_PAIR(1));
	move(state->status_y, 0);
	for (int x = 0; x < state->w; x++) mvaddch(state->status_y, x, ' ');

	char mode_str[32] = "";
	switch(state->mode) {
		case MODE_NORMAL:  strcpy(mode_str, "NORMAL"); break;
		case MODE_INSERT:  strcpy(mode_str, "INSERT"); break;
		case MODE_COMMAND: strcpy(mode_str, "COMMAND"); break;
		case MODE_FINDER:  strcpy(mode_str, "FINDER"); break;
	}

	mvprintw(state->status_y, 0, "[%s]", mode_str);
	if (state->selected_idx >= 0 && state->selected_idx < state->items_count) {
		mvprintw(state->status_y, 10, "%s%s", state->items[state->selected_idx].title, state->is_dirty ? " (*)" : "");
	} else {
		mvprintw(state->status_y, 10, "%s", state->cell_name);
	}

	// Right-aligned cell name
	int cell_name_len = strlen(state->cell_name);
	if (state->w > 40) { // Only draw if we have enough space
		mvprintw(state->status_y, state->w - cell_name_len - 1, "%s", state->cell_name);
	}

	attroff(COLOR_PAIR(1));
}

void update_finder_drawer(app_state_t *state) {
	if (state->mode != MODE_FINDER) return;

	int list_y = state->status_y + 1;
	for (int i = 0; i < state->finder_h && i < state->finder_matches_count; i++) {
		int idx = state->finder_matches[i];
		cell_item_t *item = &state->items[idx];

		char c_time_str[32], u_time_str[32], size_str[32];
		struct tm *tm_c = localtime((time_t *)&item->created_at);
		strftime(c_time_str, sizeof(c_time_str), "%Y-%m-%d %H:%M:%S", tm_c);
		struct tm *tm_u = localtime((time_t *)&item->updated_at);
		strftime(u_time_str, sizeof(u_time_str), "%Y-%m-%d %H:%M:%S", tm_u);
		format_size((int)item->size, size_str);

		char info_str[256];
		snprintf(info_str, sizeof(info_str), "%s   C:%s   U:%s", size_str, c_time_str, u_time_str);
		int info_len = strlen(info_str);

		if (i == state->finder_selected) {
			attron(A_REVERSE);
			for (int x = 0; x < state->w; x++) mvaddch(list_y + i, x, ' ');

			int max_title_len = state->w - info_len - 10;
			if (max_title_len < 10) max_title_len = 10;

			mvaddch(list_y + i, 2, item->is_file ? 'f' : 'n');
			mvprintw(list_y + i, 5, "%.*s", max_title_len, item->title);

			if (state->w > info_len + 10) {
				mvprintw(list_y + i, state->w - info_len - 2, "%s", info_str);
			}
			attroff(A_REVERSE);
		} else {
			int max_title_len = state->w - info_len - 10;
			if (max_title_len < 10) max_title_len = 10;

			attron(A_DIM);
			mvaddch(list_y + i, 2, item->is_file ? 'f' : 'n');
			attroff(A_DIM);
			mvprintw(list_y + i, 5, "%.*s", max_title_len, item->title);

			if (state->w > info_len + 10) {
				attron(A_DIM);
				mvprintw(list_y + i, state->w - info_len - 2, "%s", info_str);
				attroff(A_DIM);
			}
		}
	}
}

void update_commandline(app_state_t *state) {
	if (state->mode != MODE_COMMAND && state->mode != MODE_FINDER) return;

	move(state->cmd_y, 0);
	for (int x = 0; x < state->w; x++) mvaddch(state->cmd_y, x, ' ');

	if (state->mode == MODE_COMMAND) {
		mvprintw(state->cmd_y, 0, ":%s", state->cmd_buf);
	} else if (state->mode == MODE_FINDER) {
		mvprintw(state->cmd_y, 0, "> %s", state->finder_query);
	}
}

void update_messageline(app_state_t *state) {
	if (state->mode == MODE_COMMAND || state->mode == MODE_FINDER) return;
	if (state->messageline[0] == '\0') return;

	move(state->cmd_y, 0);
	for (int x = 0; x < state->w; x++) mvaddch(state->cmd_y, x, ' ');
	mvprintw(state->cmd_y, 0, "%s", state->messageline);
}

void expand_path(const char *path, char *out, size_t out_size) {
	if (path[0] == '~') {
		const char *home = getenv("HOME");
		if (home) {
			snprintf(out, out_size, "%s%s", home, path + 1);
			return;
		}
	}
	strncpy(out, path, out_size - 1);
	out[out_size - 1] = '\0';
}

void execute_export(app_state_t *state, const char *path) {
	if (state->selected_idx < 0) {
		snprintf(state->messageline, sizeof(state->messageline), "Error: No item opened");
		return;
	}

	char expanded[512];
	expand_path(path, expanded, sizeof(expanded));

	if (state->is_binary) {
		if (cell_export_file(state->db, state->items[state->selected_idx].id, expanded) == SQLITE_OK) {
			snprintf(state->messageline, sizeof(state->messageline), "Exported binary to %s", path);
		} else {
			snprintf(state->messageline, sizeof(state->messageline), "Error: Export failed");
		}
	} else {
		FILE *f = fopen(expanded, "w");
		if (!f) {
			snprintf(state->messageline, sizeof(state->messageline), "Error: Could not open file for writing");
			return;
		}
		for (int i = 0; i < state->line_count; i++) {
			fprintf(f, "%s", state->lines[i]);
			if (i < state->line_count - 1) fprintf(f, "\n");
		}
		fclose(f);
		snprintf(state->messageline, sizeof(state->messageline), "Exported note to %s", path);
	}
}

void move_cursor_word(app_state_t *state, int direction) {
	if (state->is_binary || !state->lines || state->line_count == 0) return;

	if (direction == 1) { // Forward (w)
		char *line = state->lines[state->cursor_y];
		int x = state->cursor_x;

		// If at end of line, move to next line
		if (line[x] == '\0') {
			if (state->cursor_y < state->line_count - 1) {
				state->cursor_y++;
				state->cursor_x = 0;
				// If it's an empty line, we stay here as it's a stop
				if (strlen(state->lines[state->cursor_y]) == 0) goto done;
				// Otherwise continue from start of next line
				line = state->lines[state->cursor_y];
				x = 0;
			} else {
				goto done;
			}
		}

		// 1. If on word character, skip the word
		if (isalnum(line[x]) || line[x] == '_') {
			while (line[x] != '\0' && (isalnum(line[x]) || line[x] == '_')) x++;
		}
		// 2. If on non-word, non-space character (punctuation), skip it
		else if (!isspace(line[x])) {
			while (line[x] != '\0' && !isalnum(line[x]) && line[x] != '_' && !isspace(line[x])) x++;
		}

		// 3. Skip whitespace
		while (line[x] != '\0' && isspace(line[x])) x++;

		// 4. If reached end of line and not on a word yet, recurse to next line start
		if (line[x] == '\0') {
			state->cursor_x = x;
			move_cursor_word(state, 1);
			return;
		}

		state->cursor_x = x;

	} else { // Backward (b)
		if (state->cursor_x == 0) {
			if (state->cursor_y > 0) {
				state->cursor_y--;
				state->cursor_x = strlen(state->lines[state->cursor_y]);
				if (state->cursor_x == 0) goto done; // Stop on empty line
			} else {
				goto done;
			}
		}

		char *line = state->lines[state->cursor_y];
		int x = state->cursor_x;

		// 1. Skip leading whitespace to the left
		while (x > 0 && isspace(line[x-1])) x--;

		if (x == 0) {
			state->cursor_x = 0;
			goto done;
		}

		// 2. Determine type of char at current (x-1) and skip that type
		if (isalnum(line[x-1]) || line[x-1] == '_') {
			while (x > 0 && (isalnum(line[x-1]) || line[x-1] == '_')) x--;
		} else {
			while (x > 0 && !isalnum(line[x-1]) && line[x-1] != '_' && !isspace(line[x-1])) x--;
		}

		state->cursor_x = x;
	}

done:
	sync_view(state);
}

void move_cursor_paragraph(app_state_t *state, int direction) {
	if (state->is_binary || !state->lines || state->line_count == 0) return;

	if (direction == 1) { // Down (})
		if (state->cursor_y < state->line_count - 1) state->cursor_y++;
		while (state->cursor_y < state->line_count - 1 && strlen(state->lines[state->cursor_y]) == 0) state->cursor_y++;
		while (state->cursor_y < state->line_count - 1 && strlen(state->lines[state->cursor_y]) > 0) state->cursor_y++;
	} else { // Up ({)
		if (state->cursor_y > 0) state->cursor_y--;
		while (state->cursor_y > 0 && strlen(state->lines[state->cursor_y]) == 0) state->cursor_y--;
		while (state->cursor_y > 0 && strlen(state->lines[state->cursor_y]) > 0) state->cursor_y--;
	}
	state->cursor_x = 0;
	sync_view(state);
}

void handle_mouse(app_state_t *state) {
	MEVENT event;
	if (getmouse(&event) == OK) {
		// Scroll wheel (button 4 is up, button 5 is down usually)
		#if defined(NCURSES_MOUSE_VERSION) && NCURSES_MOUSE_VERSION >= 2
		if (event.bstate & BUTTON4_PRESSED || event.bstate & 0x10000) { // 0x10000 is often scroll up
			if (state->scroll_y > 0) state->scroll_y--;
		} else if (event.bstate & BUTTON5_PRESSED || event.bstate & 0x00200000) { // 0x00200000 is often scroll down
			if (state->scroll_y < state->line_count - 1) state->scroll_y++;
		}
		#else
		if (event.bstate & BUTTON4_PRESSED) {
			if (state->scroll_y > 0) state->scroll_y--;
		} else if (event.bstate & BUTTON5_PRESSED) {
			if (state->scroll_y < state->line_count - 1) state->scroll_y++;
		}
		#endif
	}
}

void delete_word_back(app_state_t *state) {
	if (state->is_binary || !state->lines || state->line_count == 0) return;
	if (state->cursor_x == 0) return;

	char *line = state->lines[state->cursor_y];
	int end_x = state->cursor_x;
	int x = end_x;

	// Skip leading whitespace to the left
	while (x > 0 && isspace(line[x-1])) x--;

	if (x > 0) {
		// Determine type of char at current (x-1) and skip that type
		if (isalnum(line[x-1]) || line[x-1] == '_') {
			while (x > 0 && (isalnum(line[x-1]) || line[x-1] == '_')) x--;
		} else {
			while (x > 0 && !isalnum(line[x-1]) && line[x-1] != '_' && !isspace(line[x-1])) x--;
		}
	}

	int start_x = x;
	int line_len = strlen(line);

	memmove(line + start_x, line + end_x, line_len - end_x + 1);
	state->cursor_x = start_x;
	state->is_dirty = 1;
	sync_view(state);
}

void sync_physical_cursor(app_state_t *state) {

	if (state->mode == MODE_COMMAND) {
		curs_set(1);
		move(state->cmd_y, state->cmd_len + 1);
	} else if (state->mode == MODE_FINDER) {
		curs_set(1);
		move(state->cmd_y, state->finder_len + 2);
	} else if (state->selected_idx >= 0 && !state->is_binary) {
		curs_set(1);
		move(state->cursor_y - state->scroll_y, state->cursor_x - state->scroll_x);
	} else {
		curs_set(0);
	}
}

void interface_draw(app_state_t *state) {
	getmaxyx(stdscr, state->h, state->w);
	erase();

	state->finder_h = 0;
	if (state->mode == MODE_FINDER) {
		state->finder_h = state->h / 3;
		if (state->finder_h < 5) state->finder_h = 5;
	}

	state->cmd_y = state->h - 1;
	state->status_y = state->h - 2;
	if (state->mode == MODE_FINDER) state->status_y = state->h - state->finder_h - 2;

	update_main_area(state);
	update_statusline(state);
	update_finder_drawer(state);
	update_commandline(state);
	update_messageline(state);
	sync_physical_cursor(state);

	refresh();
}

// Orchestration

void execute_reload(app_state_t *state) {
	if (state->selected_idx >= 0) {
		load_content(state);
		snprintf(state->messageline, sizeof(state->messageline), "Reloaded from cell");
	} else {
		snprintf(state->messageline, sizeof(state->messageline), "Error: No item opened");
	}
}

void execute_new_note(app_state_t *state, const char *title) {
	if (cell_add_note(state->db, title, "") == SQLITE_OK) {
		refresh_items(state);
		state->selected_idx = 0; // New notes are ordered by updated_at DESC
		load_content(state);
		snprintf(state->messageline, sizeof(state->messageline), "Created note: %.200s", title);
	} else {
		snprintf(state->messageline, sizeof(state->messageline), "Error: Failed to create note");
	}
}

void execute_delete(app_state_t *state) {
	if (state->selected_idx >= 0) {
		cell_item_t *item = &state->items[state->selected_idx];
		char deleted_name[256];
		strncpy(deleted_name, item->title, 255);
		deleted_name[255] = '\0';
		if (cell_delete_item(state->db, item->id, item->is_file) == SQLITE_OK) {
			free_content(state);
			refresh_items(state);
			state->selected_idx = -1;
			snprintf(state->messageline, sizeof(state->messageline), "Deleted %.200s", deleted_name);
		} else {
			snprintf(state->messageline, sizeof(state->messageline), "Error: Deletion failed");
		}
	} else {
		snprintf(state->messageline, sizeof(state->messageline), "Error: No item opened");
	}
}

void execute_rename(app_state_t *state, const char *new_name) {
	if (state->selected_idx >= 0) {
		cell_item_t *item = &state->items[state->selected_idx];
		if (cell_rename_item(state->db, item->id, item->is_file, new_name) == SQLITE_OK) {
			refresh_items(state);
			state->selected_idx = 0; // Renamed items move to top due to updated_at
			load_content(state);
			snprintf(state->messageline, sizeof(state->messageline), "Renamed to %.200s", new_name);
		} else {
			snprintf(state->messageline, sizeof(state->messageline), "Error: Rename failed");
		}
	} else {
		snprintf(state->messageline, sizeof(state->messageline), "Error: No item opened");
	}
}

void process_command(app_state_t *state) {
	if (strcmp(state->cmd_buf, "q") == 0) {
		state->running = 0;
	} else if (strcmp(state->cmd_buf, "w") == 0) {
		save_content(state);
		snprintf(state->messageline, sizeof(state->messageline), "Saved to cell");
	} else if (strcmp(state->cmd_buf, "wq") == 0) {
		save_content(state);
		state->running = 0;
	} else if (strcmp(state->cmd_buf, "d") == 0 || strcmp(state->cmd_buf, "delete") == 0) {
		execute_delete(state);
	} else if (strncmp(state->cmd_buf, "export ", 7) == 0) {
		execute_export(state, state->cmd_buf + 7);
	} else if (strncmp(state->cmd_buf, "n ", 2) == 0 || strncmp(state->cmd_buf, "new ", 4) == 0) {
		const char *title = (state->cmd_buf[1] == ' ') ? state->cmd_buf + 2 : state->cmd_buf + 4;
		execute_new_note(state, title);
	} else if (strncmp(state->cmd_buf, "r ", 2) == 0 || strncmp(state->cmd_buf, "rename ", 7) == 0) {
		const char *new_name = (state->cmd_buf[1] == ' ') ? state->cmd_buf + 2 : state->cmd_buf + 7;
		execute_rename(state, new_name);
	} else if (strcmp(state->cmd_buf, "e") == 0) {
		execute_reload(state);
	}
}

void handle_normal_mode(app_state_t *state, int ch) {
	switch (ch) {
		case ':':
			state->mode = MODE_COMMAND;
			state->messageline[0] = '\0';
			state->cmd_len = 0;
			state->cmd_buf[0] = '\0';
			break;
		case 16: // Ctrl+P
			refresh_items(state);
			state->mode = MODE_FINDER;
			state->messageline[0] = '\0';
			state->finder_len = 0;
			state->finder_query[0] = '\0';
			state->finder_selected = 0;
			update_finder(state);
			break;
		case 545: // Ctrl+Left
		case 546: // Ctrl+Right
			move_cursor_word(state, (ch == 546) ? 1 : -1);
			break;
		case 543: // Ctrl+Up
		case 544: // Ctrl+Down
		case '{':
		case '}':
			move_cursor_paragraph(state, (ch == 544 || ch == '}') ? 1 : -1);
			break;
		case 'i':
			if (state->selected_idx >= 0 && !state->is_binary) {
				state->mode = MODE_INSERT;
			}
			break;
		case 'w':
			move_cursor_word(state, 1);
			break;
		case 'b':
			move_cursor_word(state, -1);
			break;
		case 'A':
			if (state->selected_idx >= 0 && !state->is_binary && state->lines) {
				state->cursor_x = strlen(state->lines[state->cursor_y]);
				state->mode = MODE_INSERT;
				sync_view(state);
			}
			break;
		case 'I':
			if (state->selected_idx >= 0 && !state->is_binary && state->lines) {
				state->cursor_x = 0;
				state->mode = MODE_INSERT;
				sync_view(state);
			}
			break;
		case 'C':
			if (state->selected_idx >= 0 && !state->is_binary && state->lines) {
				state->lines[state->cursor_y][state->cursor_x] = '\0';
				state->is_dirty = 1;
				state->mode = MODE_INSERT;
				sync_view(state);
			}
			break;
		case 'D':
			if (state->selected_idx >= 0 && !state->is_binary && state->lines) {
				state->lines[state->cursor_y][state->cursor_x] = '\0';
				state->is_dirty = 1;
				sync_view(state);
			}
			break;
		case 'j':
		case KEY_DOWN:
			if (state->cursor_y < state->line_count - 1) {
				state->cursor_y++;
				int len = strlen(state->lines[state->cursor_y]);
				if (state->cursor_x > len) state->cursor_x = len;
			}
			sync_view(state);
			break;
		case 'k':
		case KEY_UP:
			if (state->cursor_y > 0) {
				state->cursor_y--;
				int len = strlen(state->lines[state->cursor_y]);
				if (state->cursor_x > len) state->cursor_x = len;
			}
			sync_view(state);
			break;
		case 'h':
		case KEY_LEFT:
			if (state->cursor_x > 0) state->cursor_x--;
			else if (state->cursor_y > 0) {
				state->cursor_y--;
				state->cursor_x = strlen(state->lines[state->cursor_y]);
			}
			sync_view(state);
			break;
		case 'l':
		case KEY_RIGHT:
			if (state->lines && state->cursor_x < (int)strlen(state->lines[state->cursor_y])) state->cursor_x++;
			else if (state->cursor_y < state->line_count - 1) {
				state->cursor_y++;
				state->cursor_x = 0;
			}
			sync_view(state);
			break;
	}
}

void handle_command_mode(app_state_t *state, int ch) {
	if (ch == 27) { // ESC
		state->mode = MODE_NORMAL;
	} else if (ch == '\n' || ch == KEY_ENTER) {
		process_command(state);
		state->mode = MODE_NORMAL;
	} else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
		if (state->cmd_len > 0) {
			state->cmd_buf[--state->cmd_len] = '\0';
		} else {
			state->mode = MODE_NORMAL;
		}
	} else if (ch >= 32 && ch <= 126) {
		if (state->cmd_len < 255) {
			state->cmd_buf[state->cmd_len++] = (char)ch;
			state->cmd_buf[state->cmd_len] = '\0';
		}
	}
}

void handle_finder_mode(app_state_t *state, int ch) {
	if (ch == 27) { // ESC
		state->mode = MODE_NORMAL;
	} else if (ch == '\n' || ch == KEY_ENTER) {
		if (state->finder_matches_count > 0) {
			state->selected_idx = state->finder_matches[state->finder_selected];
			load_content(state);
		}
		state->mode = MODE_NORMAL;
	} else if (ch == KEY_UP || ch == (int)'k') {
		if (state->finder_selected > 0) state->finder_selected--;
	} else if (ch == KEY_DOWN || ch == (int)'j') {
		if (state->finder_selected < state->finder_matches_count - 1) state->finder_selected++;
	} else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
		if (state->finder_len > 0) {
			state->finder_query[--state->finder_len] = '\0';
			state->finder_selected = 0;
			update_finder(state);
		} else {
			state->mode = MODE_NORMAL;
		}
	} else if (ch >= 32 && ch <= 126) {
		if (state->finder_len < 255) {
			state->finder_query[state->finder_len++] = (char)ch;
			state->finder_query[state->finder_len] = '\0';
			state->finder_selected = 0;
			update_finder(state);
		}
	}
}

void handle_insert_mode(app_state_t *state, int ch) {
	if (ch == 27) { // ESC
		state->mode = MODE_NORMAL;
	} else if (ch == '\n' || ch == KEY_ENTER) {
		split_line(state);
	} else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
		delete_backspace(state);
	} else if (ch == '\t') {
		insert_text(state, "    ");
	} else if (ch >= 32 && ch <= 126) {
		char buf[2] = {(char)ch, '\0'};
		insert_text(state, buf);
	} else if (ch == 545 || ch == 546) { // Ctrl+Left or Ctrl+Right
		move_cursor_word(state, (ch == 546) ? 1 : -1);
	} else if (ch == 23) { // Ctrl+W
		delete_word_back(state);
	} else if (ch == 543 || ch == 544) { // Ctrl+Up or Ctrl+Down
		move_cursor_paragraph(state, (ch == 544) ? 1 : -1);
	} else if (ch == KEY_UP) {
		if (state->cursor_y > 0) {
			state->cursor_y--;
			int len = strlen(state->lines[state->cursor_y]);
			if (state->cursor_x > len) state->cursor_x = len;
		}
		sync_view(state);
	} else if (ch == KEY_DOWN) {
		if (state->cursor_y < state->line_count - 1) {
			state->cursor_y++;
			int len = strlen(state->lines[state->cursor_y]);
			if (state->cursor_x > len) state->cursor_x = len;
		}
		sync_view(state);
	} else if (ch == KEY_LEFT) {
		if (state->cursor_x > 0) state->cursor_x--;
		sync_view(state);
	} else if (ch == KEY_RIGHT) {
		if (state->cursor_x < (int)strlen(state->lines[state->cursor_y])) state->cursor_x++;
		sync_view(state);
	}
}

void interface_start(sqlite3 *db, const char *cell_name) {
	init_interface();
	init_statusline();

	app_state_t state = {0};
	init_messageline(&state);
	state.db = db;
	strncpy(state.cell_name, cell_name, 255);
	state.running = 1;
	state.selected_idx = -1;
	state.mode = MODE_NORMAL;

	refresh_items(&state);

	while (state.running) {
		interface_draw(&state);
		int ch = getch();

		if (ch == KEY_MOUSE) {
			handle_mouse(&state);
			continue;
		}

		switch (state.mode) {
			case MODE_NORMAL:
				handle_normal_mode(&state, ch);
				break;
			case MODE_COMMAND:
				handle_command_mode(&state, ch);
				break;
			case MODE_FINDER:
				handle_finder_mode(&state, ch);
				break;
			case MODE_INSERT:
				handle_insert_mode(&state, ch);
				break;
		}
	}

	if (state.items) free(state.items);
	if (state.finder_matches) free(state.finder_matches);
	free_content(&state);
	endwin();
}
