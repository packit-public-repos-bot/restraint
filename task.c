
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <gio/gio.h>
#include <time.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "task.h"
#include "param.h"
#include "role.h"
#include "metadata.h"
#include "packages.h"
#include "common.h"

GQuark restraint_task_runner_error(void) {
    return g_quark_from_static_string("restraint-task-runner-error-quark");
}

GQuark restraint_task_fetch_error(void) {
    return g_quark_from_static_string("restraint-task-fetch-error-quark");
}

GQuark restraint_task_fetch_libarchive_error(void) {
    return g_quark_from_static_string("restraint-task-fetch-libarchive-error-quark");
}

gboolean restraint_task_fetch(AppData *app_data, GError **error) {
    g_return_val_if_fail(app_data != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    Task *task = (Task *) app_data->tasks->data;

    GError *tmp_error = NULL;
    switch (task->fetch_method) {
        case TASK_FETCH_UNPACK:
            if (g_strcmp0(soup_uri_get_scheme(task->fetch.url), "git") == 0) {
                if (!restraint_task_fetch_git(app_data, &tmp_error)) {
                    g_propagate_error(error, tmp_error);
                    return FALSE;
                }
            } else if (g_strcmp0(soup_uri_get_scheme(task->fetch.url), "http") == 0) {
                if (!restraint_task_fetch_http(app_data, &tmp_error)) {
                    g_propagate_error(error, tmp_error);
                    return FALSE;
                }
            } else {
                g_critical("XXX IMPLEMENTME");
                return FALSE;
            }
            break;
        case TASK_FETCH_INSTALL_PACKAGE:
            if (!restraint_install_package(task->fetch.package_name, &tmp_error)) {
                g_propagate_error(error, tmp_error);
                return FALSE;
            }
            break;
        default:
            g_return_val_if_reached(FALSE);
    }
    return TRUE;
}

static void build_param_var(Param *param, GPtrArray *env) {
    g_ptr_array_add(env, g_strdup_printf("%s=%s", param->name, param->value));
}

static gboolean
task_io_callback (GIOChannel *io, GIOCondition condition, gpointer user_data) {
    AppData *app_data = (AppData *) user_data;
    Task *task = (Task *) app_data->tasks->data;

    GString *s = g_string_new(NULL);

    if (condition & G_IO_IN) {
        switch (g_io_channel_read_line_string(io, s, NULL, &task->error)) {
          case G_IO_STATUS_NORMAL:
            /* Push data to our connections.. */
            connections_write(app_data->connections, s, STREAM_STDOUT, 0);
            g_string_free (s, TRUE);
            return TRUE;

          case G_IO_STATUS_ERROR:
             g_printerr("IO error: %s\n", task->error->message);
             return FALSE;

          case G_IO_STATUS_EOF:
             g_print("finished!\n");
             return FALSE;

          case G_IO_STATUS_AGAIN:
             g_warning("Not ready.. try again.");
             return TRUE;

          default:
             g_return_val_if_reached(FALSE);
             break;
        }
    }
    if (condition & G_IO_HUP){
        return FALSE;
    }

    return FALSE;
}

static void
task_pid_callback (GPid pid, gint status, gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    Task *task = (Task *) app_data->tasks->data;

    task->pid_result = status;
    if (task->pid_result != 0) {
        if (task->state == TASK_ABORTED) {
            g_set_error(&task->error, RESTRAINT_TASK_RUNNER_ERROR,
                        RESTRAINT_TASK_RUNNER_WATCHDOG_ERROR,
                        "Local watchdog expired! Killed %i with %i", task->pid, SIGKILL);
        } else if (task->state == TASK_CANCELLED) {
            g_set_error(&task->error, RESTRAINT_TASK_RUNNER_ERROR,
                        RESTRAINT_TASK_RUNNER_WATCHDOG_ERROR,
                        "Cancelled by user! Killed %i with %i", task->pid, SIGKILL);
        } else {
            g_set_error(&task->error, RESTRAINT_TASK_RUNNER_ERROR,
                        RESTRAINT_TASK_RUNNER_RC_ERROR,
                        "%s returned non-zero %i", *task->entry_point, task->pid_result);
        }
    }
}

static void
task_pid_finish (gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    Task *task = (Task *) app_data->tasks->data;

    // Remove heartbeat handler
    if (task->heartbeat_handler_id) {
        g_source_remove(task->heartbeat_handler_id);
        task->heartbeat_handler_id = 0;
    }
    // Remove local watchdog handler
    if (task->timeout_handler_id) {
        g_source_remove(task->timeout_handler_id);
        task->timeout_handler_id = 0;
    }

    if (task->state != TASK_CANCELLED) {
        if (task->error)
            task->state = TASK_FAIL;
        else
            task->state = TASK_COMPLETE;
    }

    // Add the task_handler back to finish.
    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                task_handler, 
                                                app_data,
                                                NULL);
}

static gboolean
task_timeout_callback (gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    Task *task = (Task *) app_data->tasks->data;

    // Kill process pid
    if (kill (task->pid, SIGKILL) == 0) {
        task->state = TASK_ABORTED;
    } else {
        g_set_error(&task->error, RESTRAINT_TASK_RUNNER_ERROR,
                    RESTRAINT_TASK_RUNNER_WATCHDOG_ERROR,
                    "Local watchdog expired! But we failed to kill %i with %i", task->pid, SIGKILL);
        g_warning("%s\n", task->error->message);
        // Remove pid handler
        if (task->pid_handler_id) {
            g_source_remove(task->pid_handler_id);
            task->pid_handler_id = 0;
        }
    }

    // Remove heartbeat handler
    if (task->heartbeat_handler_id) {
        g_source_remove(task->heartbeat_handler_id);
        task->heartbeat_handler_id = 0;
    }
    return FALSE;
}

static gboolean
task_heartbeat_callback (gpointer user_data)
{
    time_t rawtime;
    struct tm * timeinfo;
    GString *message = g_string_new(NULL);
    gchar currtime[80];
    AppData *app_data = (AppData *) user_data;
    Task *task = (Task *) app_data->tasks->data;

    time (&rawtime);
    timeinfo = localtime (&rawtime);
    strftime(currtime,80,"%a %b %d %H:%M:%S %Y", timeinfo);
    g_string_printf(message, "*** Current Time: %s Localwatchdog at: %s\n", currtime, task->expire_time);
    connections_write(app_data->connections, message, STREAM_STDERR, 0);
    // Log to console.log as well?
    g_string_free(message, TRUE);
    return TRUE;
}

static gboolean
task_run (AppData *app_data, GError **error)
{
    Task *task = (Task *) app_data->tasks->data;
    time_t rawtime = time (NULL);
    gint ret_fd = -1;
    struct termios term;
    struct winsize win = {
        .ws_col = 80, .ws_row = 24,
        .ws_xpixel = 480, .ws_ypixel = 192,
    };
//    gint saved_stderr = dup (STDERR_FILENO);

//    if (saved_stderr < 0) {
//        g_set_error(&task->error, RESTRAINT_TASK_RUNNER_ERROR,
//                    RESTRAINT_TASK_RUNNER_STDERR_ERROR,
//                    "Failed to save stderr");
//        return FALSE;
//    }

    task->pid = forkpty (&ret_fd, NULL, &term, &win);
    if (task->pid < 0) {
        /* Failed to fork */
        g_set_error(&task->error, RESTRAINT_TASK_RUNNER_ERROR,
                    RESTRAINT_TASK_RUNNER_FORK_ERROR,
                    "Failed to fork!");
        return FALSE;
    } else if (task->pid == 0) {
        /* Child process. */
        /* Restore stderr.  we may not want to do this.*/
//        if (dup2 (saved_stderr, STDERR_FILENO) < 0) {
//            g_set_error(&task->error, RESTRAINT_TASK_RUNNER_ERROR,
//                        RESTRAINT_TASK_RUNNER_STDERR_ERROR,
//                        "Failed to restore stderr");
//            return FALSE;
//        }
        /* chdir */
        if (chdir (task->path) == -1) {
            g_set_error(&task->error, RESTRAINT_TASK_RUNNER_ERROR,
                        RESTRAINT_TASK_RUNNER_CHDIR_ERROR,
                        "Failed to chdir() to %s", task->path);
            return FALSE;
        }
        /* Spawn the intended program. */
        environ = task->env;
        if (execvp (*task->entry_point, task->entry_point) == -1) {
            g_warning("Failed to exec() %s, %s error:%s", *task->entry_point, task->path, strerror(errno));
            exit(1);
        }
    }
    /* Parent process. */
//    close (saved_stderr);
    // Monitor pty pipe
    GIOChannel *io = g_io_channel_unix_new(ret_fd);
    task->pty_handler_id = g_io_add_watch_full (io,
                                                G_PRIORITY_DEFAULT,
                                                G_IO_IN | G_IO_HUP,
                                                task_io_callback,
                                                app_data,
                                                NULL);
    // Monitor return pid
    task->pid_handler_id = g_child_watch_add_full (G_PRIORITY_DEFAULT,
                                                   task->pid,
                                                   task_pid_callback,
                                                   app_data,
                                                   task_pid_finish);

    struct tm timeinfo = *localtime( &rawtime);
    timeinfo.tm_sec += task->max_time;
    mktime(&timeinfo);
    strftime(task->expire_time,sizeof(task->expire_time),"%a %b %d %H:%M:%S %Y", &timeinfo);

    // Local watchdog event.
    task->timeout_handler_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                           task->max_time,
                                                           task_timeout_callback,
                                                           app_data,
                                                           NULL);
    // Local heartbeat, log to console and testout.log every 5 minutes
    task->heartbeat_handler_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                           300 /* heartbeat every 5 minutes */,
                                                           task_heartbeat_callback,
                                                           app_data,
                                                           NULL);
    return TRUE;
}

static gboolean build_env(Task *task, GError **error) {
    GPtrArray *env = g_ptr_array_new();
    gchar *prefix = "";
    if (task->rhts_compat == FALSE)
        prefix = ENV_PREFIX;
    g_list_foreach(task->recipe->roles, (GFunc) build_param_var, env);
    g_list_foreach(task->roles, (GFunc) build_param_var, env);
    g_ptr_array_add(env, g_strdup_printf("%sJOBID=%s", prefix, task->recipe->job_id));
    g_ptr_array_add(env, g_strdup_printf("%sRECIPESETID=%s", prefix, task->recipe->recipe_set_id));
    g_ptr_array_add(env, g_strdup_printf("%sRECIPEID=%s", prefix, task->recipe->recipe_id));
    g_ptr_array_add(env, g_strdup_printf("%sTASKID=%s", prefix, task->task_id));
    g_ptr_array_add(env, g_strdup_printf("%sOSDISTRO=%s", prefix, task->recipe->osdistro));
    g_ptr_array_add(env, g_strdup_printf("%sOSMAJOR=%s", prefix, task->recipe->osmajor));
    g_ptr_array_add(env, g_strdup_printf("%sOSVARIANT=%s", prefix, task->recipe->osvariant ));
    g_ptr_array_add(env, g_strdup_printf("%sOSARCH=%s", prefix, task->recipe->osarch));
    g_ptr_array_add(env, g_strdup_printf("%sTASKPATH=%s", prefix, task->path));
    g_ptr_array_add(env, g_strdup_printf("%sTASKNAME=%s", prefix, task->name));
    g_ptr_array_add(env, g_strdup_printf("%sMAXTIME=%lu", prefix, task->max_time));
    g_ptr_array_add(env, g_strdup_printf("%sLAB_CONTROLLER=", prefix));
    g_ptr_array_add(env, g_strdup_printf("%sTASKORDER=%d", prefix, task->order));
    // HOME, LANG and TERM can be overriden by user by passing it as recipe or task params.
    g_ptr_array_add(env, g_strdup_printf("HOME=/root"));
    g_ptr_array_add(env, g_strdup_printf("TERM=vt100"));
    g_ptr_array_add(env, g_strdup_printf("LANG=en_US.UTF-8"));
    g_ptr_array_add(env, g_strdup_printf("PATH=/usr/local/bin:usr/bin:/bin:/usr/local/sbin:/usr/sbin"));
//    g_ptr_array_add(env, g_strdup_printf ("%sGUESTS=%s", prefix, task->recipe->guests));
    g_list_foreach(task->recipe->params, (GFunc) build_param_var, env);
    g_list_foreach(task->params, (GFunc) build_param_var, env);
    g_ptr_array_add(env, NULL);
    task->env = (gchar **) env->pdata;
    g_ptr_array_free (env, FALSE);
    return TRUE;
}

static void
status_message_complete (SoupSession *session, SoupMessage *server_msg, gpointer user_data)
{
    gchar *status = user_data;
    //task_id = user_data;
    if (!SOUP_STATUS_IS_SUCCESSFUL(server_msg->status_code)) {
        g_warning("Updating status to %s Failed for task, libsoup status %u", status, server_msg->status_code);
        // not much else we can do here...
    }
}

static void
watchdog_message_complete (SoupSession *session, SoupMessage *server_msg, gpointer user_data)
{
    if (!SOUP_STATUS_IS_SUCCESSFUL(server_msg->status_code)) {
        g_warning("Updating watchdog Failed for task, libsoup status %u", server_msg->status_code);
        // depending on the status code we should retry?
        // We should also keep track and report back if we really fail.
    }
}

static void
results_message_complete (SoupSession *session, SoupMessage *server_msg, gpointer user_data)
{
    if (!SOUP_STATUS_IS_SUCCESSFUL(server_msg->status_code)) {
        g_warning("Updating watchdog Failed for task, libsoup status %u", server_msg->status_code);
        // depending on the status code we should retry?
        // We should also keep track and report back if we really fail.
    }
}

static void
task_status (Task *task, gchar *status, GError *reason)
{
    g_return_if_fail(task != NULL);

    SoupURI *task_status_uri;
    SoupMessage *server_msg;

    task_status_uri = soup_uri_new_with_base(task->task_uri, "status");
    server_msg = soup_message_new_from_uri("POST", task_status_uri);

    soup_uri_free(task_status_uri);
    g_return_if_fail(server_msg != NULL);

    gchar *data = NULL;
    if (reason == NULL) {
        // this is basically a bug, but to be nice let's handle it
        g_warning("%s task with no reason given", status);
        data = soup_form_encode("status", status, NULL);
    } else {
        data = soup_form_encode("status", status,
                "message", reason->message, NULL);
        g_message("%s task %s due to error: %s", status, task->task_id, reason->message);
    }
    soup_message_set_request(server_msg, "application/x-www-form-urlencoded",
            SOUP_MEMORY_TAKE, data, strlen(data));

    soup_session_queue_message(soup_session, server_msg, status_message_complete, status);
}

void
restraint_task_abort (Task *task, GError *reason)
{
    task_status (task, "Aborted", reason);
}

void
restraint_task_cancel (Task *task, GError *reason)
{
    task_status (task, "Cancelled", reason);
}

void
restraint_task_watchdog (Task *task, guint seconds)
{
    g_return_if_fail(task != NULL);
    g_return_if_fail(seconds != 0);

    SoupURI *recipe_watchdog_uri;
    SoupMessage *server_msg;
    gchar *data = NULL;

    recipe_watchdog_uri = soup_uri_new_with_base(task->recipe->recipe_uri, "watchdog");
    server_msg = soup_message_new_from_uri("POST", recipe_watchdog_uri);

    soup_uri_free(recipe_watchdog_uri);
    g_return_if_fail(server_msg != NULL);
    data = soup_form_encode("seconds", seconds, NULL);

    soup_message_set_request(server_msg, "application/x-www-form-urlencoded",
            SOUP_MEMORY_TAKE, data, strlen(data));

    soup_session_queue_message(soup_session, server_msg, watchdog_message_complete, NULL);
}

void
restraint_task_result (Task *task, gchar *result, guint score, gchar *path, gchar *message)
{
    /*
     * result, score, path and message
     */
    g_return_if_fail(task != NULL);
    g_return_if_fail(result != NULL);

    SoupURI *task_results_uri;
    SoupMessage *server_msg;

    task_results_uri = soup_uri_new_with_base(task->task_uri, "results");
    server_msg = soup_message_new_from_uri("POST", task_results_uri);

    soup_uri_free(task_results_uri);
    g_return_if_fail(server_msg != NULL);

    gchar *data = NULL;
    GHashTable *data_table = NULL;
    data_table = g_hash_table_new (NULL, NULL);
    g_hash_table_insert (data_table, "result", result);
    if (score)
        g_hash_table_insert (data_table, "score", &score);
    if (path)
        g_hash_table_insert (data_table, "path", path);
    if (message)
        g_hash_table_insert (data_table, "message", message);
    data = soup_form_encode_hash (data_table);

    soup_message_set_request(server_msg, "application/x-www-form-urlencoded",
            SOUP_MEMORY_TAKE, data, strlen(data));

    soup_session_queue_message(soup_session, server_msg, results_message_complete, NULL);
}

Task *restraint_task_new(void) {
    Task *task = g_slice_new0(Task);
    task->max_time = DEFAULT_MAX_TIME;
    task->entry_point = g_strsplit(DEFAULT_ENTRY_POINT, " ", 0);
    return task;
}

void restraint_task_free(Task *task) {
    g_return_if_fail(task != NULL);
    g_free(task->task_id);
    soup_uri_free(task->task_uri);
    g_free(task->name);
    g_free(task->path);
    switch (task->fetch_method) {
        case TASK_FETCH_INSTALL_PACKAGE:
            g_free(task->fetch.package_name);
            break;
        case TASK_FETCH_UNPACK:
            soup_uri_free(task->fetch.url);
            break;
        default:
            g_return_if_reached();
    }
    g_list_free_full(task->params, (GDestroyNotify) restraint_param_free);
    g_list_free_full(task->roles, (GDestroyNotify) restraint_role_free);
    g_strfreev (task->entry_point);
    g_strfreev (task->env);
    g_list_free_full(task->dependencies, (GDestroyNotify) g_free);
    g_slice_free(Task, task);
}

static gboolean
next_task (AppData *app_data, TaskSetupState task_state) {
    Task *task = app_data->tasks->data;
    gboolean result = TRUE;
    app_data->tasks = g_list_next (app_data->tasks);
    if (app_data->tasks) {
       task = (Task *) app_data->tasks->data;
        // Yes, there is a task.
        task->state = task_state;
    } else {
        // No more tasks, let the recipe_handler know we are done.
        app_data->state = RECIPE_COMPLETE;
        app_data->recipe_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                      recipe_handler,
                                                      app_data,
                                                      recipe_finish);
        result = FALSE;
    }
    return result;
}

gboolean
task_handler (gpointer user_data)
{
  AppData *app_data = (AppData *) user_data;
  Task *task = app_data->tasks->data;
  GString *message = g_string_new(NULL);
  gboolean result = TRUE;

  /*
   *  - Fecth the task
   *  - Update metadata
   *  - Build env variables
   *  - Update external Watchdog
   *  - Add localwatchdog timeout
   *  - Install dependencies
   *  - Run task
   *  - Add child pid watcher
   *  - Add io_add_watch on pty output
   */
  switch (task->state) {
    case TASK_IDLE:
      g_string_printf(message, "** Fetching task: %s [%s]\n", task->task_id, task->path);
      task->state = TASK_FETCH;
      break;
    case TASK_FETCH:
      // Fetch Task from rpm or url
      // Only git:// is supported currently.
      if (restraint_task_fetch (app_data, &task->error))
          task->state = TASK_FETCHING;
      else
          task->state = TASK_FAIL;
      break;
    case TASK_METADATA:
      // Update Task metadata
      // entry_point, defaults to "make run"
      // max_time which is used by both localwatchdog and externalwatchdog
      // dependencies required for task execution
      // rhts_compat is set to false if new "metadata" file exists.
      g_string_printf(message, "** Updating metadata\n");
      if (restraint_metadata_update(task, &task->error))
        task->state = TASK_ENV;
      else
        task->state = TASK_FAIL;
      break;
    case TASK_ENV:
      // Build environment to execute task
      // Includes JOBID, TASKID, OSDISTRO, etc..
      // If not running in rhts_compat mode it will prepend
      // the variables with ENV_PREFIX.
      g_string_printf(message, "** Updating env vars\n");
      if (build_env(task, &task->error))
        task->state = TASK_WATCHDOG;
      else
        task->state = TASK_FAIL;
      break;
    case TASK_WATCHDOG:
      // Setup external watchdog
      // Add EWD_TIME to task->max_time and use that for the external watchdog.
      g_string_printf(message, "** Updating watchdog\n");
      task->state = TASK_DEPENDENCIES;
      break;
    case TASK_DEPENDENCIES:
      // Install Task Dependencies
      // All dependencies are installed with system package command
      // All repodependencies are installed via fetch_git
      g_string_printf(message, "** Installing dependencies\n");
      task->state = TASK_RUN;
      break;
    case TASK_RUN:
      // Run TASK
      // Setup pid_handler
      //       pty_handler
      //       timeout_handler
      //       heartbeat_handler
      g_string_printf(message, "** Running task: %s [%s]\n", task->task_id, task->name);
      if (task_run (app_data, &task->error))
        task->state = TASK_RUNNING;
      else
        task->state = TASK_FAIL;
      break;
    case TASK_RUNNING:
        // TASK is running.
        // Remove this idle handler
        // The task_pid_complete will re-add this handler with the 
        //  state being either TASK_FAIL or TASK_COMPLETE
        return FALSE;
    case TASK_FAIL:
      // Some step along the way failed.
      if (task->error) {
        g_warning("%s\n", task->error->message);
        g_string_printf(message, "** ERROR: %s\n", task->error->message);
        restraint_task_abort(task, task->error);
        // Leave the error for now.  We will process these at the recipe level
        // g_error_free(task->error);
      }
      task->state = TASK_COMPLETE;
      break;
    case TASK_CANCELLED:
      g_string_printf(message, "** Cancelling Task : %s\n", task->task_id);
      restraint_task_cancel(task, NULL);
      result = next_task (app_data, TASK_CANCELLED);
      break;
    case TASK_COMPLETE:
      // Task completed so iterate to the next task
      g_string_printf(message, "** Completed Task : %s\n", task->task_id);
      result = next_task (app_data, TASK_IDLE);
      break;
    default:
      return TRUE;
      break;
  }
  if (message->len) {
    connections_write(app_data->connections, message, STREAM_STDERR, 0);
    g_string_free(message, TRUE);
  }
  return result;
}