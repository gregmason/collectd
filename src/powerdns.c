/**
 * collectd - src/powerdns.c
 * Copyright (C) 2007-2008  C-Ware, Inc.
 * Copyright (C) 2008       Florian Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Author:
 *   Luke Heberling <lukeh at c-ware.com>
 *   Florian Forster <octo at verplant.org>
 *
 * DESCRIPTION
 *   Queries a PowerDNS control socket for statistics
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"
#include "utils_llist.h"

#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifndef UNIX_PATH_MAX
# define UNIX_PATH_MAX sizeof (((struct sockaddr_un *)0)->sun_path)
#endif
#define FUNC_ERROR(func) do { char errbuf[1024]; ERROR ("powerdns plugin: %s failed: %s", func, sstrerror (errno, errbuf, sizeof (errbuf))); } while (0)

#define SERVER_SOCKET  "/var/run/pdns.controlsocket"
#define SERVER_COMMAND "SHOW *"

#define RECURSOR_SOCKET  "/var/run/pdns_recursor.controlsocket"
#define RECURSOR_COMMAND "get all-outqueries answers0-1 " /* {{{ */ \
  "answers100-1000 answers10-100 answers1-10 answers-slow cache-entries " \
  "cache-hits cache-misses chain-resends client-parse-errors " \
  "concurrent-queries dlg-only-drops ipv6-outqueries negcache-entries " \
  "noerror-answers nsset-invalidations nsspeeds-entries nxdomain-answers " \
  "outgoing-timeouts qa-latency questions resource-limits " \
  "server-parse-errors servfail-answers spoof-prevents sys-msec " \
  "tcp-client-overflow tcp-outqueries tcp-questions throttled-out " \
  "throttled-outqueries throttle-entries unauthorized-tcp unauthorized-udp " \
  "unexpected-packets unreachables user-msec" /* }}} */

struct list_item_s;
typedef struct list_item_s list_item_t;

struct list_item_s
{
  int (*func) (list_item_t *item);
  char *instance;
  char *command;
  struct sockaddr_un sockaddr;
  int socktype;
};

struct statname_lookup_s
{
  char *name;
  char *type;
  char *type_instance;
};
typedef struct statname_lookup_s statname_lookup_t;

/* Description of statistics returned by the recursor: {{{
all-outqueries      counts the number of outgoing UDP queries since starting
answers0-1          counts the number of queries answered within 1 milisecond
answers100-1000     counts the number of queries answered within 1 second
answers10-100       counts the number of queries answered within 100 miliseconds
answers1-10         counts the number of queries answered within 10 miliseconds
answers-slow        counts the number of queries answered after 1 second
cache-entries       shows the number of entries in the cache
cache-hits          counts the number of cache hits since starting
cache-misses        counts the number of cache misses since starting
chain-resends       number of queries chained to existing outstanding query
client-parse-errors counts number of client packets that could not be parsed
concurrent-queries  shows the number of MThreads currently running
dlg-only-drops      number of records dropped because of delegation only setting
negcache-entries    shows the number of entries in the Negative answer cache
noerror-answers     counts the number of times it answered NOERROR since starting
nsspeeds-entries    shows the number of entries in the NS speeds map
nsset-invalidations number of times an nsset was dropped because it no longer worked
nxdomain-answers    counts the number of times it answered NXDOMAIN since starting
outgoing-timeouts   counts the number of timeouts on outgoing UDP queries since starting
qa-latency          shows the current latency average
questions           counts all End-user initiated queries with the RD bit set
resource-limits     counts number of queries that could not be performed because of resource limits
server-parse-errors counts number of server replied packets that could not be parsed
servfail-answers    counts the number of times it answered SERVFAIL since starting
spoof-prevents      number of times PowerDNS considered itself spoofed, and dropped the data
sys-msec            number of CPU milliseconds spent in 'system' mode
tcp-client-overflow number of times an IP address was denied TCP access because it already had too many connections
tcp-outqueries      counts the number of outgoing TCP queries since starting
tcp-questions       counts all incoming TCP queries (since starting)
throttled-out       counts the number of throttled outgoing UDP queries since starting
throttle-entries    shows the number of entries in the throttle map
unauthorized-tcp    number of TCP questions denied because of allow-from restrictions
unauthorized-udp    number of UDP questions denied because of allow-from restrictions
unexpected-packets  number of answers from remote servers that were unexpected (might point to spoofing)
uptime              number of seconds process has been running (since 3.1.5)
user-msec           number of CPU milliseconds spent in 'user' mode
}}} */

statname_lookup_t lookup_table[] = /* {{{ */
{
  /*********************
   * Server statistics *
   *********************/
  /* Questions */
  {"recursing-questions",    "dns_question", "recurse"},
  {"tcp-queries",            "dns_question", "tcp"},
  {"udp-queries",            "dns_question", "udp"},

  /* Answers */
  {"recursing-answers",      "dns_answer",   "recurse"},
  {"tcp-answers",            "dns_answer",   "tcp"},
  {"udp-answers",            "dns_answer",   "udp"},

  /* Cache stuff */
  {"packetcache-hit",        "cache_result", "packet-hit"},
  {"packetcache-miss",       "cache_result", "packet-miss"},
  {"packetcache-size",       "cache_size",   "packet"},
  {"query-cache-hit",        "cache_result", "query-hit"},
  {"query-cache-miss",       "cache_result", "query-miss"},

  /* Latency */
  {"latency",                "latency",      NULL},

  /* Other stuff.. */
  {"corrupt-packets",        "io_packets",   "corrupt"},
  {"deferred-cache-inserts", "counter",      "cache-deferred_insert"},
  {"deferred-cache-lookup",  "counter",      "cache-deferred_lookup"},
  {"qsize-a",                "cache_size",   "answers"},
  {"qsize-q",                "cache_size",   "questions"},
  {"servfail-packets",       "io_packets",   "servfail"},
  {"timedout-packets",       "io_packets",   "timeout"},
  {"udp4-answers",           "dns_answer",   "udp4"},
  {"udp4-queries",           "dns_question", "queries-udp4"},
  {"udp6-answers",           "dns_answer",   "udp6"},
  {"udp6-queries",           "dns_question", "queries-udp6"},

  /***********************
   * Recursor statistics *
   ***********************/
  /* Answers by return code */
  {"noerror-answers",     "dns_rcode",    "NOERROR"},
  {"nxdomain-answers",    "dns_rcode",    "NXDOMAIN"},
  {"servfail-answers",    "dns_rcode",    "SERVFAIL"},

  /* CPU utilization */
  {"sys-msec",            "cpu",          "system"},
  {"user-msec",           "cpu",          "user"},

  /* Question-to-answer latency */
  {"qa-latency",          "latency",      NULL},

  /* Cache */
  {"cache-entries",       "cache_size",   NULL},
  {"cache-hits",          "cache_result", "hit"},
  {"cache-misses",        "cache_result", "miss"},

  /* Total number of questions.. */
  {"questions",           "dns_qtype",    "total"}

  /* TODO: Add all recursor metrics here */
}; /* }}} */
int lookup_table_length = STATIC_ARRAY_SIZE (lookup_table);

static llist_t *list = NULL;

#define PDNS_LOCAL_SOCKPATH LOCALSTATEDIR"/run/"PACKAGE_NAME"-powerdns"
static char *local_sockpath = NULL;

/* TODO: Do this before 4.4:
 * - Make list of ``interesting values'' configurable
 * - Authorative server:
 *   - Store each element in a list or an array
 *   - Check values returned by `SHOW *' against that list.
 * - Recursor:
 *   - Use the list to build a command to request the given values
 *   - Complete list of known pdns -> collectd mappings.
 *
 * -octo
 */

/* <http://doc.powerdns.com/recursor-stats.html> */
static void submit (const char *plugin_instance, /* {{{ */
    const char *pdns_type, const char *value)
{
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[1];

  const char *type = NULL;
  const char *type_instance = NULL;
  const data_set_t *ds;

  int i;

  for (i = 0; i < lookup_table_length; i++)
    if (strcmp (lookup_table[i].name, pdns_type) == 0)
      break;

  if (lookup_table[i].type == NULL)
    return;

  if (i >= lookup_table_length)
  {
    DEBUG ("powerdns plugin: submit: Not found in lookup table: %s = %s;",
        pdns_type, value);
    return;
  }

  type = lookup_table[i].type;
  type_instance = lookup_table[i].type_instance;

  ds = plugin_get_ds (type);
  if (ds == NULL)
  {
    ERROR ("powerdns plugin: The lookup table returned type `%s', "
        "but I cannot find it via `plugin_get_ds'.",
        type);
    return;
  }

  if (ds->ds_num != 1)
  {
    ERROR ("powerdns plugin: type `%s' has %i data sources, "
        "but I can only handle one.",
        type, ds->ds_num);
    return;
  }

  if (ds->ds[0].type == DS_TYPE_GAUGE)
  {
    char *endptr = NULL;

    values[0].gauge = strtod (value, &endptr);

    if (endptr == value)
    {
      ERROR ("powerdns plugin: Cannot convert `%s' "
          "to a floating point number.", value);
      return;
    }
  }
  else
  {
    char *endptr = NULL;

    values[0].counter = strtoll (value, &endptr, 0);
    if (endptr == value)
    {
      ERROR ("powerdns plugin: Cannot convert `%s' "
          "to an integer number.", value);
      return;
    }
  }

  vl.values = values;
  vl.values_len = 1;
  vl.time = time (NULL);
  sstrncpy (vl.host, hostname_g, sizeof (vl.host));
  sstrncpy (vl.plugin, "powerdns", sizeof (vl.plugin));
  if (type_instance != NULL)
    sstrncpy (vl.type_instance, type_instance, sizeof (vl.type_instance));
  sstrncpy (vl.plugin_instance, plugin_instance, sizeof (vl.plugin_instance));

  plugin_dispatch_values (type, &vl);
} /* }}} static void submit */

static int powerdns_get_data_dgram (list_item_t *item, /* {{{ */
    char **ret_buffer,
    size_t *ret_buffer_size)
{
  int sd;
  int status;

  char temp[4096];
  char *buffer = NULL;
  size_t buffer_size = 0;

  struct sockaddr_un sa_unix;

  sd = socket (PF_UNIX, item->socktype, 0);
  if (sd < 0)
  {
    FUNC_ERROR ("socket");
    return (-1);
  }

  memset (&sa_unix, 0, sizeof (sa_unix));
  sa_unix.sun_family = AF_UNIX;
  strncpy (sa_unix.sun_path,
      (local_sockpath != NULL) ? local_sockpath : PDNS_LOCAL_SOCKPATH,
      sizeof (sa_unix.sun_path));
  sa_unix.sun_path[sizeof (sa_unix.sun_path) - 1] = 0;

  status = unlink (sa_unix.sun_path);
  if ((status != 0) && (errno != ENOENT))
  {
    FUNC_ERROR ("unlink");
    close (sd);
    return (-1);
  }

  do /* while (0) */
  {
    /* We need to bind to a specific path, because this is a datagram socket
     * and otherwise the daemon cannot answer. */
    status = bind (sd, (struct sockaddr *) &sa_unix, sizeof (sa_unix));
    if (status != 0)
    {
      FUNC_ERROR ("bind");
      break;
    }

    /* Make the socket writeable by the daemon.. */
    status = chmod (sa_unix.sun_path, 0666);
    if (status != 0)
    {
      FUNC_ERROR ("chmod");
      break;
    }

    status = connect (sd, (struct sockaddr *) &item->sockaddr,
        sizeof (item->sockaddr));
    if (status != 0)
    {
      FUNC_ERROR ("connect");
      break;
    }

    status = send (sd, item->command, strlen (item->command), 0);
    if (status < 0)
    {
      FUNC_ERROR ("send");
      break;
    }

    status = recv (sd, temp, sizeof (temp), /* flags = */ 0);
    if (status < 0)
    {
      FUNC_ERROR ("recv");
      break;
    }
    status = 0;
  } while (0);

  close (sd);
  unlink (sa_unix.sun_path);

  if (status != 0)
    return (-1);

  buffer_size = status + 1;
  buffer = (char *) malloc (buffer_size);
  if (buffer == NULL)
  {
    FUNC_ERROR ("malloc");
    return (-1);
  }

  memcpy (buffer, temp, status);
  buffer[status] = 0;

  *ret_buffer = buffer;
  *ret_buffer_size = buffer_size;

  return (0);
} /* }}} int powerdns_get_data_dgram */

static int powerdns_get_data_stream (list_item_t *item, /* {{{ */
    char **ret_buffer,
    size_t *ret_buffer_size)
{
  int sd;
  int status;

  char temp[4096];
  char *buffer = NULL;
  size_t buffer_size = 0;

  sd = socket (PF_UNIX, item->socktype, 0);
  if (sd < 0)
  {
    FUNC_ERROR ("socket");
    return (-1);
  }

  status = connect (sd, (struct sockaddr *) &item->sockaddr,
      sizeof (item->sockaddr));
  if (status != 0)
  {
    FUNC_ERROR ("connect");
    close (sd);
    return (-1);
  }

  /* strlen + 1, because we need to send the terminating NULL byte, too. */
  status = send (sd, item->command, strlen (item->command) + 1,
      /* flags = */ 0);
  if (status < 0)
  {
    FUNC_ERROR ("send");
    close (sd);
    return (-1);
  }

  while (42)
  {
    char *buffer_new;

    status = recv (sd, temp, sizeof (temp), /* flags = */ 0);
    if (status < 0)
    {
      FUNC_ERROR ("recv");
      break;
    }
    else if (status == 0)
      break;

    buffer_new = (char *) realloc (buffer, buffer_size + status + 1);
    if (buffer_new == NULL)
    {
      FUNC_ERROR ("realloc");
      status = -1;
      break;
    }
    buffer = buffer_new;

    memcpy (buffer + buffer_size, temp, status);
    buffer_size += status;
    buffer[buffer_size] = 0;
  } /* while (42) */
  close (sd);
  sd = -1;

  if (status < 0)
  {
    sfree (buffer);
  }
  else
  {
    assert (status == 0);
    *ret_buffer = buffer;
    *ret_buffer_size = buffer_size;
  }

  return (status);
} /* }}} int powerdns_get_data_stream */

static int powerdns_get_data (list_item_t *item, char **ret_buffer,
    size_t *ret_buffer_size)
{
  if (item->socktype == SOCK_DGRAM)
    return (powerdns_get_data_dgram (item, ret_buffer, ret_buffer_size));
  else if (item->socktype == SOCK_STREAM)
    return (powerdns_get_data_stream (item, ret_buffer, ret_buffer_size));
  else
  {
    ERROR ("powerdns plugin: Unknown socket type: %i", (int) item->socktype);
    return (-1);
  }
} /* int powerdns_get_data */

static int powerdns_read_server (list_item_t *item) /* {{{ */
{
  char *buffer = NULL;
  size_t buffer_size = 0;
  int status;

  char *dummy;
  char *saveptr;

  char *key;
  char *value;

  status = powerdns_get_data (item, &buffer, &buffer_size);
  if (status != 0)
    return (-1);

  /* corrupt-packets=0,deferred-cache-inserts=0,deferred-cache-lookup=0,latency=0,packetcache-hit=0,packetcache-miss=0,packetcache-size=0,qsize-q=0,query-cache-hit=0,query-cache-miss=0,recursing-answers=0,recursing-questions=0,servfail-packets=0,tcp-answers=0,tcp-queries=0,timedout-packets=0,udp-answers=0,udp-queries=0,udp4-answers=0,udp4-queries=0,udp6-answers=0,udp6-queries=0, */
  dummy = buffer;
  saveptr = NULL;
  while ((key = strtok_r (dummy, ",", &saveptr)) != NULL)
  {
    dummy = NULL;

    value = strchr (key, '=');
    if (value == NULL)
      break;

    *value = '\0';
    value++;

    if (value[0] == '\0')
      continue;

    submit (item->instance, key, value);
  } /* while (strtok_r) */

  sfree (buffer);

  return (0);
} /* }}} int powerdns_read_server */

static int powerdns_read_recursor (list_item_t *item) /* {{{ */
{
  char *buffer = NULL;
  size_t buffer_size = 0;
  int status;

  char *dummy;

  char *keys_list;
  char *key;
  char *key_saveptr;
  char *value;
  char *value_saveptr;

  status = powerdns_get_data (item, &buffer, &buffer_size);
  if (status != 0)
    return (-1);

  keys_list = strdup (item->command);
  if (keys_list == NULL)
  {
    FUNC_ERROR ("strdup");
    sfree (buffer);
    return (-1);
  }

  key_saveptr = NULL;
  value_saveptr = NULL;

  /* Skip the `get' at the beginning */
  strtok_r (keys_list, " \t", &key_saveptr);

  dummy = buffer;
  while ((value = strtok_r (dummy, " \t\n\r", &value_saveptr)) != NULL)
  {
    dummy = NULL;

    key = strtok_r (NULL, " \t", &key_saveptr);
    if (key == NULL)
      break;

    submit (item->instance, key, value);
  } /* while (strtok_r) */

  sfree (buffer);
  sfree (keys_list);

  return (0);
} /* }}} int powerdns_read_recursor */

static int powerdns_config_add_string (const char *name, /* {{{ */
    char **dest,
    oconfig_item_t *ci)
{
  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("powerdns plugin: `%s' needs exactly one string argument.",
	name);
    return (-1);
  }

  sfree (*dest);
  *dest = strdup (ci->values[0].value.string);
  if (*dest == NULL)
    return (-1);

  return (0);
} /* }}} int ctail_config_add_string */

static int powerdns_config_add_server (oconfig_item_t *ci) /* {{{ */
{
  char *socket_temp;

  list_item_t *item;
  int status;
  int i;

  if ((ci->values_num != 1) || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("powerdns plugin: `%s' needs exactly one string argument.",
	ci->key);
    return (-1);
  }

  item = (list_item_t *) malloc (sizeof (list_item_t));
  if (item == NULL)
  {
    ERROR ("powerdns plugin: malloc failed.");
    return (-1);
  }
  memset (item, '\0', sizeof (list_item_t));

  item->instance = strdup (ci->values[0].value.string);
  if (item->instance == NULL)
  {
    ERROR ("powerdns plugin: strdup failed.");
    sfree (item);
    return (-1);
  }

  /*
   * Set default values for the members of list_item_t
   */
  if (strcasecmp ("Server", ci->key) == 0)
  {
    item->func = powerdns_read_server;
    item->command = strdup (SERVER_COMMAND);
    item->socktype = SOCK_STREAM;
    socket_temp = strdup (SERVER_SOCKET);
  }
  else if (strcasecmp ("Recursor", ci->key) == 0)
  {
    item->func = powerdns_read_recursor;
    item->command = strdup (RECURSOR_COMMAND);
    item->socktype = SOCK_DGRAM;
    socket_temp = strdup (RECURSOR_SOCKET);
  }

  status = 0;
  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *option = ci->children + i;

    if (strcasecmp ("Command", option->key) == 0)
      status = powerdns_config_add_string ("Command", &item->command, option);
    else if (strcasecmp ("Socket", option->key) == 0)
      status = powerdns_config_add_string ("Socket", &socket_temp, option);
    else
    {
      ERROR ("powerdns plugin: Option `%s' not allowed here.", option->key);
      status = -1;
    }

    if (status != 0)
      break;
  }

  while (status == 0)
  {
    llentry_t *e;

    if (socket_temp == NULL)
    {
      ERROR ("powerdns plugin: socket_temp == NULL.");
      status = -1;
      break;
    }

    if (item->command == NULL)
    {
      ERROR ("powerdns plugin: item->command == NULL.");
      status = -1;
      break;
    }

    item->sockaddr.sun_family = AF_UNIX;
    sstrncpy (item->sockaddr.sun_path, socket_temp, UNIX_PATH_MAX);

    e = llentry_create (item->instance, item);
    if (e == NULL)
    {
      ERROR ("powerdns plugin: llentry_create failed.");
      status = -1;
      break;
    }
    llist_append (list, e);

    break;
  }

  if (status != 0)
  {
    sfree (item);
    return (-1);
  }

  DEBUG ("powerdns plugin: Add server: instance = %s;", item->instance);

  return (0);
} /* }}} int powerdns_config_add_server */

static int powerdns_config (oconfig_item_t *ci) /* {{{ */
{
  int i;

  DEBUG ("powerdns plugin: powerdns_config (ci = %p);", (void *) ci);

  if (list == NULL)
  {
    list = llist_create ();

    if (list == NULL)
    {
      ERROR ("powerdns plugin: `llist_create' failed.");
      return (-1);
    }
  }

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *option = ci->children + i;

    if ((strcasecmp ("Server", option->key) == 0)
	|| (strcasecmp ("Recursor", option->key) == 0))
      powerdns_config_add_server (option);
    if (strcasecmp ("LocalSocket", option->key) == 0)
    {
      char *temp = strdup (option->key);
      if (temp == NULL)
        return (1);
      sfree (local_sockpath);
      local_sockpath = temp;
    }
    else
    {
      ERROR ("powerdns plugin: Option `%s' not allowed here.", option->key);
    }
  } /* for (i = 0; i < ci->children_num; i++) */

  return (0);
} /* }}} int powerdns_config */

static int powerdns_read (void)
{
  llentry_t *e;

  for (e = llist_head (list); e != NULL; e = e->next)
  {
    list_item_t *item = e->value;
    item->func (item);
  }

  return (0);
} /* static int powerdns_read */

static int powerdns_shutdown (void)
{
  llentry_t *e;

  if (list == NULL)
    return (0);

  for (e = llist_head (list); e != NULL; e = e->next)
  {
    list_item_t *item = (list_item_t *) e->value;
    e->value = NULL;

    sfree (item->instance);
    sfree (item->command);
    sfree (item);
  }

  llist_destroy (list);
  list = NULL;

  return (0);
} /* static int powerdns_shutdown */

void module_register (void)
{
  plugin_register_complex_config ("powerdns", powerdns_config);
  plugin_register_read ("powerdns", powerdns_read);
  plugin_register_shutdown ("powerdns", powerdns_shutdown );
} /* void module_register */

/* vim: set sw=2 sts=2 ts=8 fdm=marker : */