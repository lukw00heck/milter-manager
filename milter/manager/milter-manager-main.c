/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2008  Kouhei Sutou <kou@cozmixng.org>
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#  include "../../config.h"
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/un.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <fcntl.h>

#include <locale.h>
#include <glib/gi18n.h>

#include <glib/gstdio.h>

#include "../manager.h"
#include "milter-manager-process-launcher.h"

static gboolean initialized = FALSE;
static MilterManager *the_manager = NULL;
static gchar *option_spec = NULL;
static gchar *config_dir = NULL;

static gboolean io_detached = FALSE;

static void (*default_sigint_handler) (int signum);
static void (*default_sigterm_handler) (int signum);
static void (*default_sighup_handler) (int signum);

#define milter_manager_error(...) G_STMT_START  \
{                                               \
    if (io_detached) {                          \
        milter_error(__VA_ARGS__);              \
    } else {                                    \
        g_print(__VA_ARGS__);                   \
        g_print("\n");                          \
    }                                           \
} G_STMT_END

static gboolean
print_version (const gchar *option_name, const gchar *value,
               gpointer data, GError **error)
{
    g_print("%s %s\n", PACKAGE, VERSION);
    exit(EXIT_SUCCESS);
    return TRUE;
}

static gboolean
parse_spec_arg (const gchar *option_name,
                const gchar *value,
                gpointer data,
                GError **error)
{
    GError *spec_error = NULL;
    gboolean success;

    success = milter_connection_parse_spec(value, NULL, NULL, NULL, &spec_error);
    if (success) {
        option_spec = g_strdup(value);
    } else {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_BAD_VALUE,
                    _("%s"), spec_error->message);
        g_error_free(spec_error);
    }

    return success;
}

static const GOptionEntry option_entries[] =
{
    {"spec", 's', 0, G_OPTION_ARG_CALLBACK, parse_spec_arg,
     N_("The address of the desired communication socket."), "PROTOCOL:ADDRESS"},
    {"config-dir", 0, 0, G_OPTION_ARG_STRING, &config_dir,
     N_("The configuration directory that has configuration file."),
     "DIRECTORY"},
    {"version", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, print_version,
     N_("Show version"), NULL},
    {NULL}
};

void
milter_manager_init (int *argc, char ***argv)
{
    GOptionContext *option_context;
    GOptionGroup *main_group;
    GError *error = NULL;

    if (initialized)
        return;

    initialized = TRUE;

    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    milter_init();

    option_context = g_option_context_new(NULL);
    g_option_context_add_main_entries(option_context, option_entries, NULL);
    main_group = g_option_context_get_main_group(option_context);

    if (!g_option_context_parse(option_context, argc, argv, &error)) {
        g_print("%s\n", error->message);
        g_error_free(error);
        g_option_context_free(option_context);
        exit(EXIT_FAILURE);
    }

    _milter_manager_configuration_init();
}

void
milter_manager_quit (void)
{
    if (!initialized)
        return;

    _milter_manager_configuration_quit();
}

static void
shutdown_client (int signum)
{
    if (the_manager)
        milter_client_shutdown(MILTER_CLIENT(the_manager));

    switch (signum) {
    case SIGINT:
        signal(SIGINT, default_sigint_handler);
        break;
    case SIGTERM:
        signal(SIGTERM, default_sigterm_handler);
        break;
    default:
        signal(signum, SIG_DFL);
        break;
    }
}

static void
reload_configuration (int signum)
{
    MilterManagerConfiguration *config;

    if (!the_manager)
        return;

    config = milter_manager_get_configuration(the_manager);
    if (!config)
        return;

    milter_manager_configuration_reload(config);
}

static void
cb_error (MilterErrorEmittable *emittable, GError *error, gpointer user_data)
{
    g_print("milter-manager error: %s\n", error->message);
}

static gboolean
cb_free_controller (gpointer data)
{
    MilterManagerController *controller = data;

    g_object_unref(controller);
    return FALSE;
}

static void
cb_controller_reader_finished (MilterReader *reader, gpointer data)
{
    MilterManagerController *controller = data;

    g_idle_add(cb_free_controller, controller);
}

static void
process_controller_connection(MilterManager *manager, GIOChannel *agent_channel)
{
    MilterManagerController *controller;
    MilterWriter *writer;
    MilterReader *reader;

    controller = milter_manager_controller_new(manager);

    writer = milter_writer_io_channel_new(agent_channel);
    milter_agent_set_writer(MILTER_AGENT(controller), writer);
    g_object_unref(writer);

    reader = milter_reader_io_channel_new(agent_channel);
    milter_agent_set_reader(MILTER_AGENT(controller), reader);
    g_object_unref(reader);

    g_signal_connect(reader, "finished",
                     G_CALLBACK(cb_controller_reader_finished), controller);
}

static gboolean
accept_controller_connection (gint controller_fd, MilterManager *manager)
{
    gint agent_fd;
    GIOChannel *agent_channel;

    milter_debug("start accepting...: %d", controller_fd);
    agent_fd = accept(controller_fd, NULL, NULL);
    if (agent_fd == -1) {
        milter_error("failed to accept(): %s", g_strerror(errno));
        return TRUE;
    }

    milter_debug("accepted!: %d", agent_fd);
    agent_channel = g_io_channel_unix_new(agent_fd);
    g_io_channel_set_encoding(agent_channel, NULL, NULL);
    g_io_channel_set_flags(agent_channel, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_close_on_unref(agent_channel, TRUE);
    process_controller_connection(manager, agent_channel);
    g_io_channel_unref(agent_channel);

    return TRUE;
}

static gboolean
controller_watch_func (GIOChannel *channel, GIOCondition condition,
                       gpointer data)
{
    MilterManager *manager = data;
    gboolean keep_callback = TRUE;

    if (condition & G_IO_IN ||
        condition & G_IO_PRI) {
        guint fd;

        fd = g_io_channel_unix_get_fd(channel);
        keep_callback = accept_controller_connection(fd, manager);
    }

    if (condition & G_IO_ERR ||
        condition & G_IO_HUP ||
        condition & G_IO_NVAL) {
        gchar *message;

        message = milter_utils_inspect_io_condition_error(condition);
        milter_error("%s", message);
        g_free(message);
        keep_callback = FALSE;
    }

    return keep_callback;
}


static guint
setup_controller_connection (MilterManager *manager)
{
    MilterManagerConfiguration *config;
    const gchar *spec;
    GIOChannel *channel;
    GError *error = NULL;
    guint watch_id = 0;

    config = milter_manager_get_configuration(manager);
    spec = milter_manager_configuration_get_controller_connection_spec(config);
    if (!spec) {
        milter_debug("controller connection spec is missing. "
                     "controller connection is disabled");
        return 0;
    }

    channel = milter_connection_listen(spec, -1, NULL, NULL, &error);
    if (!channel) {
        milter_error("failed to listen controller connection: <%s>: <%s>",
                     spec, error->message);
        g_error_free(error);
        return 0;
    }

    watch_id = g_io_add_watch(channel,
                              G_IO_IN | G_IO_PRI |
                              G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                              controller_watch_func, manager);

    return watch_id;
}

typedef enum {
    READ_PIPE,
    WRITE_PIPE
} PipeMode;

static void
close_pipe (int *pipe, PipeMode mode)
{
    if (pipe[mode] == -1)
        return;
    close(pipe[mode]);
    pipe[mode] = -1;
}

static GIOChannel *
create_io_channel (int pipe, GIOFlags flag)
{
    GIOChannel *channel;

    channel = g_io_channel_unix_new(pipe);
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_channel_set_flags(channel, flag, NULL);
    g_io_channel_set_close_on_unref(channel, TRUE);

    return channel;
}

static GIOChannel *
create_read_io_channel (int pipe)
{
    return create_io_channel(pipe, G_IO_FLAG_IS_READABLE | G_IO_FLAG_NONBLOCK);
}

static GIOChannel *
create_write_io_channel (int pipe)
{
    return create_io_channel(pipe, G_IO_FLAG_IS_WRITEABLE);
}

static gboolean
create_process_launcher_pipes (gint **command_pipe, gint **reply_pipe)
{
    if (pipe(*command_pipe) == -1) {
        milter_manager_error("failed to create pipe for launcher command: %s",
                             g_strerror(errno));
        return FALSE;
    }

    if (pipe(*reply_pipe) == -1) {
        milter_manager_error("failed to create pipe for launcher reply: %s",
                             g_strerror(errno));
        close((*command_pipe)[WRITE_PIPE]);
        close((*command_pipe)[READ_PIPE]);
        return FALSE;
    }

    return TRUE;
}

static void
prepare_process_launcher_pipes_for_process_launcher (gint *command_pipe,
                                                     gint *reply_pipe,
                                                     GIOChannel **read_channel,
                                                     GIOChannel **write_channel)
{
    close_pipe(command_pipe, WRITE_PIPE);
    close_pipe(reply_pipe, READ_PIPE);

    *write_channel = create_write_io_channel(reply_pipe[WRITE_PIPE]);
    *read_channel = create_read_io_channel(command_pipe[READ_PIPE]);
}

static void
prepare_process_launcher_pipes_for_manager (gint *command_pipe,
                                            gint *reply_pipe,
                                            GIOChannel **read_channel,
                                            GIOChannel **write_channel)
{
    close_pipe(command_pipe, READ_PIPE);
    close_pipe(reply_pipe, WRITE_PIPE);

    *write_channel = create_write_io_channel(command_pipe[WRITE_PIPE]);
    *read_channel = create_read_io_channel(reply_pipe[READ_PIPE]);
}

static void
start_process_launcher (GIOChannel *read_channel, GIOChannel *write_channel)
{
    MilterManagerProcessLauncher *launcher;
    MilterReader *reader;
    MilterWriter *writer;

    reader = milter_reader_io_channel_new(read_channel);
    g_io_channel_unref(read_channel);

    writer = milter_writer_io_channel_new(write_channel);

    launcher = milter_manager_process_launcher_new();
    milter_agent_set_reader(MILTER_AGENT(launcher), reader);
    milter_agent_set_writer(MILTER_AGENT(launcher), writer);
    g_object_unref(reader);
    g_object_unref(writer);

    milter_agent_start(MILTER_AGENT(launcher));

    milter_manager_process_launcher_run(launcher);

    g_object_unref(launcher);
}

static gboolean
start_process_launcher_process (MilterManager *manager)
{
    gint command_pipe[2];
    gint reply_pipe[2];
    gint *command_pipe_p;
    gint *reply_pipe_p;
    GIOChannel *read_channel = NULL;
    GIOChannel *write_channel = NULL;

    command_pipe_p = command_pipe;
    reply_pipe_p = reply_pipe;
    if (!create_process_launcher_pipes(&command_pipe_p, &reply_pipe_p))
        return FALSE;

    switch (fork()) {
    case 0:
        g_object_unref(manager);
        prepare_process_launcher_pipes_for_process_launcher(command_pipe_p,
                                                            reply_pipe_p,
                                                            &read_channel,
                                                            &write_channel);
        start_process_launcher(read_channel, write_channel);
        _exit(EXIT_FAILURE);
        break;
    case -1:
        milter_manager_error("failed to fork process launcher process: %s",
                             g_strerror(errno));
        return FALSE;
    default:
        prepare_process_launcher_pipes_for_manager(command_pipe_p,
                                                   reply_pipe_p,
                                                   &read_channel,
                                                   &write_channel);
        milter_manager_set_launcher_channel(manager,
                                            read_channel,
                                            write_channel);
        break;
    }

    return TRUE;
}

static gboolean
switch_user (MilterManager *manager)
{
    MilterManagerConfiguration *configuration;
    const gchar *effective_user;
    struct passwd *password;

    configuration = milter_manager_get_configuration(manager);

    effective_user = milter_manager_configuration_get_effective_user(configuration);
    if (!effective_user)
        effective_user = "nobody";

    errno = 0;
    password = getpwnam(effective_user);
    if (!password) {
        if (errno == 0) {
            milter_manager_error(
                "failed to find password entry for effective user: %s",
                effective_user);
        } else {
            milter_manager_error(
                "failed to get password entry for effective user: %s: %s",
                effective_user, g_strerror(errno));
        }
        return FALSE;
    }


    if (setuid(password->pw_uid) == -1) {
        milter_manager_error("failed to change effective user: %s: %s",
                             effective_user, g_strerror(errno));
        return FALSE;
    }

    return TRUE;
}

static gboolean
switch_group (MilterManager *manager)
{
    MilterManagerConfiguration *configuration;
    const gchar *effective_group;
    struct group *group;

    configuration = milter_manager_get_configuration(manager);

    effective_group = milter_manager_configuration_get_effective_group(configuration);
    if (!effective_group)
        return TRUE;

    errno = 0;
    group = getgrnam(effective_group);
    if (!group) {
        if (errno == 0) {
            milter_manager_error(
                "failed to find group entry for effective group: %s",
                effective_group);
        } else {
            milter_manager_error(
                "failed to get group entry for effective group: %s: %s",
                effective_group, g_strerror(errno));
        }
        return FALSE;
    }


    if (setgid(group->gr_gid) == -1) {
        milter_manager_error("failed to change effective group: %s: %s",
                             effective_group, g_strerror(errno));
        return FALSE;
    }

    return TRUE;
}

static gboolean
detach_io (void)
{
    gboolean success = FALSE;
    int null_stdin_fd = -1;
    int null_stdout_fd = -1;
    int null_stderr_fd = -1;

    null_stdin_fd = open("/dev/null", O_RDONLY);
    if (null_stdin_fd == -1) {
        g_print("failed to open /dev/null for STDIN: %s\n", g_strerror(errno));
        goto cleanup;
    }

    null_stdout_fd = open("/dev/null", O_WRONLY);
    if (null_stdout_fd == -1) {
        g_print("failed to open /dev/null for STDOUT: %s\n", g_strerror(errno));
        goto cleanup;
    }

    null_stderr_fd = open("/dev/null", O_WRONLY);
    if (null_stderr_fd == -1) {
        g_print("failed to open /dev/null for STDERR: %s\n", g_strerror(errno));
        goto cleanup;
    }

    if (dup2(null_stdin_fd, STDIN_FILENO) == -1) {
        g_print("failed to detach STDIN: %s\n", g_strerror(errno));
        goto cleanup;
    }

    if (dup2(null_stdout_fd, STDOUT_FILENO) == -1) {
        g_print("failed to detach STDOUT: %s\n", g_strerror(errno));
        goto cleanup;
    }

    if (dup2(null_stderr_fd, STDERR_FILENO) == -1) {
        g_printerr("failed to detach STDERR: %s\n", g_strerror(errno));
        goto cleanup;
    }

    success = TRUE;

cleanup:
    if (null_stdin_fd == -1)
        close(null_stdin_fd);
    if (null_stdout_fd == -1)
        close(null_stdout_fd);
    if (null_stderr_fd == -1)
        close(null_stderr_fd);

    if (success)
        io_detached = TRUE;

    return success;
}

static gboolean
daemonize (void)
{
    switch (fork()) {
    case 0:
        break;
    case -1:
        g_print("failed to fork child process: %s\n", g_strerror(errno));
        return FALSE;
    default:
        _exit(EXIT_SUCCESS);
        break;
    }

    if (setsid() == -1) {
        g_print("failed to create session: %s\n", g_strerror(errno));
        return FALSE;
    }

    switch (fork()) {
    case 0:
        break;
    case -1:
        g_print("failed to fork grandchild process: %s\n", g_strerror(errno));
        return FALSE;
    default:
        _exit(EXIT_SUCCESS);
        break;
    }

    if (g_chdir("/") == -1) {
        g_print("failed to change working directory to '/': %s\n",
                g_strerror(errno));
        return FALSE;
    }

    if (!detach_io())
        return FALSE;

    return TRUE;
}

void
milter_manager_main (void)
{
    MilterClient *client;
    MilterManager *manager;
    MilterManagerConfiguration *config;
    guint controller_connection_watch_id = 0;
    GError *error = NULL;
    gboolean daemon;
    gchar *pid_file = NULL;

    config = milter_manager_configuration_new(NULL);
    if (config_dir)
        milter_manager_configuration_prepend_load_path(config, config_dir);
    milter_manager_configuration_reload(config);
    manager = milter_manager_new(config);
    g_object_unref(config);

    client = MILTER_CLIENT(manager);

    /* FIXME */
    controller_connection_watch_id = setup_controller_connection(manager);

    g_signal_connect(client, "error",
                     G_CALLBACK(cb_error), NULL);


    if (!milter_client_set_connection_spec(client, option_spec, &error)) {
        g_object_unref(manager);
        g_print("%s\n", error->message);
        return;
    }

    daemon = milter_manager_configuration_is_daemon(config);
    if (daemon && !daemonize()) {
        g_object_unref(manager);
        return;
    }

    if (milter_manager_configuration_is_privilege_mode(config) &&
        !start_process_launcher_process(manager)) {
        g_object_unref(manager);
        return;
    }

    if (geteuid() == 0 &&
        (!switch_group(manager) || !switch_user(manager))) {
        g_object_unref(manager);
        return;
    }

    pid_file = g_strdup(milter_manager_configuration_get_pid_file(config));
    if (pid_file) {
        gchar *content;
        GError *error = NULL;

        content = g_strdup_printf("%u\n", getpid());
        if (!g_file_set_contents(pid_file, content, -1, &error)) {
            milter_error("failed to save PID: %s: %s", pid_file, error->message);
            g_error_free(error);
            g_free(pid_file);
            pid_file = NULL;
        }
    }

    the_manager = manager;
    default_sigint_handler = signal(SIGINT, shutdown_client);
    default_sigterm_handler = signal(SIGTERM, shutdown_client);
    default_sighup_handler = signal(SIGHUP, reload_configuration);
    if (!milter_client_main(client))
        milter_manager_error("failed to start milter-manager process.");
    signal(SIGHUP, default_sighup_handler);
    signal(SIGTERM, default_sigterm_handler);
    signal(SIGINT, default_sigint_handler);

    if (the_manager) {
        g_object_unref(the_manager);
        the_manager = NULL;
    }

    if (pid_file) {
        if (g_unlink(pid_file) == -1)
            milter_error("failed to remove PID file: %s: %s",
                         pid_file, g_strerror(errno));
        g_free(pid_file);
    }
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
