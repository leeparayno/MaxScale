/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file monitor.c  - The monitor module management routines
 *
 * @verbatim
 * Revision History
 *
 * Date         Who                     Description
 * 08/07/13     Mark Riddoch            Initial implementation
 * 23/05/14     Massimiliano Pinto      Addition of monitor_interval parameter
 *                                      and monitor id
 * 30/10/14     Massimiliano Pinto      Addition of disable_master_failback parameter
 * 07/11/14     Massimiliano Pinto      Addition of monitor network timeouts
 * 08/05/15     Markus Makela           Moved common monitor variables to MONITOR struct
 *
 * @endverbatim
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monitor.h>
#include <spinlock.h>
#include <modules.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <secrets.h>
#include <maxscale_pcre2.h>
#include <externcmd.h>
#include <mysqld_error.h>
#include <mysql_utils.h>

/*
 *  Create declarations of the enum for monitor events and also the array of
 *  structs containing the matching names. The data is taken from def_monitor_event.h
 *
 */

#undef ADDITEM
#define ADDITEM( _event_type, _event_name ) { #_event_name }
const monitor_def_t monitor_event_definitions[MAX_MONITOR_EVENT] =
{
#include "def_monitor_event.h"
};
#undef ADDITEM

static MONITOR  *allMonitors = NULL;
static SPINLOCK monLock = SPINLOCK_INIT;

static void monitor_servers_free(MONITOR_SERVERS *servers);

/**
 * Allocate a new monitor, load the associated module for the monitor
 * and start execution on the monitor.
 *
 * @param name          The name of the monitor module to load
 * @param module        The module to load
 * @return      The newly created monitor
 */
MONITOR *
monitor_alloc(char *name, char *module)
{
    MONITOR *mon;

    if ((mon = (MONITOR *)malloc(sizeof(MONITOR))) == NULL)
    {
        return NULL;
    }

    if ((mon->module = load_module(module, MODULE_MONITOR)) == NULL)
    {
        MXS_ERROR("Unable to load monitor module '%s'.", name);
        free(mon);
        return NULL;
    }
    mon->state = MONITOR_STATE_ALLOC;
    mon->name = strdup(name);
    mon->handle = NULL;
    mon->databases = NULL;
    mon->password = NULL;
    mon->user = NULL;
    mon->password = NULL;
    mon->read_timeout = DEFAULT_READ_TIMEOUT;
    mon->write_timeout = DEFAULT_WRITE_TIMEOUT;
    mon->connect_timeout = DEFAULT_CONNECT_TIMEOUT;
    mon->interval = MONITOR_INTERVAL;
    mon->parameters = NULL;
    spinlock_init(&mon->lock);
    spinlock_acquire(&monLock);
    mon->next = allMonitors;
    allMonitors = mon;
    spinlock_release(&monLock);

    return mon;
}

/**
 * Free a monitor, first stop the monitor and then remove the monitor from
 * the chain of monitors and free the memory.
 *
 * @param mon   The monitor to free
 */
void
monitor_free(MONITOR *mon)
{
    MONITOR *ptr;

    mon->module->stopMonitor(mon);
    mon->state = MONITOR_STATE_FREED;
    spinlock_acquire(&monLock);
    if (allMonitors == mon)
    {
        allMonitors = mon->next;
    }
    else
    {
        ptr = allMonitors;
        while (ptr->next && ptr->next != mon)
        {
            ptr = ptr->next;
        }
        if (ptr->next)
        {
            ptr->next = mon->next;
        }
    }
    spinlock_release(&monLock);
    free_config_parameter(mon->parameters);
    monitor_servers_free(mon->databases);
    free(mon->name);
    free(mon);
}


/**
 * Start an individual monitor that has previously been stopped.
 *
 * @param monitor The Monitor that should be started
 */
void
monitorStart(MONITOR *monitor, void* params)
{
    spinlock_acquire(&monitor->lock);

    if ((monitor->handle = (*monitor->module->startMonitor)(monitor, params)))
    {
        monitor->state = MONITOR_STATE_RUNNING;
    }
    else
    {
        MXS_ERROR("Failed to start monitor '%s'.", monitor->name);
    }

    spinlock_release(&monitor->lock);
}

/**
 * Start all monitors
 */
void monitorStartAll()
{
    MONITOR *ptr;

    spinlock_acquire(&monLock);
    ptr = allMonitors;
    while (ptr)
    {
        monitorStart(ptr, ptr->parameters);
        ptr = ptr->next;
    }
    spinlock_release(&monLock);
}

/**
 * Stop a given monitor
 *
 * @param monitor       The monitor to stop
 */
void
monitorStop(MONITOR *monitor)
{
    spinlock_acquire(&monitor->lock);

    /** Only stop the monitor if it is running */
    if (monitor->state == MONITOR_STATE_RUNNING)
    {
        monitor->state = MONITOR_STATE_STOPPING;
        monitor->module->stopMonitor(monitor);
        monitor->state = MONITOR_STATE_STOPPED;

        MONITOR_SERVERS* db = monitor->databases;
        while (db)
        {
            // TODO: Create a generic entry point for this or move it inside stopMonitor
            mysql_close(db->con);
            db->con = NULL;
            db = db->next;
        }
    }

    spinlock_release(&monitor->lock);
}

/**
 * Shutdown all running monitors
 */
void
monitorStopAll()
{
    MONITOR *ptr;

    spinlock_acquire(&monLock);
    ptr = allMonitors;
    while (ptr)
    {
        monitorStop(ptr);
        ptr = ptr->next;
    }
    spinlock_release(&monLock);
}

/**
 * Add a server to a monitor. Simply register the server that needs to be
 * monitored to the running monitor module.
 *
 * @param mon           The Monitor instance
 * @param server        The Server to add to the monitoring
 */
void
monitorAddServer(MONITOR *mon, SERVER *server)
{
    MONITOR_SERVERS     *ptr, *db;

    if ((db = (MONITOR_SERVERS *)malloc(sizeof(MONITOR_SERVERS))) == NULL)
    {
        return;
    }
    db->server = server;
    db->con = NULL;
    db->next = NULL;
    db->mon_err_count = 0;
    db->log_version_err = true;
    /** Server status is uninitialized */
    db->mon_prev_status = -1;
    /* pending status is updated by get_replication_tree */
    db->pending_status = 0;

    spinlock_acquire(&mon->lock);

    if (mon->databases == NULL)
    {
        mon->databases = db;
    }
    else
    {
        ptr = mon->databases;
        while (ptr->next != NULL)
        {
            ptr = ptr->next;
        }
        ptr->next = db;
    }
    spinlock_release(&mon->lock);
}

/**
 * Free monitor server list
 * @param servers Servers to free
 */
static void monitor_servers_free(MONITOR_SERVERS *servers)
{
    while (servers)
    {
        MONITOR_SERVERS *tofree = servers;
        servers = servers->next;
        if (tofree->con)
        {
            mysql_close(tofree->con);
        }
        free(tofree);
    }
}

/**
 * Add a default user to the monitor. This user is used to connect to the
 * monitored databases but may be overriden on a per server basis.
 *
 * @param mon           The monitor instance
 * @param user          The default username to use when connecting
 * @param passwd        The default password associated to the default user.
 */
void
monitorAddUser(MONITOR *mon, char *user, char *passwd)
{
    mon->user = strdup(user);
    mon->password = strdup(passwd);
}

/**
 * Show all monitors
 *
 * @param dcb   DCB for printing output
 */
void
monitorShowAll(DCB *dcb)
{
    MONITOR *ptr;

    spinlock_acquire(&monLock);
    ptr = allMonitors;
    while (ptr)
    {
        monitorShow(dcb, ptr);
        ptr = ptr->next;
    }
    spinlock_release(&monLock);
}

/**
 * Show a single monitor
 *
 * @param dcb   DCB for printing output
 */
void
monitorShow(DCB *dcb, MONITOR *monitor)
{

    dcb_printf(dcb, "Monitor: %p\n", monitor);
    dcb_printf(dcb, "\tName:                   %s\n", monitor->name);
    if (monitor->handle)
    {
        if (monitor->module->diagnostics)
        {
            monitor->module->diagnostics(dcb, monitor);
        }
        else
        {
            dcb_printf(dcb, "\t(no diagnostics)\n");
        }
    }
    else
    {
        dcb_printf(dcb, "\tMonitor failed\n");
    }
}

/**
 * List all the monitors
 *
 * @param dcb   DCB for printing output
 */
void
monitorList(DCB *dcb)
{
    MONITOR *ptr;

    spinlock_acquire(&monLock);
    ptr = allMonitors;
    dcb_printf(dcb, "---------------------+---------------------\n");
    dcb_printf(dcb, "%-20s | Status\n", "Monitor");
    dcb_printf(dcb, "---------------------+---------------------\n");
    while (ptr)
    {
        dcb_printf(dcb, "%-20s | %s\n", ptr->name,
                   ptr->state & MONITOR_STATE_RUNNING
                   ? "Running" : "Stopped");
        ptr = ptr->next;
    }
    dcb_printf(dcb, "---------------------+---------------------\n");
    spinlock_release(&monLock);
}

/**
 * Find a monitor by name
 *
 * @param       name    The name of the monitor
 * @return      Pointer to the monitor or NULL
 */
MONITOR *
monitor_find(char *name)
{
    MONITOR *ptr;

    spinlock_acquire(&monLock);
    ptr = allMonitors;
    while (ptr)
    {
        if (!strcmp(ptr->name, name))
        {
            break;
        }
        ptr = ptr->next;
    }
    spinlock_release(&monLock);
    return ptr;
}

/**
 * Set the monitor sampling interval.
 *
 * @param mon           The monitor instance
 * @param interval      The sampling interval in milliseconds
 */
void
monitorSetInterval(MONITOR *mon, unsigned long interval)
{
    mon->interval = interval;
}

/**
 * Set Monitor timeouts for connect/read/write
 *
 * @param mon           The monitor instance
 * @param type          The timeout handling type
 * @param value         The timeout to set
 */
bool
monitorSetNetworkTimeout(MONITOR *mon, int type, int value)
{
    bool rval = true;

    if (value > 0)
    {
        switch (type)
        {
            case MONITOR_CONNECT_TIMEOUT:
                mon->connect_timeout = value;
                break;

            case MONITOR_READ_TIMEOUT:
                mon->read_timeout = value;
                break;

            case MONITOR_WRITE_TIMEOUT:
                mon->write_timeout = value;
                break;

            default:
                MXS_ERROR("Monitor setNetworkTimeout received an unsupported action type %i", type);
                rval = false;
                break;
        }
    }
    else
    {
        MXS_ERROR("Negative value for monitor timeout.");
        rval = false;
    }
    return rval;
}

/**
 * Provide a row to the result set that defines the set of monitors
 *
 * @param set   The result set
 * @param data  The index of the row to send
 * @return The next row or NULL
 */
static RESULT_ROW *
monitorRowCallback(RESULTSET *set, void *data)
{
    int *rowno = (int *)data;
    int i = 0;;
    char buf[20];
    RESULT_ROW *row;
    MONITOR *ptr;

    spinlock_acquire(&monLock);
    ptr = allMonitors;
    while (i < *rowno && ptr)
    {
        i++;
        ptr = ptr->next;
    }
    if (ptr == NULL)
    {
        spinlock_release(&monLock);
        free(data);
        return NULL;
    }
    (*rowno)++;
    row = resultset_make_row(set);
    resultset_row_set(row, 0, ptr->name);
    resultset_row_set(row, 1, ptr->state & MONITOR_STATE_RUNNING
                      ? "Running" : "Stopped");
    spinlock_release(&monLock);
    return row;
}

/**
 * Return a resultset that has the current set of monitors in it
 *
 * @return A Result set
 */
RESULTSET *
monitorGetList()
{
    RESULTSET *set;
    int *data;

    if ((data = (int *)malloc(sizeof(int))) == NULL)
    {
        return NULL;
    }
    *data = 0;
    if ((set = resultset_create(monitorRowCallback, data)) == NULL)
    {
        free(data);
        return NULL;
    }
    resultset_add_column(set, "Monitor", 20, COL_TYPE_VARCHAR);
    resultset_add_column(set, "Status", 10, COL_TYPE_VARCHAR);

    return set;
}

/**
 * @brief Check if the monitor user has all required permissions to operate properly.
 *
 * @param service Monitor to inspect
 * @param query Query to execute
 * @return True on success, false if monitor credentials lack permissions
 */
bool check_monitor_permissions(MONITOR* monitor, const char* query)
{
    if (monitor->databases == NULL)
    {
        MXS_ERROR("[%s] Monitor is missing the servers parameter.", monitor->name);
        return false;
    }

    char *user = monitor->user;
    char *dpasswd = decryptPassword(monitor->password);
    GATEWAY_CONF* cnf = config_get_global_options();
    bool rval = false;

    for (MONITOR_SERVERS *mondb = monitor->databases; mondb; mondb = mondb->next)
    {
        MYSQL *mysql = mysql_init(NULL);

        if (mysql == NULL)
        {
            MXS_ERROR("[%s] Error: MySQL connection initialization failed.", __FUNCTION__);
            break;
        }

        mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, &cnf->auth_read_timeout);
        mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &cnf->auth_conn_timeout);
        mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT, &cnf->auth_write_timeout);

        if (mxs_mysql_real_connect(mysql, mondb->server, user, dpasswd) == NULL)
        {
            MXS_ERROR("[%s] Failed to connect to server '%s' (%s:%d) when"
                      " checking monitor user credentials and permissions: %s",
                      monitor->name, mondb->server->unique_name, mondb->server->name,
                      mondb->server->port, mysql_error(mysql));
            switch (mysql_errno(mysql))
            {
                case ER_ACCESS_DENIED_ERROR:
                case ER_DBACCESS_DENIED_ERROR:
                case ER_ACCESS_DENIED_NO_PASSWORD_ERROR:
                    break;
                default:
                    rval = true;
                    break;
            }
        }
        else if (mysql_query(mysql, query) != 0)
        {
            switch (mysql_errno(mysql))
            {
                case ER_TABLEACCESS_DENIED_ERROR:
                case ER_COLUMNACCESS_DENIED_ERROR:
                case ER_SPECIFIC_ACCESS_DENIED_ERROR:
                case ER_PROCACCESS_DENIED_ERROR:
                case ER_KILL_DENIED_ERROR:
                    rval = false;
                    break;

                default:
                    rval = true;
                    break;
            }

            MXS_ERROR("[%s] Failed to execute query '%s' with user '%s'. MySQL error message: %s",
                      monitor->name, query, user, mysql_error(mysql));
        }
        else
        {
            rval = true;
            MYSQL_RES *res = mysql_use_result(mysql);
            if (res == NULL)
            {
                MXS_ERROR("[%s] Result retrieval failed when checking monitor permissions: %s",
                          monitor->name, mysql_error(mysql));
            }
            else
            {
                mysql_free_result(res);
            }
        }
        mysql_close(mysql);
    }

    free(dpasswd);
    return rval;
}

/**
 * Add parameters to the monitor
 * @param monitor Monitor
 * @param params Config parameters
 */
void monitorAddParameters(MONITOR *monitor, CONFIG_PARAMETER *params)
{
    while (params)
    {
        CONFIG_PARAMETER* clone = config_clone_param(params);
        if (clone)
        {
            clone->next = monitor->parameters;
            monitor->parameters = clone;
        }
        params = params->next;
    }
}

/**
 * Set a pending status bit in the monitor server
 *
 * @param server        The server to update
 * @param bit           The bit to clear for the server
 */
void
monitor_set_pending_status(MONITOR_SERVERS *ptr, int bit)
{
    ptr->pending_status |= bit;
}

/**
 * Clear a pending status bit in the monitor server
 *
 * @param server        The server to update
 * @param bit           The bit to clear for the server
 */
void
monitor_clear_pending_status(MONITOR_SERVERS *ptr, int bit)
{
    ptr->pending_status &= ~bit;
}

/*
 * Determine a monitor event, defined by the difference between the old
 * status of a server and the new status.
 *
 * @param   node                The monitor server data for a particular server
 * @result  monitor_event_t     A monitor event (enum)
 */
monitor_event_t
mon_get_event_type(MONITOR_SERVERS* node)
{
    typedef enum
    {
        DOWN_EVENT,
        UP_EVENT,
        LOSS_EVENT,
        NEW_EVENT,
        UNSUPPORTED_EVENT
    } general_event_type;

    general_event_type event_type = UNSUPPORTED_EVENT;

    unsigned int prev = node->mon_prev_status
                        & (SERVER_RUNNING | SERVER_MASTER | SERVER_SLAVE | SERVER_JOINED | SERVER_NDB);
    unsigned int present = node->server->status
                           & (SERVER_RUNNING | SERVER_MASTER | SERVER_SLAVE | SERVER_JOINED | SERVER_NDB);

    if (prev == present)
    {
        /* No change in the bits we're interested in */
        return UNDEFINED_MONITOR_EVENT;
    }

    if ((prev & SERVER_RUNNING) == 0)
    {
        /* The server was not running previously */
        if ((present & SERVER_RUNNING) != 0)
        {
            event_type = UP_EVENT;
        }
        /* Otherwise, was not running and still is not running */
        /* - this is not a recognised event */
    }
    else
    {
        /* Previous state must have been running */
        if ((present & SERVER_RUNNING) == 0)
        {
            event_type = DOWN_EVENT;
        }
        else
        {
            /* Was running and still is */
            if (prev & (SERVER_MASTER | SERVER_SLAVE | SERVER_JOINED | SERVER_NDB))
            {
                /* We used to know what kind of server it was */
                event_type = LOSS_EVENT;
            }
            else
            {
                /* We didn't know what kind of server it was, now we do*/
                event_type = NEW_EVENT;
            }
        }
    }

    switch (event_type)
    {
        case UP_EVENT:
            return (present & SERVER_MASTER) ? MASTER_UP_EVENT :
                   (present & SERVER_SLAVE) ? SLAVE_UP_EVENT :
                   (present & SERVER_JOINED) ? SYNCED_UP_EVENT :
                   (present & SERVER_NDB) ? NDB_UP_EVENT :
                   SERVER_UP_EVENT;
        case DOWN_EVENT:
            return (prev & SERVER_MASTER) ? MASTER_DOWN_EVENT :
                   (prev & SERVER_SLAVE) ? SLAVE_DOWN_EVENT :
                   (prev & SERVER_JOINED) ? SYNCED_DOWN_EVENT :
                   (prev & SERVER_NDB) ? NDB_DOWN_EVENT :
                   SERVER_DOWN_EVENT;
        case LOSS_EVENT:
            return (prev & SERVER_MASTER) ? LOST_MASTER_EVENT :
                   (prev & SERVER_SLAVE) ? LOST_SLAVE_EVENT :
                   (prev & SERVER_JOINED) ? LOST_SYNCED_EVENT :
                   LOST_NDB_EVENT;
        case NEW_EVENT:
            return (present & SERVER_MASTER) ? NEW_MASTER_EVENT :
                   (present & SERVER_SLAVE) ? NEW_SLAVE_EVENT :
                   (present & SERVER_JOINED) ? NEW_SYNCED_EVENT :
                   NEW_NDB_EVENT;
        default:
            return UNDEFINED_MONITOR_EVENT;
    }
}

/*
 * Given a monitor event (enum) provide a text string equivalent
 * @param   node    The monitor server data whose event is wanted
 * @result  string  The name of the monitor event for the server
 */
const char*
mon_get_event_name(MONITOR_SERVERS* node)
{
    return monitor_event_definitions[mon_get_event_type(node)].name;
}

/*
 * Given the text version of a monitor event, determine the event (enum)
 *
 * @param   event_name          String containing the event name
 * @result  monitor_event_t     Monitor event corresponding to name
 */
monitor_event_t
mon_name_to_event(const char *event_name)
{
    monitor_event_t event;

    for (event = 0; event < MAX_MONITOR_EVENT; event++)
    {
        if (0 == strcasecmp(monitor_event_definitions[event].name, event_name))
        {
            return event;
        }
    }
    return UNDEFINED_MONITOR_EVENT;
}

/**
 * Create a list of running servers
 *
 * @param servers Monitored servers
 * @param dest Destination where the string is appended, must be null terminated
 * @param len Length of @c dest
 */
static void mon_append_node_names(MONITOR_SERVERS* servers, char* dest, int len, int status)
{
    char *separator = "";
    char arr[MAX_SERVER_NAME_LEN + 32]; // Some extra space for port
    dest[0] = '\0';

    while (servers && strlen(dest) < (len - strlen(separator)))
    {
        if (status == 0 || servers->server->status & status)
        {
            strncat(dest, separator, len);
            separator = ",";
            snprintf(arr, sizeof(arr), "%s:%d", servers->server->name, servers->server->port);
            strncat(dest, arr, len - strlen(dest) - 1);
        }
        servers = servers->next;
    }
}

/**
 * Check if current monitored server status has changed
 *
 * @param mon_srv       The monitored server
 * @return              true if status has changed or false
 */
bool
mon_status_changed(MONITOR_SERVERS* mon_srv)
{
    /* Previous status is -1 if not yet set */
    return (mon_srv->mon_prev_status != -1
            && mon_srv->mon_prev_status != mon_srv->server->status);
}

/**
 * Check if current monitored server has a loggable failure status
 *
 * @param mon_srv       The monitored server
 * @return              true if failed status can be logged or false
 */
bool
mon_print_fail_status(MONITOR_SERVERS* mon_srv)
{
    return (SERVER_IS_DOWN(mon_srv->server) && mon_srv->mon_err_count == 0);
}

/**
 * Launch a script
 * @param mon Owning monitor
 * @param ptr The server which has changed state
 * @param script Script to execute
 */
void
monitor_launch_script(MONITOR* mon, MONITOR_SERVERS* ptr, char* script)
{
    EXTERNCMD* cmd = externcmd_allocate(script);

    if (cmd == NULL)
    {
        MXS_ERROR("Failed to initialize script '%s'. See previous errors for the "
                  "cause of this failure.", script);
        return;
    }

    if (externcmd_matches(cmd, "$INITIATOR"))
    {
        char initiator[strlen(ptr->server->name) + 24]; // Extra space for port
        snprintf(initiator, sizeof(initiator), "%s:%d", ptr->server->name, ptr->server->port);
        externcmd_substitute_arg(cmd, "[$]INITIATOR", initiator);
    }

    if (externcmd_matches(cmd, "$EVENT"))
    {
        externcmd_substitute_arg(cmd, "[$]EVENT", mon_get_event_name(ptr));
    }

    char nodelist[PATH_MAX + MON_ARG_MAX + 1] = {'\0'};

    if (externcmd_matches(cmd, "$NODELIST"))
    {
        mon_append_node_names(mon->databases, nodelist, sizeof(nodelist), SERVER_RUNNING);
        externcmd_substitute_arg(cmd, "[$]NODELIST", nodelist);
    }

    if (externcmd_matches(cmd, "$LIST"))
    {
        mon_append_node_names(mon->databases, nodelist, sizeof(nodelist), 0);
        externcmd_substitute_arg(cmd, "[$]LIST", nodelist);
    }

    if (externcmd_matches(cmd, "$MASTERLIST"))
    {
        mon_append_node_names(mon->databases, nodelist, sizeof(nodelist), SERVER_MASTER);
        externcmd_substitute_arg(cmd, "[$]MASTERLIST", nodelist);
    }

    if (externcmd_matches(cmd, "$SLAVELIST"))
    {
        mon_append_node_names(mon->databases, nodelist, sizeof(nodelist), SERVER_SLAVE);
        externcmd_substitute_arg(cmd, "[$]SLAVELIST", nodelist);
    }

    if (externcmd_matches(cmd, "$SYNCEDLIST"))
    {
        mon_append_node_names(mon->databases, nodelist, sizeof(nodelist), SERVER_JOINED);
        externcmd_substitute_arg(cmd, "[$]SYNCEDLIST", nodelist);
    }

    if (externcmd_execute(cmd))
    {
        MXS_ERROR("Failed to execute script '%s' on server state change event '%s'.",
                  script, mon_get_event_name(ptr));
    }
    else
    {
        MXS_NOTICE("Executed monitor script '%s' on event '%s'.",
                   script, mon_get_event_name(ptr));
    }

    externcmd_free(cmd);
}

/**
 * Parse a string of event names to an array with enabled events.
 * @param events Pointer to an array of boolean values
 * @param count Size of the array
 * @param string String to parse
 * @return 0 on success. 1 when an error has occurred or an unexpected event was
 * found.
 */
int
mon_parse_event_string(bool* events, size_t count, char* given_string)
{
    char *tok, *saved, *string = strdup(given_string);
    monitor_event_t event;

    tok = strtok_r(string, ",| ", &saved);

    if (tok == NULL)
    {
        free(string);
        return -1;
    }

    while (tok)
    {
        event = mon_name_to_event(tok);
        if (event == UNDEFINED_MONITOR_EVENT)
        {
            MXS_ERROR("Invalid event name %s", tok);
            free(string);
            return -1;
        }
        if (event < count)
        {
            events[event] = true;
            tok = strtok_r(NULL, ",| ", &saved);
        }
    }

    free(string);
    return 0;
}

/**
 * Connect to a database. This will always leave a valid database handle in the
 * database->con pointer. This allows the user to call MySQL C API functions to
 * find out the reason of the failure.
 * @param mon Monitor
 * @param database Monitored database
 * @return MONITOR_CONN_OK if the connection is OK else the reason for the failure
 */
connect_result_t
mon_connect_to_db(MONITOR* mon, MONITOR_SERVERS *database)
{
    connect_result_t rval = MONITOR_CONN_OK;

    /** Return if the connection is OK */
    if (database->con && mysql_ping(database->con) == 0)
    {
        return rval;
    }

    if (database->con)
    {
        mysql_close(database->con);
    }

    if ((database->con = mysql_init(NULL)))
    {
        char *uname = database->server->monuser ? database->server->monuser : mon->user;
        char *passwd = database->server->monpw ? database->server->monpw : mon->password;
        char *dpwd = decryptPassword(passwd);

        mysql_options(database->con, MYSQL_OPT_CONNECT_TIMEOUT, (void *) &mon->connect_timeout);
        mysql_options(database->con, MYSQL_OPT_READ_TIMEOUT, (void *) &mon->read_timeout);
        mysql_options(database->con, MYSQL_OPT_WRITE_TIMEOUT, (void *) &mon->write_timeout);

        time_t start = time(NULL);
        bool result = (mxs_mysql_real_connect(database->con, database->server, uname, dpwd) != NULL);
        time_t end = time(NULL);

        if (!result)
        {
            if ((int) difftime(end, start) >= mon->connect_timeout)
            {
                rval = MONITOR_CONN_TIMEOUT;
            }
            else
            {
                rval = MONITOR_CONN_REFUSED;
            }
        }

        free(dpwd);
    }
    else
    {
        rval = MONITOR_CONN_REFUSED;
    }

    return rval;
}

/**
 * Log an error about the failure to connect to a backend server
 * and why it happened.
 * @param database Backend database
 * @param rval Return value of mon_connect_to_db
 */
void
mon_log_connect_error(MONITOR_SERVERS* database, connect_result_t rval)
{
    MXS_ERROR(rval == MONITOR_CONN_TIMEOUT ?
              "Monitor timed out when connecting to "
              "server %s:%d : \"%s\"" :
              "Monitor was unable to connect to "
              "server %s:%d : \"%s\"",
              database->server->name,
              database->server->port,
              mysql_error(database->con));
}

void mon_log_state_change(MONITOR_SERVERS *ptr)
{
    SERVER srv;
    srv.status = ptr->mon_prev_status;
    char *prev = server_status(&srv);
    char *next = server_status(ptr->server);
    MXS_NOTICE("Server changed state: %s[%s:%u]: %s. [%s] -> [%s]",
               ptr->server->unique_name, ptr->server->name, ptr->server->port,
               mon_get_event_name(ptr), prev, next);
    free(prev);
    free(next);
}
