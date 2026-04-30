#ifndef __GUI_HPP_
#define __GUI_HPP_

#include <SQLiteCpp/Database.h>
#include <gtk/gtk.h>

namespace gui {

int run(int argc, char **argv, SQLite::Database &db);

}

#endif
