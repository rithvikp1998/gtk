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


static void
cpdb_printer_get_settings_from_options (GtkPrinter *printer,
                                        GtkPrinterOptionSet *options,
                                        GtkPrintSettings *settings)
{
    GtkPrinterOption *option;

    option = gtk_printer_option_set_lookup(options, "gtk-n-up");
    if (option)
      gtk_print_settings_set ( settings, GTK_PRINT_SETTINGS_NUMBER_UP, option->value );

    option = gtk_printer_option_set_lookup( options, "gtk-n-up-layout");
    if (option)
      gtk_print_settings_set( settings, GTK_PRINT_SETTINGS_NUMBER_UP_LAYOUT, option->value);

    
}


static GtkPrinterOptionSet *cpdb_printer_get_options (GtkPrinter *printer,
                                                      GtkPrintSettings *settings,
                                                      GtkPageSetup *page_setup,
                                                      GtkPrintCapabilities capabilities)
{
  GtkPrinterOption *option;
  GtkPrinterOptionSet *set = gtk_printer_option_set_new();

  // CPDB Option CPD_OPTION_NUMBER_UP
  const char *n_up[] = {"1", "2", "4", "6", "9", "16" };
  option = gtk_printer_option_new ("gtk-n-up", C_("printer option", "Pages per Sheet"), GTK_PRINTER_OPTION_TYPE_PICKONE);
  gtk_printer_option_choices_from_array (option, G_N_ELEMENTS (n_up), n_up, n_up);
  gtk_printer_option_set (option, "1");
  gtk_printer_option_set_add (set, option);

  // CPDB Option CPD_OPTION_JOB_PRIORITY
  const char *priority[] = {"100", "80", "50", "30" };
  const char *priority_display[] = {N_("Urgent"), N_("High"), N_("Medium"), N_("Low") };
  option = gtk_printer_option_new ("gtk-job-prio", _("Job Priority"), GTK_PRINTER_OPTION_TYPE_PICKONE);
  gtk_printer_option_choices_from_array (option, G_N_ELEMENTS (priority), priority, priority_display);
  gtk_printer_option_set (option, "50");
  gtk_printer_option_set_add (set, option);
  g_object_unref (option);

  // CPDB Option CPD_OPTION_SIDES
  const char *sides[] = {"one-sided", "two-sided-short", "two-sided-long"};
  const char *sides_display[] = {N_("One Sided"), N_("Long Edged (Standard)"), N_("Short Edged (Flip)")};
  option = gtk_printer_option_new ("gtk-duplex", _("Duplex Printing"), GTK_PRINTER_OPTION_TYPE_PICKONE);
  gtk_printer_option_choices_from_array (option, G_N_ELEMENTS (sides), sides, sides_display);
  gtk_printer_option_set (option, "one-sided");
  gtk_printer_option_set_add (set, option);
  g_object_unref (option);

  return set;
}

static void cpdb_printer_prepare_for_print (GtkPrinter *printer,
                                            GtkPrintJob *print_job,
                                            GtkPrintSettings *settings,
                                            GtkPageSetup *page_setup)
{
  double scale;
  GtkPrintPages pages;
  GtkPageRange *ranges;
  GtkPageSet page_set;
  GtkPaperSize *paper_size;
  const char *ppd_paper_name;
  int n_ranges;


  pages = gtk_print_settings_get_print_pages (settings);
  gtk_print_job_set_pages (print_job, pages);
  if ( pages == GTK_PRINT_PAGES_RANGES )
    ranges = gtk_print_settings_get_page_ranges ( settings, &n_ranges );
  else
  {
    ranges = NULL;
    n_ranges = 0;
  }
  gtk_print_job_set_page_ranges (print_job, ranges, n_ranges);
  
  gtk_print_job_set_collate (print_job, gtk_print_settings_get_collate (settings));
  gtk_print_job_set_reverse (print_job, gtk_print_settings_get_reverse (settings));
  gtk_print_job_set_num_copies (print_job, gtk_print_settings_get_n_copies (settings));
  gtk_print_job_set_n_up (print_job, gtk_print_settings_get_number_up (settings));
  gtk_print_job_set_n_up_layout (print_job, gtk_print_settings_get_number_up_layout (settings));
  gtk_print_job_set_rotate (print_job, TRUE);

  scale = gtk_print_settings_get_scale (settings);
  if (scale != 100.0)
    gtk_print_job_set_scale (print_job, scale / 100.0);

  page_set = gtk_print_settings_get_page_set (settings);
  if (page_set == GTK_PAGE_SET_EVEN)
    gtk_print_settings_set (settings, "cpdb-page-set", "even");
  else if (page_set == GTK_PAGE_SET_ODD)
    gtk_print_settings_set (settings, "cpdb-page-set", "odd");
  gtk_print_job_set_page_set (print_job, GTK_PAGE_SET_ALL);

  paper_size = gtk_page_setup_get_paper_size (page_setup);
  ppd_paper_name = gtk_paper_size_get_ppd_name (paper_size);

  if (ppd_paper_name != NULL)
    gtk_print_settings_set (settings, "cpdb-PageSize", ppd_paper_name);
  else if (gtk_paper_size_is_ipp (paper_size))
    gtk_print_settings_set (settings, "cpdb-media", gtk_paper_size_get_name (paper_size));
  else
    {
      char width[G_ASCII_DTOSTR_BUF_SIZE];
      char height[G_ASCII_DTOSTR_BUF_SIZE];
      char *custom_name;

      g_ascii_formatd (width, sizeof (width), "%.2f", gtk_paper_size_get_width (paper_size, GTK_UNIT_POINTS));
      g_ascii_formatd (height, sizeof (height), "%.2f", gtk_paper_size_get_height (paper_size, GTK_UNIT_POINTS));
      custom_name = g_strdup_printf (("Custom.%sx%s"), width, height);
      gtk_print_settings_set (settings, "cpdb-PageSize", custom_name);
      g_free (custom_name);
    }

  if (gtk_print_settings_get_number_up (settings) > 1)
    {
      GtkNumberUpLayout  layout = gtk_print_settings_get_number_up_layout (settings);
      GEnumClass        *enum_class;
      GEnumValue        *enum_value;

      switch (gtk_page_setup_get_orientation (page_setup))
        {
          case GTK_PAGE_ORIENTATION_LANDSCAPE:
            if (layout < 4)
              layout = layout + 2 + 4 * (1 - layout / 2);
            else
              layout = layout - 3 - 2 * (layout % 2);
            break;
          case GTK_PAGE_ORIENTATION_REVERSE_PORTRAIT:
            layout = (layout + 3 - 2 * (layout % 2)) % 4 + 4 * (layout / 4);
            break;
          case GTK_PAGE_ORIENTATION_REVERSE_LANDSCAPE:
            if (layout < 4)
              layout = layout + 5 - 2 * (layout % 2);
            else
              layout = layout - 6 + 4 * (1 - (layout - 4) / 2);
            break;

          case GTK_PAGE_ORIENTATION_PORTRAIT:
          default:
            break;
        }

      enum_class = g_type_class_ref (GTK_TYPE_NUMBER_UP_LAYOUT);
      enum_value = g_enum_get_value (enum_class, layout);
      gtk_print_settings_set (settings, "cpdb-number-up-layout", enum_value->value_nick);
      g_type_class_unref (enum_class);
    }
  
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
