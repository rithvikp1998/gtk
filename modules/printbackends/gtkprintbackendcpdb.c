#include "config.h"
#include "gtkprinterprivate.h"
#include "gtkprintbackendcpdb.h"

#include <glib/gi18n-lib.h>
#include <cpdb-libs-frontend.h>

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
      (char *) GTK_PRINT_BACKEND_EXTENSION_POINT_NAME,
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

  backend_class->request_printer_list = cpdb_get_printer_list;
  backend_class->printer_create_cairo_surface = cpdb_printer_create_cairo_surface;
  backend_class->printer_get_options = cpdb_printer_get_options;
  backend_class->printer_get_settings_from_options = cpdb_printer_get_settings_from_options;
  backend_class->printer_prepare_for_print = cpdb_printer_prepare_for_print;
}

static void
gtk_print_backend_cpdb_class_finalize (GtkPrintBackendCpdbClass *class)
{
}

static void
gtk_print_backend_cpdb_init (GtkPrintBackendCpdb *backend)
{
  gtkPrintBackend = GTK_PRINT_BACKEND (backend);
  frontendObj = get_new_FrontendObj(NULL, add_printer_callback, remove_printer_callback);
  connect_to_dbus(frontendObj);
}

static void
cpdb_get_printer_list(GtkPrintBackend *backend)
{
  gtkPrintBackend = GTK_PRINT_BACKEND (backend);
  refresh_printer_list(frontendObj);
}

static void cpdb_printer_get_settings_from_options (GtkPrinter *printer,
                                                    GtkPrinterOptionSet *options,
                                                    GtkPrintSettings *settings)
{

}

static GtkPrinterOptionSet *cpdb_printer_get_options (GtkPrinter *printer,
                                                      GtkPrintSettings *settings,
                                                      GtkPageSetup *page_setup,
                                                      GtkPrintCapabilities capabilities)
{
  return gtk_printer_option_set_new();
}

static void cpdb_printer_prepare_for_print (GtkPrinter *printer,
                                            GtkPrintJob *print_job,
                                            GtkPrintSettings *settings,
                                            GtkPageSetup *page_setup)
{
}

static cairo_surface_t * cpdb_printer_create_cairo_surface (GtkPrinter *printer,
                                                            GtkPrintSettings *settings,
                                                            double width,
                                                            double height,
                                                            GIOChannel *cache_io)
{
  cairo_surface_t *surface;
  return surface;
}

static void gtk_print_backend_cpdb_print_stream (GtkPrintBackend *print_backend,
                                                 GtkPrintJob *job,
                                                 GIOChannel *data_io,
                                                 GtkPrintJobCompleteFunc callback,
                                                 gpointer user_data,
                                                 GDestroyNotify dnotify)
{
}

static GtkPrinter *get_gtk_printer_from_printer_obj(PrinterObj *p) {
  GtkPrinter *printer;

  printer = g_object_new (GTK_TYPE_PRINTER,
                          "name", p->name,
                          "backend", GTK_PRINT_BACKEND_CPDB (gtkPrintBackend));

  gtk_printer_set_icon_name (printer, "printer");
  gtk_printer_set_state_message(printer, p->state);
  gtk_printer_set_location(printer, p->location);
  gtk_printer_set_description(printer, p->info);
  gtk_printer_set_is_accepting_jobs(printer, p->is_accepting_jobs);
  gtk_printer_set_job_count(printer, get_active_jobs_count(p));

  gtk_printer_set_has_details (printer, TRUE);
  gtk_printer_set_is_active (printer, TRUE);
  // Given GCP is going to be deprecated, the CUPS default printer wil be overall default.
  if (strcmp(p->backend_name, "CUPS") == 0 &&
      strcmp(get_default_printer(frontendObj, p->backend_name), p->name) == 0)
  {
    gtk_printer_set_is_default (printer, TRUE);
  }
  return printer;
}

static int add_printer_callback(PrinterObj *p)
{
  GtkPrinter *printer = get_gtk_printer_from_printer_obj(p);
  gtk_print_backend_add_printer (gtkPrintBackend, printer);
  g_object_unref (printer);
  gtk_print_backend_set_list_done (gtkPrintBackend);

  return 0;
}

static int remove_printer_callback(PrinterObj *p)
{
  g_message("Removed Printer %s : %s!\n", p->name, p->backend_name);
  return 0;
}
