#ifndef __GTK_PRINT_BACKEND_CPDB_H__
#define __GTK_PRINT_BACKEND_CPDB_H__

#include <glib-object.h>
#include "gtkprintbackendprivate.h"
#include <cpdb-libs-frontend.h>

G_BEGIN_DECLS

#define GTK_TYPE_PRINT_BACKEND_CPDB            (gtk_print_backend_cpdb_get_type ())
#define GTK_PRINT_BACKEND_CPDB(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_PRINT_BACKEND_CPDB, GtkPrintBackendCpdb))
#define GTK_IS_PRINT_BACKEND_CPDB(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_PRINT_BACKEND_CPDB))

typedef struct _GtkPrintBackendCpdb      GtkPrintBackendCpdb;

GtkPrintBackend *gtk_print_backend_cpdb_new      (void);
GType          gtk_print_backend_cpdb_get_type (void) G_GNUC_CONST;

static void cpdb_get_printer_list(GtkPrintBackend *backend);

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

static FrontendObj *frontendObj;
static int add_printer_callback(PrinterObj *p);
static int remove_printer_callback(PrinterObj *p);

static GtkPrintBackend *gtkPrintBackend;

static void                 cpdb_printer_get_settings_from_options (GtkPrinter              *printer,
                                                                   GtkPrinterOptionSet     *options,
                                                                   GtkPrintSettings        *settings);
static GtkPrinterOptionSet *cpdb_printer_get_options               (GtkPrinter              *printer,
                                                                   GtkPrintSettings        *settings,
                                                                   GtkPageSetup            *page_setup,
                                                                   GtkPrintCapabilities     capabilities);
static void                 cpdb_printer_prepare_for_print         (GtkPrinter              *printer,
                                                                   GtkPrintJob             *print_job,
                                                                   GtkPrintSettings        *settings,
                                                                   GtkPageSetup            *page_setup);
static cairo_surface_t *    cpdb_printer_create_cairo_surface      (GtkPrinter              *printer,
                                                                   GtkPrintSettings        *settings,
                                                                   double                   width,
                                                                   double                   height,
                                                                   GIOChannel              *cache_io);
static void                 gtk_print_backend_cpdb_print_stream    (GtkPrintBackend         *print_backend,
                                                                   GtkPrintJob             *job,
                                                                   GIOChannel              *data_io,
                                                                   GtkPrintJobCompleteFunc  callback,
                                                                   gpointer                 user_data,
                                                                   GDestroyNotify           dnotify);

G_END_DECLS

#endif