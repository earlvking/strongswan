/*
 * Copyright (C) 2012 Martin Willi
 * Copyright (C) 2012 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "lookip_socket.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

#include <daemon.h>
#include <threading/thread.h>
#include <threading/mutex.h>
#include <utils/linked_list.h>
#include <processing/jobs/callback_job.h>

#include "lookip_msg.h"

typedef struct private_lookip_socket_t private_lookip_socket_t;

/**
 * Private data of an lookip_socket_t object.
 */
struct private_lookip_socket_t {

	/**
	 * Public lookip_socket_t interface.
	 */
	lookip_socket_t public;

	/**
	 * lookip
	 */
	lookip_listener_t *listener;

	/**
	 * lookip unix socket file descriptor
	 */
	int socket;

	/**
	 * List of registered listeners, as entry_t
	 */
	linked_list_t *clients;

	/**
	 * Mutex to lock clients list
	 */
	mutex_t *mutex;
};

/**
 * Open lookip unix socket
 */
static bool open_socket(private_lookip_socket_t *this)
{
	struct sockaddr_un addr;
	mode_t old;

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, LOOKIP_SOCKET);

	this->socket = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (this->socket == -1)
	{
		DBG1(DBG_CFG, "creating lookip socket failed");
		return FALSE;
	}
	unlink(addr.sun_path);
	old = umask(~(S_IRWXU | S_IRWXG));
	if (bind(this->socket, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		DBG1(DBG_CFG, "binding lookip socket failed: %s", strerror(errno));
		close(this->socket);
		return FALSE;
	}
	umask(old);
	if (chown(addr.sun_path, charon->caps->get_uid(charon->caps),
			  charon->caps->get_gid(charon->caps)) != 0)
	{
		DBG1(DBG_CFG, "changing lookip socket permissions failed: %s",
			 strerror(errno));
	}
	if (listen(this->socket, 10) < 0)
	{
		DBG1(DBG_CFG, "listening on lookip socket failed: %s", strerror(errno));
		close(this->socket);
		unlink(addr.sun_path);
		return FALSE;
	}
	return TRUE;
}

/**
 * Listener callback entry
 */
typedef struct {
	/* FD to write to */
	int fd;
	/* message type to send */
	int type;
	/* back pointer to socket, only for subscriptions */
	private_lookip_socket_t *this;
} entry_t;

/**
 * Destroy entry
 */
static void entry_destroy(entry_t *this)
{
	close(this->fd);
	free(this);
}

/**
 * Callback function for listener
 */
static bool listener_cb(entry_t *entry, bool up, host_t *vip,
						host_t *other, identification_t *id, char *name)
{
	lookip_response_t resp = {
		.type = entry->type,
	};

	/* filter events */
	if (up && entry->type == LOOKIP_NOTIFY_DOWN)
	{
		return TRUE;
	}
	if (!up && entry->type == LOOKIP_NOTIFY_UP)
	{
		return TRUE;
	}

	snprintf(resp.vip, sizeof(resp.vip), "%H", vip);
	snprintf(resp.ip, sizeof(resp.ip), "%H", other);
	snprintf(resp.id, sizeof(resp.id), "%Y", id);
	snprintf(resp.name, sizeof(resp.name), "%s", name);

	switch (send(entry->fd, &resp, sizeof(resp), 0))
	{
		case sizeof(resp):
			return TRUE;
		case 0:
			/* client disconnected, adios */
			break;
		default:
			DBG1(DBG_CFG, "sending lookip response failed: %s", strerror(errno));
			break;
	}
	if (entry->this)
	{	/* unregister listener */
		entry->this->mutex->lock(entry->this->mutex);
		entry->this->clients->remove(entry->this->clients, entry, NULL);
		entry->this->mutex->unlock(entry->this->mutex);

		entry_destroy(entry);
	}
	return FALSE;
}

/**
 * Perform a entry lookup
 */
static void query(private_lookip_socket_t *this, int fd, lookip_request_t *req)
{
	entry_t entry = {
		.fd = fd,
		.type = LOOKIP_ENTRY,
	};
	host_t *vip = NULL;

	if (req)
	{	/* lookup */
		req->vip[sizeof(req->vip) - 1] = 0;
		vip = host_create_from_string(req->vip, 0);
		if (vip)
		{
			this->listener->lookup(this->listener, vip,
								   (void*)listener_cb, &entry);
			vip->destroy(vip);
		}
	}
	else
	{	/* dump */
		this->listener->lookup(this->listener, NULL,
							   (void*)listener_cb, &entry);
	}
}

/**
 * Subscribe to virtual IP events
 */
static void subscribe(private_lookip_socket_t *this, int fd, int type)
{
	entry_t *entry;

	INIT(entry,
		.fd = fd,
		.type = type,
		.this = this,
	);

	this->mutex->lock(this->mutex);
	this->clients->insert_last(this->clients, entry);
	this->mutex->unlock(this->mutex);

	this->listener->add_listener(this->listener, (void*)listener_cb, entry);
}

/**
 * Accept client connections, dispatch
 */
static job_requeue_t receive(private_lookip_socket_t *this)
{
	struct sockaddr_un addr;
	int fd, len = sizeof(addr);
	lookip_request_t req;
	bool oldstate, subscribed = FALSE;

	oldstate = thread_cancelability(TRUE);
	fd = accept(this->socket, (struct sockaddr*)&addr, &len);
	thread_cancelability(oldstate);

	if (fd != -1)
	{
		while (TRUE)
		{
			oldstate = thread_cancelability(TRUE);
			len = recv(fd, &req, sizeof(req), 0);
			thread_cancelability(oldstate);

			if (len == sizeof(req))
			{
				switch (req.type)
				{
					case LOOKIP_LOOKUP:
						query(this, fd, &req);
						continue;
					case LOOKIP_DUMP:
						query(this, fd, NULL);
						continue;
					case LOOKIP_REGISTER_UP:
						subscribe(this, fd, LOOKIP_NOTIFY_UP);
						subscribed = TRUE;
						continue;
					case LOOKIP_REGISTER_DOWN:
						subscribe(this, fd, LOOKIP_NOTIFY_DOWN);
						subscribed = TRUE;
						continue;
					case LOOKIP_END:
						break;
					default:
						DBG1(DBG_CFG, "received unknown lookip command");
						break;
				}
			}
			else
			{
				if (len != 0)
				{
					DBG1(DBG_CFG, "receiving lookip request failed: %s",
						 strerror(errno));
				}
				break;
			}
			break;
		}
		if (!subscribed)
		{	/* don't close if we queued the fd */
			close(fd);
		}
	}
	else
	{
		DBG1(DBG_CFG, "accepting lookip connection failed: %s",
			 strerror(errno));
	}
	return JOB_REQUEUE_FAIR;
}

METHOD(lookip_socket_t, destroy, void,
	private_lookip_socket_t *this)
{
	this->clients->destroy_function(this->clients, (void*)entry_destroy);
	this->mutex->destroy(this->mutex);
	close(this->socket);
	free(this);
}

/**
 * See header
 */
lookip_socket_t *lookip_socket_create(lookip_listener_t *listener)
{
	private_lookip_socket_t *this;

	INIT(this,
		.public = {
			.destroy = _destroy,
		},
		.listener = listener,
		.clients = linked_list_create(),
		.mutex = mutex_create(MUTEX_TYPE_DEFAULT),
	);

	if (!open_socket(this))
	{
		free(this);
		return NULL;
	}

	lib->processor->queue_job(lib->processor,
		(job_t*)callback_job_create_with_prio((callback_job_cb_t)receive, this,
				NULL, (callback_job_cancel_t)return_false, JOB_PRIO_CRITICAL));

	return &this->public;
}