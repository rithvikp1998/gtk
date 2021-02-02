#include "config.h"

#include <glib/gi18n-lib.h>

#include <gtk/gtk.h>
#include "gtkprinterprivate.h"

#include "gtkprintbackendcpdb.h"

typedef struct _GtkPrintBackendCpdbClass GtkPrintBackendCpdbClass;

#define GTK_PRINT_BACKEND_CPDB_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_PRINT_BACKEND_CPDB, GtkPrintBackendCpdbClass))
#define GTK_IS_PRINT_BACKEND_CPDB_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_PRINT_BACKEND_CPDB))
#define GTK_PRINT_BACKEND_CPDB_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_PRINT_BACKEND_CPDB, GtkPrintBackendCpdbClass))

struct _GtkPrintBackendCpdbClass
{
  GtkPrintBackendClass parent_class;
};

struct _GtkPrintBackendCpdb
{
  GtkPrintBackend parent_instance;
};

static GObjectClass *backend_parent_class;

G_DEFINE_DYNAMIC_TYPE (GtkPrintBackendCpdb, gtk_print_backend_cpdb, GTK_TYPE_PRINT_BACKEND)

void
g_io_module_load (GIOModule *module)
{
  g_type_module_use (G_TYPE_MODULE (module));

  gtk_print_backend_cpdb_register_type (G_TYPE_MODULE (module));

  g_io_extension_point_implement (GTK_PRINT_BACKEND_EXTENSION_POINT_NAME,
                                  GTK_TYPE_PRINT_BACKEND_CPDB,
                                  "cpdb",
                                  10);
}

void
g_io_module_unload (GIOModule *module)
{
}

char **
g_io_module_query (void)
{
  char *eps[] = {
    GTK_PRINT_BACKEND_EXTENSION_POINT_NAME,
    NULL
  };

  return g_strdupv (eps);
}

GtkPrintBackend *
gtk_print_backend_cpdb_new (void)
{
  return g_object_new (GTK_TYPE_PRINT_BACKEND_CPDB, NULL);
}

static void
gtk_print_backend_cpdb_class_init (GtkPrintBackendCpdbClass *class)
{
  GtkPrintBackendClass *backend_class = GTK_PRINT_BACKEND_CLASS (class);
  
  backend_parent_class = g_type_class_peek_parent (class);
}

static void
gtk_print_backend_cpdb_class_finalize (GtkPrintBackendCpdbClass *class)
{
}

static void
gtk_print_backend_cpdb_init (GtkPrintBackendCpdb *backend)
{
  GtkPrinter *printer;

  printer = g_object_new (GTK_TYPE_PRINTER,
			  "name", _("Print to CPDB"),
			  "backend", backend,
			  "is-virtual", FALSE,
			  "accepts-pdf", TRUE,
			  "accepts-ps", TRUE,
			  NULL);
  gtk_printer_set_has_details (printer, TRUE);
  gtk_printer_set_icon_name (printer, "printer");
  gtk_printer_set_is_active (printer, TRUE);
  gtk_printer_set_is_default (printer, TRUE);

  gtk_print_backend_add_printer (GTK_PRINT_BACKEND (backend), printer);
  g_object_unref (printer);
  gtk_print_backend_set_list_done (GTK_PRINT_BACKEND (backend));
}
