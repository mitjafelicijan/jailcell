#ifndef INTERFACE_H
#define INTERFACE_H

#include "sqlite3.h"

void interface_start(sqlite3 *db, const char *cell_name);

#endif
