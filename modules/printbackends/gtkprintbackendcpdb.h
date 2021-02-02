#ifndef __GTK_PRINT_BACKEND_CPDB_H__
#define __GTK_PRINT_BACKEND_CPDB_H__

#include <glib-object.h>
#include "gtkprintbackendprivate.h"

G_BEGIN_DECLS

#define GTK_TYPE_PRINT_BACKEND_CPDB            (gtk_print_backend_cpdb_get_type ())
#define GTK_PRINT_BACKEND_CPDB(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_PRINT_BACKEND_CPDB, GtkPrintBackendCpdb))
#define GTK_IS_PRINT_BACKEND_CPDB(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_PRINT_BACKEND_CPDB))

typedef struct _GtkPrintBackendCpdb      GtkPrintBackendCpdb;

GtkPrintBackend *gtk_print_backend_cpdb_new      (void);
GType          gtk_print_backend_cpdb_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif