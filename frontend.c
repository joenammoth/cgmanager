/* frontend.h: definitions of the dbus and scm-enhanced dbus
 *             frontend routines.
 *
 * Copyright © 2013 Stphane Graber
 * Author: Stphane Graber <stgraber@ubuntu.com>
 * Copyright © 2014 Canonical Group Limited
 * Author: Serge Hallyn <serge.hallyn@ubuntu.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define __frontend_c
#include <frontend.h>

int daemonise = FALSE;
bool setns_pid_supported = false;
unsigned long mypidns;
bool setns_user_supported = false;
unsigned long myuserns;

struct scm_sock_data *alloc_scm_sock_data(int fd, enum req_type t)
{
	struct scm_sock_data *d;
	int optval = -1;

	if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"Failed to set passcred: %s", strerror(errno));
		return NULL;
	}
	d = nih_alloc(NULL, sizeof(*d));
	if (!d) {
		nih_dbus_error_raise_printf (DBUS_ERROR_NO_MEMORY,
			"Out of memory");
		return NULL;
	}
	memset(d, 0, sizeof(*d));
	d->fd = fd;
	d->type = t;
	return d;
}

bool need_two_creds(enum req_type t)
{
	switch (t) {
	case REQ_TYPE_GET_PID:
	case REQ_TYPE_MOVE_PID:
	case REQ_TYPE_CHOWN:
		return true;
	default:
		return false;
	}
}

void scm_sock_error_handler (void *data, NihIo *io)
{
	struct scm_sock_data *d = data;
	NihError *error = nih_error_get ();
	nih_error("got an error, type %d", d->type);
	nih_error("error %s", strerror(error->number));
	nih_free(error);
}

void scm_sock_close (struct scm_sock_data *data, NihIo *io)
{
	nih_assert (data);
	nih_assert (io);
	close (data->fd);
	nih_free (data);
	nih_free (io);
}

bool kick_fd_client(int fd)
{
	char buf = '1';
	if (write(fd, &buf, 1) != 1) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to start write on scm fd: %s", strerror(errno));
		return false;
	}
	return true;
}

void sock_scm_reader(struct scm_sock_data *data,
			NihIo *io, const char *buf, size_t len)
{
	struct ucred ucred;

	if (!get_nih_io_creds(io, &ucred)) {
		nih_error("failed to read ucred");
		nih_io_shutdown(io);
		return;
	}
	if (data->step == 0) {
		memcpy(&data->rcred, &ucred, sizeof(struct ucred));
		if (need_two_creds(data->type)) {
			data->step = 1;
			if (!kick_fd_client(data->fd))
				nih_io_shutdown(io);
			return;
		}
	} else
		memcpy(&data->vcred, &ucred, sizeof(struct ucred));

	switch (data->type) {
	case REQ_TYPE_GET_PID: get_pid_scm_complete(data); break;
	case REQ_TYPE_MOVE_PID: move_pid_scm_complete(data); break;
	case REQ_TYPE_CREATE: create_scm_complete(data); break;
	case REQ_TYPE_CHOWN: chown_scm_complete(data); break;
	case REQ_TYPE_GET_VALUE: get_value_complete(data); break;
	case REQ_TYPE_SET_VALUE: set_value_complete(data); break;
	case REQ_TYPE_REMOVE: remove_scm_complete(data); break;
	case REQ_TYPE_GET_TASKS: get_tasks_scm_complete(data); break;
	default:
		nih_fatal("%s: bad req_type %d", __func__, data->type);
		exit(1);
	}
	nih_io_shutdown(io);
}

int cgmanager_ping (void *data, NihDBusMessage *message, int junk)
{
	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	return 0;
}

void get_pid_scm_complete(struct scm_sock_data *data)
{
	char *output = NULL;
	int ret;

	ret = get_pid_cgroup_main(data, data->controller, data->rcred,
				data->vcred, &output);
	if (ret == 0)
		ret = write(data->fd, output, strlen(output)+1);
	else
		ret = write(data->fd, &data->rcred, 0);  // kick the client
	if (ret < 0)
		nih_error("GetPidCgroupScm: Error writing final result to client: %s",
			strerror(errno));
}

/*
 * This is one of the dbus callbacks.
 * Caller requests the cgroup of @pid in a given @controller
 */
int cgmanager_get_pid_cgroup_scm (void *data, NihDBusMessage *message,
			const char *controller, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_GET_PID);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}

	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

/* GetPidCgroup */
/*
 * This is one of the dbus callbacks.
 * Caller requests the cgroup of @pid in a given @controller
 */
int cgmanager_get_pid_cgroup (void *data, NihDBusMessage *message,
			const char *controller, int plain_pid, char **output)
{
	int fd = 0, ret;
	struct ucred rcred, vcred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &rcred, &len) != -1);

	nih_info (_("GetPidCgroup: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, rcred.pid, rcred.uid, rcred.gid);

	// XXX can we safely ignore this?
	if (!setns_pid_supported) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"kernel too old, use GetPidCgroupScm");
		return -1;
	}
	if (!is_same_pidns(rcred.pid)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"GetPidCgroup called from non-init namespace");
		return -1;
	}
	vcred.uid = 0;
	vcred.gid = 0;
	vcred.pid = plain_pid;
	ret = get_pid_cgroup_main(message, controller, rcred, vcred, output);
	if (ret) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"invalid request");
		return -1;
	}
	return 0;
}

void move_pid_scm_complete(struct scm_sock_data *data)
{
	char b = '0';

	if (move_pid_main(data->controller, data->cgroup, data->rcred,
			  data->vcred) == 0)
		b = '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("MovePidScm: Error writing final result to client");
}

int cgmanager_move_pid_scm (void *data, NihDBusMessage *message,
			const char *controller, const char *cgroup,
			int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_MOVE_PID);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

/*
 * This is one of the dbus callbacks.
 * Caller requests moving a @pid to a particular cgroup identified
 * by the name (@cgroup) and controller type (@controller).
 */
int cgmanager_move_pid (void *data, NihDBusMessage *message,
			const char *controller, const char *cgroup, int plain_pid)
{
	int fd = 0, ret;
	struct ucred ucred, vcred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("MovePid: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	vcred.uid = 0;
	vcred.gid = 0;
	vcred.pid = plain_pid;
	ret = move_pid_main(controller, cgroup, ucred, vcred);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

void create_scm_complete(struct scm_sock_data *data)
{
	char b = '0';
	int32_t existed;

	if (create_main(data->controller, data->cgroup, data->rcred, &existed) == 0)
		b = existed == 1 ? '2' : '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("createScm: Error writing final result to client");
}

int cgmanager_create_scm (void *data, NihDBusMessage *message,
		 const char *controller, const char *cgroup, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_CREATE);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

/* 
 * This is one of the dbus callbacks.
 * Caller requests creating a new @cgroup name of type @controller.
 * @name is taken to be relative to the caller's cgroup and may not
 * start with / or .. .
 */
int cgmanager_create (void *data, NihDBusMessage *message,
			 const char *controller, const char *cgroup, int32_t *existed)
{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;

	*existed = -1;
	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("Create: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = create_main(controller, cgroup, ucred, existed);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"invalid request");
	nih_info(_("%s: returning %d; existed is %d"), __func__, ret, *existed);
	return ret;
}

void chown_scm_complete(struct scm_sock_data *data)
{
	char b = '0';

	if (chown_main(data->controller, data->cgroup, data->rcred, data->vcred)
			== 0)
		b = '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("ChownScm: Error writing final result to client");
}

int cgmanager_chown_scm (void *data, NihDBusMessage *message,
			const char *controller, const char *cgroup, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_CHOWN);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader)  sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

/*
 * This is one of the dbus callbacks.
 * Caller requests chowning a cgroup @name in controller @cgroup to a
 * particular @uid.  The uid must be passed in as an scm_cred so the
 * kernel translates it for us.  @r must be root in its own user ns.
 *
 * If we are asked to chown /b to UID, then we will chown:
 * /b itself, /b/tasks, and /b/procs.  Any other files in /b will not be
 * chown.  UID can then create subdirs of /b, but not raise his limits.
 */
int cgmanager_chown (void *data, NihDBusMessage *message,
			const char *controller, const char *cgroup, int uid, int gid)
{
	int fd = 0, ret;
	struct ucred ucred, vcred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("Chown: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	// XXX what are the ramifications if we ignore this?
	if (!setns_pid_supported || !setns_user_supported) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"kernel too old, use ChownScm");
		return -1;
	}
	if (!is_same_pidns(ucred.pid)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"chown called from different pid namespace");
		return -1;
	}
	if (!is_same_userns(ucred.pid)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"chown called from different user namespace");
		return -1;
	}

	vcred.pid = 0;
	vcred.uid = uid;
	vcred.gid = gid;

	ret = chown_main(controller, cgroup, ucred, vcred);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

void get_value_complete(struct scm_sock_data *data)
{
	char *output = NULL;
	int ret;

	if (!get_value_main(data, data->controller, data->cgroup, data->key,
			data->rcred, &output))
		ret = write(data->fd, output, strlen(output)+1);
	else
		ret = write(data->fd, &data->rcred, 0);  // kick the client
	if (ret < 0)
		nih_error("GetValueScm: Error writing final result to client");
}

int cgmanager_get_value_scm (void *data, NihDBusMessage *message,
				 const char *controller, const char *req_cgroup,
				 const char *key, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_GET_VALUE);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, req_cgroup);
	d->key = nih_strdup(d, key);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;

}

/* 
 * This is one of the dbus callbacks.
 * Caller requests the value of a particular cgroup file.
 * @controller is the controller, @req_cgroup the cgroup name, and @key the
 * file being queried (i.e. memory.usage_in_bytes).  @req_cgroup is relative
 * to the caller's cgroup, unless it begins with '/' or '..'.
 *
 * XXX Should '/' be disallowed, only '..' allowed?  Otherwise callers can't
 * pretend to be the cgroup root which is annoying in itself
 */
int cgmanager_get_value (void *data, NihDBusMessage *message,
				 const char *controller, const char *req_cgroup,
				 const char *key, char **value)

{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Message was NULL");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("GetValue: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = get_value_main(message, controller, req_cgroup, key, ucred, value);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"invalid request");
	return ret;
}

void set_value_complete(struct scm_sock_data *data)
{
	char b = '0';
	if (set_value_main(data->controller, data->cgroup, data->key,
			data->value, data->rcred) == 0)
		b = '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("SetValueScm: Error writing final result to client");
}

int cgmanager_set_value_scm (void *data, NihDBusMessage *message,
				 const char *controller, const char *req_cgroup,
				 const char *key, const char *value, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_SET_VALUE);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, req_cgroup);
	d->key = nih_strdup(d, key);
	d->value = nih_strdup(d, value);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

/* 
 * This is one of the dbus callbacks.
 * Caller requests that a particular cgroup @key be set to @value
 * @controller is the controller, @req_cgroup the cgroup name, and @key the
 * file being queried (i.e. memory.usage_in_bytes).  @req_cgroup is relative
 * to the caller's cgroup.
 */
int cgmanager_set_value (void *data, NihDBusMessage *message,
				 const char *controller, const char *req_cgroup,
				 const char *key, const char *value)

{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Message was NULL");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("SetValue: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = set_value_main(controller, req_cgroup, key, value, ucred);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

void remove_scm_complete(struct scm_sock_data *data)
{
	char b = '0';
	int ret;
	int32_t existed = -1;

	ret = remove_main(data->controller, data->cgroup, data->rcred,
			data->recursive, &existed);
	if (ret == 0)
		b = existed == 1 ? '2' : '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("removeScm: Error writing final result to client");
}

int cgmanager_remove_scm (void *data, NihDBusMessage *message,
		 const char *controller, const char *cgroup, int recursive, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_REMOVE);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);
	d->recursive = recursive;

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

/* 
 * This is one of the dbus callbacks.
 * Caller requests creating a new @cgroup name of type @controller.
 * @name is taken to be relative to the caller's cgroup and may not
 * start with / or .. .
 */
int cgmanager_remove (void *data, NihDBusMessage *message, const char *controller,
			const char *cgroup, int recursive, int32_t *existed)
{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;

	*existed = -1;
	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("Remove: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = remove_main(controller, cgroup, ucred, recursive, existed);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

void get_tasks_scm_complete(struct scm_sock_data *data)
{
	struct ucred pcred;
	int i, ret;
	int32_t *pids, nrpids;
	ret = get_tasks_main(data, data->controller, data->cgroup,
			data->rcred, &pids);
	if (ret < 0) {
		nih_error("Error getting nrtasks for %s:%s for pid %d",
			data->controller, data->cgroup, data->rcred.pid);
		return;
	}
	nrpids = ret;
	if (write(data->fd, &nrpids, sizeof(int32_t)) != sizeof(int32_t)) {
		nih_error("get_tasks_scm: Error writing final result to client");
		return;
	}
	pcred.uid = 0; pcred.gid = 0;
	for (i=0; i<ret; i++) {
		pcred.pid = pids[i];
		if (send_creds(data->fd, &pcred)) {
			nih_error("get_tasks_scm: error writing pids back to client");
			return;
		}
	}
}

int cgmanager_get_tasks_scm (void *data, NihDBusMessage *message,
		 const char *controller, const char *cgroup, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_GET_TASKS);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

/* 
 * This is one of the dbus callbacks.
 * Caller requests the number of tasks in @cgroup in @controller
 * returns nrpids, or -1 on error.
 */
int cgmanager_get_tasks (void *data, NihDBusMessage *message, const char *controller,
			const char *cgroup, int32_t **pids, size_t *nrpids)
{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;
	int32_t *tmp;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("GetTasks: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = get_tasks_main(message, controller, cgroup, ucred, &tmp);
	if (ret >= 0) {
		*nrpids = ret;
		*pids = tmp;
		ret = 0;
	} else
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

static dbus_bool_t allow_user(DBusConnection *connection, unsigned long uid, void *data)
{
	return TRUE;
}

int client_connect (DBusServer *server, DBusConnection *conn)
{
	if (server == NULL || conn == NULL)
		return FALSE;

	dbus_connection_set_unix_user_function(conn, allow_user, NULL, NULL);
	dbus_connection_set_allow_anonymous(conn, TRUE);

	nih_info (_("Connection from private client"));

	NIH_MUST (nih_dbus_object_new (NULL, conn,
				"/org/linuxcontainers/cgmanager",
				cgmanager_interfaces, NULL));

	return TRUE;
}

void client_disconnect (DBusConnection *conn)
{
	if (conn == NULL)
		return;

	nih_info (_("Disconnected from private client"));
}
