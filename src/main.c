/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#if defined (HAVE_MALLINFO) || defined (HAVE_MALLINFO2)
#include <malloc.h>
#endif
#include <stdlib.h>
#include <string.h>

#include <cogl-pango/cogl-pango.h>
#include <clutter/clutter.h>
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <glib/gi18n-lib.h>
#include <girepository.h>
#include <meta/meta-context.h>
#include <meta/meta-plugin.h>
#include <meta/prefs.h>
#include <atk-bridge.h>

#include "shell-global.h"
#include "shell-global-private.h"
#include "shell-perf-log.h"
#include "st.h"

extern GType gnome_shell_plugin_get_type (void);

#define SHELL_DBUS_SERVICE "org.gnome.Shell"

#define WM_NAME "GNOME Shell"
#define GNOME_WM_KEYBINDINGS "Mutter,GNOME Shell"

static gboolean is_gdm_mode = FALSE;
static char *session_mode = NULL;
static int caught_signal = 0;

#define DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER 1
#define DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER 4

enum {
  SHELL_DEBUG_BACKTRACE_WARNINGS = 1,
  SHELL_DEBUG_BACKTRACE_SEGFAULTS = 2,
};
static int _shell_debug;
static gboolean _tracked_signals[NSIG] = { 0 };

static void
shell_dbus_acquire_name (GDBusProxy  *bus,
                         guint32      request_name_flags,
                         guint32     *request_name_result,
                         const gchar *name,
                         gboolean     fatal)
{
  GError *error = NULL;
  GVariant *request_name_variant;

  if (!(request_name_variant = g_dbus_proxy_call_sync (bus,
                                                       "RequestName",
                                                       g_variant_new ("(su)", name, request_name_flags),
                                                       0, /* call flags */
                                                       -1, /* timeout */
                                                       NULL, /* cancellable */
                                                       &error)))
    {
      g_printerr ("failed to acquire %s: %s\n", name, error->message);
      g_clear_error (&error);
      if (!fatal)
        return;
      exit (1);
    }
  g_variant_get (request_name_variant, "(u)", request_name_result);
  g_variant_unref (request_name_variant);
}

static void
shell_dbus_init (gboolean replace)
{
  GDBusConnection *session;
  GDBusProxy *bus;
  GError *error = NULL;
  guint32 request_name_flags;
  guint32 request_name_result;

  session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);

  if (error) {
    g_printerr ("Failed to connect to session bus: %s", error->message);
    exit (1);
  }

  bus = g_dbus_proxy_new_sync (session,
                               G_DBUS_PROXY_FLAGS_NONE,
                               NULL, /* interface info */
                               "org.freedesktop.DBus",
                               "/org/freedesktop/DBus",
                               "org.freedesktop.DBus",
                               NULL, /* cancellable */
                               &error);

  if (!bus)
    {
      g_printerr ("Failed to get a session bus proxy: %s", error->message);
      exit (1);
    }

  request_name_flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    request_name_flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  shell_dbus_acquire_name (bus,
                           request_name_flags,
                           &request_name_result,
                           SHELL_DBUS_SERVICE, TRUE);
  if (!(request_name_result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
        || request_name_result == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER))
    {
      g_printerr (SHELL_DBUS_SERVICE " already exists on bus and --replace not specified\n");
      exit (1);
    }

  g_object_unref (bus);
  g_object_unref (session);
}

static void
shell_introspection_init (void)
{

  g_irepository_prepend_search_path (MUTTER_TYPELIB_DIR);
  g_irepository_prepend_search_path (GNOME_SHELL_PKGLIBDIR);

  /* We need to explicitly add the directories where the private libraries are
   * installed to the GIR's library path, so that they can be found at runtime
   * when linking using DT_RUNPATH (instead of DT_RPATH), which is the default
   * for some linkers (e.g. gold) and in some distros (e.g. Debian).
   */
  g_irepository_prepend_library_path (MUTTER_TYPELIB_DIR);
  g_irepository_prepend_library_path (GNOME_SHELL_PKGLIBDIR);
}

static void
shell_fonts_init (void)
{
  CoglPangoFontMap *fontmap;

  /* Disable text mipmapping; it causes problems on pre-GEM Intel
   * drivers and we should just be rendering text at the right
   * size rather than scaling it. If we do effects where we dynamically
   * zoom labels, then we might want to reconsider.
   */
  fontmap = COGL_PANGO_FONT_MAP (clutter_get_font_map ());
  cogl_pango_font_map_set_use_mipmapping (fontmap, FALSE);
}

static void
shell_profiler_init (void)
{
  ShellGlobal *global;
  GjsProfiler *profiler;
  GjsContext *context;
  const char *enabled;
  const char *fd_str;
  int fd = -1;

  /* Sysprof uses the "GJS_TRACE_FD=N" environment variable to connect GJS
   * profiler data to the combined Sysprof capture. Since we are in control of
   * the GjsContext, we need to proxy this FD across to the GJS profiler.
   */

  fd_str = g_getenv ("GJS_TRACE_FD");
  enabled = g_getenv ("GJS_ENABLE_PROFILER");
  if (fd_str == NULL || enabled == NULL)
    return;

  global = shell_global_get ();
  g_return_if_fail (global);

  context = _shell_global_get_gjs_context (global);
  g_return_if_fail (context);

  profiler = gjs_context_get_profiler (context);
  g_return_if_fail (profiler);

  if (fd_str)
    {
      fd = atoi (fd_str);

      if (fd > 2)
        {
          gjs_profiler_set_fd (profiler, fd);
          gjs_profiler_start (profiler);
        }
    }
}

static void
shell_profiler_shutdown (void)
{
  ShellGlobal *global;
  GjsProfiler *profiler;
  GjsContext *context;

  global = shell_global_get ();
  context = _shell_global_get_gjs_context (global);
  profiler = gjs_context_get_profiler (context);

  if (profiler)
    gjs_profiler_stop (profiler);
}

static void
malloc_statistics_callback (ShellPerfLog *perf_log,
                            gpointer      data)
{
#if defined (HAVE_MALLINFO) || defined (HAVE_MALLINFO2)
#ifdef HAVE_MALLINFO2
  struct mallinfo2 info = mallinfo2 ();
#else
  struct mallinfo info = mallinfo ();
#endif

  shell_perf_log_update_statistic_i (perf_log,
                                     "malloc.arenaSize",
                                     info.arena);
  shell_perf_log_update_statistic_i (perf_log,
                                     "malloc.mmapSize",
                                     info.hblkhd);
  shell_perf_log_update_statistic_i (perf_log,
                                     "malloc.usedSize",
                                     info.uordblks);
#endif /* defined (HAVE_MALLINFO) || defined (HAVE_MALLINFO2) */
}

static void
shell_perf_log_init (void)
{
  ShellPerfLog *perf_log = shell_perf_log_get_default ();

  /* For probably historical reasons, mallinfo() defines the returned values,
   * even those in bytes as int, not size_t. We're determined not to use
   * more than 2G of malloc'ed memory, so are OK with that.
   */
  shell_perf_log_define_statistic (perf_log,
                                   "malloc.arenaSize",
                                   "Amount of memory allocated by malloc() with brk(), in bytes",
                                   "i");
  shell_perf_log_define_statistic (perf_log,
                                   "malloc.mmapSize",
                                   "Amount of memory allocated by malloc() with mmap(), in bytes",
                                   "i");
  shell_perf_log_define_statistic (perf_log,
                                   "malloc.usedSize",
                                   "Amount of malloc'ed memory currently in use",
                                   "i");

  shell_perf_log_add_statistics_callback (perf_log,
                                          malloc_statistics_callback,
                                          NULL, NULL);
}

static void
shell_a11y_init (void)
{
  cally_accessibility_init ();

  if (clutter_get_accessibility_enabled () == FALSE)
    {
      g_warning ("Accessibility: clutter has no accessibility enabled"
                 " skipping the atk-bridge load");
    }
  else
    {
      atk_bridge_adaptor_init (NULL, NULL);
    }
}

static void
shell_init_debug (const char *debug_env)
{
  static const GDebugKey keys[] = {
    { "backtrace-warnings", SHELL_DEBUG_BACKTRACE_WARNINGS },
    { "backtrace-segfaults", SHELL_DEBUG_BACKTRACE_SEGFAULTS },
  };

  _shell_debug = g_parse_debug_string (debug_env, keys,
                                       G_N_ELEMENTS (keys));
}

static GLogWriterOutput
default_log_writer (GLogLevelFlags   log_level,
                    const GLogField *fields,
                    gsize            n_fields,
                    gpointer         user_data)
{
  GLogWriterOutput output;
  int i;

  output = g_log_writer_default (log_level, fields, n_fields, user_data);

  if ((_shell_debug & SHELL_DEBUG_BACKTRACE_WARNINGS) &&
      ((log_level & G_LOG_LEVEL_CRITICAL) ||
       (log_level & G_LOG_LEVEL_WARNING)))
    {
      const char *log_domain = NULL;

      for (i = 0; i < n_fields; i++)
        {
          if (g_strcmp0 (fields[i].key, "GLIB_DOMAIN") == 0)
            {
              log_domain = fields[i].value;
              break;
            }
        }

      /* Filter out Gjs logs, those already have the stack */
      if (g_strcmp0 (log_domain, "Gjs") != 0)
        gjs_dumpstack ();
    }

  return output;
}

static GLogWriterOutput
shut_up (GLogLevelFlags   log_level,
         const GLogField *fields,
         gsize            n_fields,
         gpointer         user_data)
{
  return (GLogWriterOutput) {0};
}

static void
dump_gjs_stack_alarm_sigaction (int signo)
{
  g_log_set_writer_func (g_log_writer_default, NULL, NULL);
  g_warning ("Failed to dump Javascript stack, got stuck");
  g_log_set_writer_func (default_log_writer, NULL, NULL);

  raise (caught_signal);
}

static void
dump_gjs_stack_on_signal_handler (int signo)
{
  struct sigaction sa = { .sa_handler = dump_gjs_stack_alarm_sigaction };
  gsize i;

  /* Ignore all the signals starting this point, a part the one we'll raise
   * (which is implicitly ignored here through SA_RESETHAND), this is needed
   * not to get this handler being called by other signals that we were
   * tracking and that might be emitted by code called starting from now.
   */
  for (i = 0; i < G_N_ELEMENTS (_tracked_signals); ++i)
    {
      if (_tracked_signals[i] && i != signo)
        signal (i, SIG_IGN);
    }

  /* Waiting at least 5 seconds for the dumpstack, if it fails, we raise the error */
  caught_signal = signo;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGALRM, &sa, NULL);

  alarm (5);
  gjs_dumpstack ();
  alarm (0);

  raise (signo);
}

static void
dump_gjs_stack_on_signal (int signo)
{
  struct sigaction sa = {
    .sa_flags   = SA_RESETHAND | SA_NODEFER,
    .sa_handler = dump_gjs_stack_on_signal_handler,
  };

  sigemptyset (&sa.sa_mask);

  sigaction (signo, &sa, NULL);
  _tracked_signals[signo] = TRUE;
}

static gboolean
list_modes (const char  *option_name,
            const char  *value,
            gpointer     data,
            GError     **error)
{
  ShellGlobal *global;
  GjsContext *context;
  const char *script;
  int status;

  /* Many of our imports require global to be set, so rather than
   * tayloring our imports carefully here to avoid that dependency,
   * we just set it.
   * ShellGlobal has some GTK+ dependencies, so initialize GTK+; we
   * don't really care if it fails though (e.g. when running from a tty),
   * so we mute all warnings */
  g_log_set_writer_func (shut_up, NULL, NULL);
  gtk_init_check (NULL, NULL);

  _shell_global_init (NULL);
  global = shell_global_get ();
  context = _shell_global_get_gjs_context (global);

  shell_introspection_init ();

  script = "imports.ui.environment.init();"
           "imports.ui.sessionMode.listModes();";
  if (!gjs_context_eval (context, script, -1, "<main>", &status, NULL))
      g_message ("Retrieving list of available modes failed.");

  g_object_unref (context);
  exit (status);
}

static gboolean
print_version (const gchar    *option_name,
               const gchar    *value,
               gpointer        data,
               GError        **error)
{
  g_print ("GNOME Shell %s\n", VERSION);
  exit (0);
}

GOptionEntry gnome_shell_options[] = {
  {
    "version", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
    print_version,
    N_("Print version"),
    NULL
  },
  {
    "gdm-mode", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE,
    &is_gdm_mode,
    N_("Mode used by GDM for login screen"),
    NULL
  },
  {
    "mode", 0, 0, G_OPTION_ARG_STRING,
    &session_mode,
    N_("Use a specific mode, e.g. “gdm” for login screen"),
    "MODE"
  },
  {
    "list-modes", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
    list_modes,
    N_("List possible modes"),
    NULL
  },
  { NULL }
};

static gboolean
on_sigterm (gpointer user_data)
{
  MetaContext *context = META_CONTEXT (user_data);

  meta_context_terminate (context);

  return G_SOURCE_REMOVE;
}

static void
init_signal_handlers (MetaContext *context)
{
  struct sigaction act = { 0 };
  sigset_t empty_mask;

  sigemptyset (&empty_mask);
  act.sa_handler = SIG_IGN;
  act.sa_mask = empty_mask;
  act.sa_flags = 0;
  if (sigaction (SIGPIPE,  &act, NULL) < 0)
    g_warning ("Failed to register SIGPIPE handler: %s", g_strerror (errno));
#ifdef SIGXFSZ
  if (sigaction (SIGXFSZ,  &act, NULL) < 0)
    g_warning ("Failed to register SIGXFSZ handler: %s", g_strerror (errno));
#endif

  g_unix_signal_add (SIGTERM, on_sigterm, context);
}

static void
change_to_home_directory (void)
{
  const char *home_dir;

  home_dir = g_get_home_dir ();
  if (!home_dir)
    return;

  if (chdir (home_dir) < 0)
    g_warning ("Could not change to home directory %s", home_dir);
}

int
main (int argc, char **argv)
{
  g_autoptr (MetaContext) context = NULL;
  GError *error = NULL;
  int ecode = EXIT_SUCCESS;

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  context = meta_create_context (WM_NAME);
  meta_context_add_option_entries (context, gnome_shell_options,
                                   GETTEXT_PACKAGE);
  meta_context_add_option_group (context, g_irepository_get_option_group ());

  session_mode = (char *) g_getenv ("GNOME_SHELL_SESSION_MODE");

  if (!meta_context_configure (context, &argc, &argv, &error))
    {
      g_printerr ("Failed to configure: %s", error->message);
      return EXIT_FAILURE;
    }

  meta_context_set_plugin_gtype (context, gnome_shell_plugin_get_type ());
  meta_context_set_gnome_wm_keybindings (context, GNOME_WM_KEYBINDINGS);

  init_signal_handlers (context);
  change_to_home_directory ();

  if (!meta_context_setup (context, &error))
    {
      g_printerr ("Failed to setup: %s", error->message);
      return EXIT_FAILURE;
    }

  /* FIXME: Add gjs API to set this stuff and don't depend on the
   * environment.  These propagate to child processes.
   */
  g_setenv ("GJS_DEBUG_OUTPUT", "stderr", TRUE);
  g_setenv ("GJS_DEBUG_TOPICS", "JS ERROR;JS LOG", TRUE);

  shell_init_debug (g_getenv ("SHELL_DEBUG"));

  shell_dbus_init (meta_context_is_replacing (context));
  shell_a11y_init ();
  shell_perf_log_init ();
  shell_introspection_init ();
  shell_fonts_init ();

  g_log_set_writer_func (default_log_writer, NULL, NULL);

  /* Initialize the global object */
  if (session_mode == NULL)
    session_mode = is_gdm_mode ? (char *)"gdm" : (char *)"user";

  _shell_global_init ("session-mode", session_mode, NULL);

  dump_gjs_stack_on_signal (SIGABRT);
  dump_gjs_stack_on_signal (SIGFPE);
  dump_gjs_stack_on_signal (SIGIOT);
  dump_gjs_stack_on_signal (SIGTRAP);

  if ((_shell_debug & SHELL_DEBUG_BACKTRACE_SEGFAULTS))
    {
      dump_gjs_stack_on_signal (SIGBUS);
      dump_gjs_stack_on_signal (SIGSEGV);
    }

  shell_profiler_init ();

  if (meta_context_get_compositor_type (context) == META_COMPOSITOR_TYPE_WAYLAND)
    meta_context_raise_rlimit_nofile (context, NULL);

  if (!meta_context_start (context, &error))
    {
      g_printerr ("GNOME Shell failed to start: %s", error->message);
      return EXIT_FAILURE;
    }

  if (!meta_context_run_main_loop (context, &error))
    {
      g_printerr ("GNOME Shell terminated with an error: %s", error->message);
      ecode = EXIT_FAILURE;
    }

  meta_context_destroy (g_steal_pointer (&context));

  shell_profiler_shutdown ();

  g_debug ("Doing final cleanup");
  _shell_global_destroy_gjs_context (shell_global_get ());
  g_object_unref (shell_global_get ());

  return ecode;
}
