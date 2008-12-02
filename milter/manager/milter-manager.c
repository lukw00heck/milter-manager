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

#include <locale.h>
#include <glib/gi18n.h>

#include <stdlib.h>
#include <signal.h>
#include <errno.h>

#include "milter-manager.h"
#include "milter-manager-leader.h"
#include "milter-manager-logger.h"

#define MILTER_MANAGER_GET_PRIVATE(obj)                 \
    (G_TYPE_INSTANCE_GET_PRIVATE((obj),                 \
                                 MILTER_TYPE_MANAGER,   \
                                 MilterManagerPrivate))

typedef struct _MilterManagerPrivate MilterManagerPrivate;
struct _MilterManagerPrivate
{
    MilterManagerConfiguration *configuration;
    MilterManagerLogger *logger;
    GList *leaders;
};

enum
{
    PROP_0,
    PROP_CONFIGURATION
};

G_DEFINE_TYPE(MilterManager, milter_manager, MILTER_TYPE_CLIENT)

static void dispose        (GObject         *object);
static void set_property   (GObject         *object,
                            guint            prop_id,
                            const GValue    *value,
                            GParamSpec      *pspec);
static void get_property   (GObject         *object,
                            guint            prop_id,
                            GValue          *value,
                            GParamSpec      *pspec);

static void   connection_established      (MilterClient *client,
                                           MilterClientContext *context);
static gchar *get_default_connection_spec (MilterClient *client);

static void
milter_manager_class_init (MilterManagerClass *klass)
{
    GObjectClass *gobject_class;
    MilterClientClass *client_class;
    GParamSpec *spec;

    gobject_class = G_OBJECT_CLASS(klass);
    client_class = MILTER_CLIENT_CLASS(klass);

    gobject_class->dispose      = dispose;
    gobject_class->set_property = set_property;
    gobject_class->get_property = get_property;

    client_class->connection_established      = connection_established;
    client_class->get_default_connection_spec = get_default_connection_spec;

    spec = g_param_spec_object("configuration",
                               "Configuration",
                               "The configuration of the milter controller",
                               MILTER_TYPE_MANAGER_CONFIGURATION,
                               G_PARAM_READWRITE);
    g_object_class_install_property(gobject_class, PROP_CONFIGURATION, spec);

    g_type_class_add_private(gobject_class,
                             sizeof(MilterManagerPrivate));
}

static void
milter_manager_init (MilterManager *manager)
{
    MilterManagerPrivate *priv;
    const gchar *config_dir_env;

    priv = MILTER_MANAGER_GET_PRIVATE(manager);

    priv->configuration = milter_manager_configuration_new(NULL);
    config_dir_env = g_getenv("MILTER_MANAGER_CONFIG_DIR");
    if (config_dir_env)
        milter_manager_configuration_add_load_path(priv->configuration,
                                                   config_dir_env);
    milter_manager_configuration_add_load_path(priv->configuration, CONFIG_DIR);
    milter_manager_configuration_reload(priv->configuration);

    priv->leaders = NULL;
    priv->logger = milter_manager_logger_new();
}

static void
dispose (GObject *object)
{
    MilterManagerPrivate *priv;

    priv = MILTER_MANAGER_GET_PRIVATE(object);

    if (priv->configuration) {
        g_object_unref(priv->configuration);
        priv->configuration = NULL;
    }

    if (priv->leaders) {
        g_list_foreach(priv->leaders, (GFunc)g_object_unref, NULL);
        g_list_free(priv->leaders);
        priv->leaders = NULL;
    }

    if (priv->logger) {
        g_object_unref(priv->logger);
        priv->logger = NULL;
    }

    G_OBJECT_CLASS(milter_manager_parent_class)->dispose(object);
}

static void
set_property (GObject      *object,
              guint         prop_id,
              const GValue *value,
              GParamSpec   *pspec)
{
    MilterManagerPrivate *priv;

    priv = MILTER_MANAGER_GET_PRIVATE(object);
    switch (prop_id) {
      case PROP_CONFIGURATION:
        if (priv->configuration)
            g_object_unref(priv->configuration);
        priv->configuration = g_value_get_object(value);
        if (priv->configuration)
            g_object_ref(priv->configuration);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
    MilterManagerPrivate *priv;

    priv = MILTER_MANAGER_GET_PRIVATE(object);
    switch (prop_id) {
      case PROP_CONFIGURATION:
        g_value_set_object(value, priv->configuration);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

MilterManager *
milter_manager_new (void)
{
    return g_object_new(MILTER_TYPE_MANAGER, NULL);
}

static MilterStatus
cb_client_negotiate (MilterClientContext *context, MilterOption *option,
                     gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_negotiate(leader, option);
}

static MilterStatus
cb_client_connect (MilterClientContext *context, const gchar *host_name,
                   struct sockaddr *address, socklen_t address_length,
                   gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_connect(leader, host_name,
                                         address, address_length);
}

static MilterStatus
cb_client_helo (MilterClientContext *context, const gchar *fqdn,
                gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_helo(leader, fqdn);
}

static MilterStatus
cb_client_envelope_from (MilterClientContext *context, const gchar *from,
                         gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_envelope_from(leader, from);
}

static MilterStatus
cb_client_envelope_recipient (MilterClientContext *context,
                              const gchar *recipient, gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_envelope_recipient(leader, recipient);
}

static MilterStatus
cb_client_data (MilterClientContext *context, gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_data(leader);
}

static MilterStatus
cb_client_unknown (MilterClientContext *context, const gchar *command,
                   gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_unknown(leader, command);
}

static MilterStatus
cb_client_header (MilterClientContext *context,
                  const gchar *name, const gchar *value,
                  gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_header(leader, name, value);
}

static MilterStatus
cb_client_end_of_header (MilterClientContext *context, gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_end_of_header(leader);
}

static MilterStatus
cb_client_body (MilterClientContext *context, const gchar *chunk, gsize size,
                gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_body(leader, chunk, size);
}

static MilterStatus
cb_client_end_of_message (MilterClientContext *context, gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_end_of_message(leader, NULL, 0);
}

static MilterStatus
cb_client_abort (MilterClientContext *context, gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    return milter_manager_leader_abort(leader);
}

static void
cb_client_define_macro (MilterClientContext *context, MilterCommand command,
                        GHashTable *macros, gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    milter_manager_leader_define_macro(leader, command, macros);
}

static void
cb_client_mta_timeout (MilterClientContext *context, gpointer user_data)
{
    MilterManagerLeader *leader = user_data;

    milter_manager_leader_mta_timeout(leader);
}

static MilterStatus
cb_client_quit (MilterClientContext *context, gpointer user_data)
{
    MilterManagerLeader *leader = user_data;
    MilterStatus status;

    status = milter_manager_leader_quit(leader);

#define DISCONNECT(name)                                                \
    g_signal_handlers_disconnect_by_func(context,                       \
                                         G_CALLBACK(cb_client_ ## name), \
                                         user_data)

    DISCONNECT(negotiate);
    DISCONNECT(connect);
    DISCONNECT(helo);
    DISCONNECT(envelope_from);
    DISCONNECT(envelope_recipient);
    DISCONNECT(data);
    DISCONNECT(unknown);
    DISCONNECT(header);
    DISCONNECT(end_of_header);
    DISCONNECT(body);
    DISCONNECT(end_of_message);
    DISCONNECT(quit);
    DISCONNECT(abort);
    DISCONNECT(define_macro);
    DISCONNECT(mta_timeout);

#undef DISCONNECT

    g_object_unref(leader);

    return status;
}

static void
setup_context_signals (MilterClientContext *context,
                       MilterManagerConfiguration *configuration)
{
    MilterManagerLeader *leader;

    leader = milter_manager_leader_new(configuration, context);

#define CONNECT(name)                                   \
    g_signal_connect(context, #name,                    \
                     G_CALLBACK(cb_client_ ## name),    \
                     leader)

    CONNECT(negotiate);
    CONNECT(connect);
    CONNECT(helo);
    CONNECT(envelope_from);
    CONNECT(envelope_recipient);
    CONNECT(data);
    CONNECT(unknown);
    CONNECT(header);
    CONNECT(end_of_header);
    CONNECT(body);
    CONNECT(end_of_message);
    CONNECT(quit);
    CONNECT(abort);
    CONNECT(define_macro);
    CONNECT(mta_timeout);

#undef CONNECT
}

static void
connection_established (MilterClient *client, MilterClientContext *context)
{
    MilterManagerPrivate *priv;
    MilterManagerConfiguration *configuration;

    priv = MILTER_MANAGER_GET_PRIVATE(client);
    configuration = priv->configuration;
    setup_context_signals(context, configuration);
}

static gchar *
get_default_connection_spec (MilterClient *client)
{
    MilterManager *manager;
    MilterManagerPrivate *priv;
    MilterManagerConfiguration *configuration;
    const gchar *spec;

    manager = MILTER_MANAGER(client);
    priv = MILTER_MANAGER_GET_PRIVATE(manager);
    configuration = priv->configuration;
    spec =
        milter_manager_configuration_get_manager_connection_spec(configuration);
    if (spec) {
        return g_strdup(spec);
    } else {
        MilterClientClass *klass;

        klass = MILTER_CLIENT_CLASS(milter_manager_parent_class);
        return klass->get_default_connection_spec(MILTER_CLIENT(manager));
    }
}

MilterManagerConfiguration *
milter_manager_get_configuration (MilterManager *manager)
{
    return MILTER_MANAGER_GET_PRIVATE(manager)->configuration;
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/
