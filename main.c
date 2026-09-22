#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <getopt.h>
#include "cell.h"
#include "interface.h"

void get_password(const char *prompt, char *password, size_t size) {
	struct termios oldt, newt;
	printf("%s", prompt);
	fflush(stdout);

	tcgetattr(STDIN_FILENO, &oldt);
	newt = oldt;
	newt.c_lflag &= ~(ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, &newt);

	if (fgets(password, size, stdin) != NULL) {
		password[strcspn(password, "\n")] = 0;
	}

	tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
	printf("\n");
}

void usage(const char *progname) {
	fprintf(stderr, "Usage: %s [options] <cell_name>\n", progname);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -n, --new           Create new cell\n");
	fprintf(stderr, "  -a, --add           Add note (requires -t and -c)\n");
	fprintf(stderr, "  -l, --list          List all notes\n");
	fprintf(stderr, "  -s, --show <id>     Show note content\n");
	fprintf(stderr, "  -t, --title <t>     Title for new note\n");
	fprintf(stderr, "  -c, --content <c>   Content for new note\n");
	fprintf(stderr, "  -F, --file <path>   Add file to cell\n");
	fprintf(stderr, "  -f, --format <fmt>  Output format (text, json, xml)\n");
	fprintf(stderr, "  -x, --export <id>   Export file from cell (requires -o)\n");
	fprintf(stderr, "  -o, --out <path>    Output path for export\n");
	fprintf(stderr, "  -v, --version       Show version information\n");
	fprintf(stderr, "  -h, --help          Show this help message\n");
	fprintf(stderr, "\nIf no action is specified, starts TUI.\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
	int opt;
	char *cell_name = NULL;
	char *title = NULL;
	char *content = NULL;
	char *file_path = NULL;
	char *out_path = NULL;
	char *format = "text";
	long export_id = -1;
	long show_id = -1;
	int mode = 0; // 0 for UI, 1 for new, 2 for add, 3 for list, 4 for add file, 5 for export, 6 for show
	int mode_count = 0;

	static struct option long_options[] = {
		{"new",     no_argument,       0, 'n'},
		{"add",     no_argument,       0, 'a'},
		{"list",    no_argument,       0, 'l'},
		{"show",    required_argument, 0, 's'},
		{"title",   required_argument, 0, 't'},
		{"content", required_argument, 0, 'c'},
		{"file",    required_argument, 0, 'F'},
		{"format",  required_argument, 0, 'f'},
		{"export",  required_argument, 0, 'x'},
		{"out",     required_argument, 0, 'o'},
		{"version", no_argument,       0, 'v'},
		{"help",    no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "anls:t:c:F:f:x:o:vh", long_options, NULL)) != -1) {
		switch (opt) {
			case 'n':
				mode = 1;
				mode_count++;
				break;
			case 'a':
				mode = 2;
				mode_count++;
				break;
			case 'l':
				mode = 3;
				mode_count++;
				break;
			case 's': {
				char *endptr;
				show_id = strtol(optarg, &endptr, 10);
				if (*endptr != '\0' || show_id < 0) {
					fprintf(stderr, "Error: Invalid note ID '%s'\n", optarg);
					exit(EXIT_FAILURE);
				}
				mode = 6;
				mode_count++;
				break;
			}
			case 't':
				title = optarg;
				break;
			case 'c':
				content = optarg;
				break;
			case 'F':
				file_path = optarg;
				mode = 4;
				mode_count++;
				break;
			case 'f':
				format = optarg;
				break;
			case 'x': {
				char *endptr;
				export_id = strtol(optarg, &endptr, 10);
				if (*endptr != '\0' || export_id < 0) {
					fprintf(stderr, "Error: Invalid export ID '%s'\n", optarg);
					exit(EXIT_FAILURE);
				}
				mode = 5;
				mode_count++;
				break;
			}
			case 'o':
				out_path = optarg;
				break;
			case 'v':
				printf("jailcell version %s\n", JAILCELL_VERSION);
				exit(EXIT_SUCCESS);
			case 'h':
				usage(argv[0]);
				break;
			default:
				usage(argv[0]);
		}
	}

	if (optind < argc) {
		cell_name = argv[optind];
	}

	if (!cell_name) {
		usage(argv[0]);
	}

	if (mode_count > 1) {
		fprintf(stderr, "Error: Multiple actions specified. Choose only one.\n");
		usage(argv[0]);
	}

	// Auto-select add mode if title/content provided without -a
	if (mode == 0 && title && content) {
		mode = 2;
	}

	// Validation
	if (mode == 2 && (!title || !content)) {
		fprintf(stderr, "Error: Adding a note requires both --title and --content\n");
		usage(argv[0]);
	}

	if (mode == 5 && (!out_path)) {
		fprintf(stderr, "Error: Exporting a file requires --out path\n");
		usage(argv[0]);
	}

	char password[256];
	get_password("Enter master password: ", password, sizeof(password));

	sqlite3 *db = NULL;
	int rc = cell_open(cell_name, password, &db);

	// Clear password from memory
	memset(password, 0, sizeof(password));

	if (rc != SQLITE_OK) {
		if (rc == SQLITE_NOTADB) {
			fprintf(stderr, "Password incorrect\n");
		} else {
			fprintf(stderr, "Error opening database: %s\n", sqlite3_errstr(rc));
		}
		return 1;
	}

	// Always ensure schema is initialized
	rc = cell_init(db);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to initialize cell: %s\n", sqlite3_errstr(rc));
		sqlite3_close(db);
		return 1;
	}

	if (mode == 1) {
		printf("Cell '%s' created and initialized successfully.\n", cell_name);
	} else if (mode == 2) {
		rc = cell_add_note(db, title, content);
		if (rc == SQLITE_OK) {
			printf("Note added to '%s'.\n", cell_name);
		} else {
			fprintf(stderr, "Failed to add note: %s\n", sqlite3_errstr(rc));
		}
	} else if (mode == 3) {
		if (strcmp(format, "json") == 0) {
			rc = cell_list_notes_json(db);
		} else if (strcmp(format, "xml") == 0) {
			rc = cell_list_notes_xml(db);
		} else {
			rc = cell_list_notes(db);
		}
		if (rc != SQLITE_OK) {
			fprintf(stderr, "Failed to list notes: %s\n", sqlite3_errstr(rc));
		}
	} else if (mode == 4) {
		rc = cell_add_file(db, file_path);
		if (rc == SQLITE_OK) {
			printf("File '%s' added to cell.\n", file_path);
		} else {
			fprintf(stderr, "Failed to add file: %s\n", sqlite3_errstr(rc));
		}
	} else if (mode == 5) {
		rc = cell_export_file(db, export_id, out_path);
		if (rc == SQLITE_OK) {
			printf("File exported to '%s'.\n", out_path);
		} else {
			fprintf(stderr, "Failed to export file: %s\n", sqlite3_errstr(rc));
		}
	} else if (mode == 6) {
		if (strcmp(format, "json") == 0) {
			rc = cell_get_note_json(db, show_id);
			if (rc == SQLITE_NOTFOUND) {
				fprintf(stderr, "Item with ID %ld not found in notes or files.\n", show_id);
			} else if (rc != SQLITE_OK) {
				fprintf(stderr, "Failed to get item: %s\n", sqlite3_errstr(rc));
			}
		} else if (strcmp(format, "xml") == 0) {
			rc = cell_get_note_xml(db, show_id);
			if (rc == SQLITE_NOTFOUND) {
				fprintf(stderr, "Item with ID %ld not found in notes or files.\n", show_id);
			} else if (rc != SQLITE_OK) {
				fprintf(stderr, "Failed to get item: %s\n", sqlite3_errstr(rc));
			}
		} else {
			char *note_content = NULL;
			rc = cell_get_note_content(db, show_id, &note_content);
			if (rc == SQLITE_OK) {
				printf("%s\n", note_content);
				free(note_content);
			} else if (rc == SQLITE_NOTFOUND) {
				// Check if it's a file
				int size;
				char *mime_type = NULL;
				rc = cell_get_file_info(db, show_id, &size, &mime_type);
				if (rc == SQLITE_OK) {
					printf("Item is a File.\n");
					printf("Size: %d bytes\n", size);
					printf("MIME Type: %s\n", mime_type);
					printf("Use --export -x %ld -o <path> to extract the file.\n", show_id);
					free(mime_type);
				} else {
					fprintf(stderr, "Item with ID %ld not found in notes or files.\n", show_id);
				}
			} else {
				fprintf(stderr, "Failed to get item: %s\n", sqlite3_errstr(rc));
			}
		}
	} else if (mode == 0) {
		interface_start(db, cell_name);
	}

	sqlite3_close(db);
	return 0;
}
