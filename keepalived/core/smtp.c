/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        SMTP WRAPPER connect to a specified smtp server and send mail
 *              using the smtp protocol according to the RFC 821. A non blocking
 *              timeouted connection is used to handle smtp protocol.
 *
 * Version:     $Id: smtp.c,v 0.6.5 2002/07/01 23:41:28 acassen Exp $
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 */

#include "smtp.h"
#include "memory.h"
#include "list.h"
#include "utils.h"

extern data *conf_data;

/* static prototype */
static int smtp_send_cmd_thread(thread *);

static void
free_smtp_all(smtp_thread_arg * smtp_arg)
{
	FREE(smtp_arg->buffer);
	FREE(smtp_arg->subject);
	FREE(smtp_arg->body);
	FREE(smtp_arg);
}

static char *
fetch_next_email(smtp_thread_arg * smtp_arg)
{
	return list_element(conf_data->email, smtp_arg->email_it);
}

static int
smtp_read_cmd_thread(thread * thread)
{
	smtp_thread_arg *smtp_arg;
	char *fetched_email;
	char *buffer;
	char *reply;
	int rcv_buffer_size = 0;
	int status = -1;

	smtp_arg = THREAD_ARG(thread);

	if (thread->type == THREAD_READ_TIMEOUT) {
		DBG("Timeout reading data to remote SMTP server [%s:%d].",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		free_smtp_all(smtp_arg);
		close(thread->u.fd);
		return 0;
	}

	buffer = smtp_arg->buffer;

	while ((rcv_buffer_size =
		read(thread->u.fd, buffer + smtp_arg->buflen,
		     SMTP_BUFFER_LENGTH - smtp_arg->buflen)) != 0) {
		if (rcv_buffer_size == -1) {
			if (errno == EAGAIN)
				goto end;
			DBG("Error reading data to remote SMTP server [%s:%d].",
			    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
			free_smtp_all(smtp_arg);
			close(thread->u.fd);
			return 0;
		}

		/* received data overflow buffer size ? */
		if (smtp_arg->buflen >= SMTP_BUFFER_MAX) {
			DBG("Received buffer from remote SMTP server [%s:%d]"
			    " overflow our get read buffer length.",
			    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
			free_smtp_all(smtp_arg);
			close(thread->u.fd);
			return 0;
		} else {
			smtp_arg->buflen += rcv_buffer_size;
			buffer[smtp_arg->buflen] = 0;	/* NULL terminate */
			if (rcv_buffer_size < SMTP_BUFFER_LENGTH)
				goto end;
		}
	}

      end:

// printf("Received : %s", buffer);

	/* parse the buffer, finding the last line of the response for the code */
	reply = buffer;
	while (reply < buffer + smtp_arg->buflen) {
		char *p;

		p = strstr(reply, "\r\n");
		if (!p) {
			memmove(buffer, reply,
				smtp_arg->buflen - (reply - buffer));
			smtp_arg->buflen -= (reply - buffer);
			buffer[smtp_arg->buflen] = 0;

			thread_add_read(thread->master, smtp_read_cmd_thread,
					smtp_arg, thread->u.fd,
					conf_data->smtp_connection_to);
			return 0;
		}

		if (reply[3] == '-') {
			/* Skip over the \r\n */
			reply = p + 2;
			continue;
		}

		status =
		    ((reply[0] - '0') * 100) + ((reply[1] - '0') * 10) +
		    (reply[2] - '0');

		reply = p + 2;
		break;
	}

	memmove(buffer, reply, smtp_arg->buflen - (reply - buffer));
	smtp_arg->buflen -= (reply - buffer);
	buffer[smtp_arg->buflen] = 0;

	if (status == -1) {
		thread_add_read(thread->master, smtp_read_cmd_thread, smtp_arg,
				thread->u.fd, conf_data->smtp_connection_to);
		return 0;
	}

	/* setting the next stage */
	switch (smtp_arg->stage) {
	case CONNECTION:
		if (status == 220) {
			smtp_arg->stage = HELO;
		} else {
			DBG("Error connecting smtp server : [%s]",
			    buffer);
			smtp_arg->stage = ERROR;
		}
		break;

	case HELO:
		if (status == 250) {
			smtp_arg->stage = MAIL;
		} else {
			DBG("Error processing HELO cmd : [%s]",
			    buffer);
			smtp_arg->stage = ERROR;
		}
		break;

	case MAIL:
		if (status == 250) {
			smtp_arg->stage = RCPT;
		} else {
			DBG("Error processing MAIL FROM cmd : [%s]",
			    buffer);
			smtp_arg->stage = ERROR;
		}
		break;

	case RCPT:
		if (status == 250) {
			smtp_arg->email_it++;

			fetched_email = fetch_next_email(smtp_arg);

			if (!fetched_email)
				smtp_arg->stage = DATA;
		} else {
			DBG("Error processing RCPT TO cmd : [%s]",
			    buffer);
			smtp_arg->stage = ERROR;
		}
		break;

	case DATA:
		if (status == 354) {
			smtp_arg->stage = BODY;
		} else {
			DBG("Error processing DATA cmd : [%s]",
			    buffer);
			smtp_arg->stage = ERROR;
		}
		break;

	case BODY:
		if (status == 250) {
			smtp_arg->stage = QUIT;
			syslog(LOG_INFO, "SMTP alert successfully sent.");
		} else {
			DBG("Error processing DOT cmd : [%s]",
			    buffer);
			smtp_arg->stage = ERROR;
		}
		break;

	case QUIT:
		/* final state, we are disconnected from the remote host */
		free_smtp_all(smtp_arg);
		close(thread->u.fd);
		return 0;

	case ERROR:
		break;
	}

	/* Registering next smtp command processing thread */
	thread_add_write(thread->master, smtp_send_cmd_thread, smtp_arg,
			 thread->u.fd, conf_data->smtp_connection_to);
	return 0;
}

/* Getting localhost official canonical name */
static char *
get_local_name(void)
{
	struct hostent *host;
	struct utsname name;

	if (uname(&name) < 0)
		return NULL;

	if (!(host = gethostbyname(name.nodename)))
		return NULL;

	return host->h_name;
}

static int
smtp_send_cmd_thread(thread * thread)
{
	smtp_thread_arg *smtp_arg;
	char *fetched_email;
	char *buffer;

	smtp_arg = THREAD_ARG(thread);

	if (thread->type == THREAD_WRITE_TIMEOUT) {
		DBG("Timeout sending data to remote SMTP server [%s:%d].",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		free_smtp_all(smtp_arg);
		close(thread->u.fd);
		return 0;
	}

	/* allocate temporary command buffer */
	buffer = (char *) MALLOC(SMTP_BUFFER_MAX);

	switch (smtp_arg->stage) {
	case CONNECTION:
		break;

	case HELO:
		snprintf(buffer, SMTP_BUFFER_MAX, SMTP_HELO_CMD,
			 get_local_name());
		if (send(thread->u.fd, buffer, strlen(buffer), 0) == -1)
			smtp_arg->stage = ERROR;
		break;

	case MAIL:
		snprintf(buffer, SMTP_BUFFER_MAX, SMTP_MAIL_CMD,
			 conf_data->email_from);
		if (send(thread->u.fd, buffer, strlen(buffer), 0) == -1)
			smtp_arg->stage = ERROR;
		break;

	case RCPT:
		/* We send RCPT TO command multiple time to add all our email receivers.
		 * --rfc821.3.1
		 */
		fetched_email = fetch_next_email(smtp_arg);

		snprintf(buffer, SMTP_BUFFER_MAX, SMTP_RCPT_CMD, fetched_email);
		if (send(thread->u.fd, buffer, strlen(buffer), 0) == -1)
			smtp_arg->stage = ERROR;
		break;

	case DATA:
		if (send(thread->u.fd, SMTP_DATA_CMD, strlen(SMTP_DATA_CMD), 0)
		    == -1)
			smtp_arg->stage = ERROR;
		break;

	case BODY:
		snprintf(buffer, SMTP_BUFFER_MAX, SMTP_HEADERS_CMD,
			 conf_data->email_from, smtp_arg->subject);
		/* send the subject field */
		if (send(thread->u.fd, buffer, strlen(buffer), 0) == -1)
			smtp_arg->stage = ERROR;

		memset(buffer, 0, SMTP_BUFFER_MAX);
		snprintf(buffer, SMTP_BUFFER_MAX, SMTP_BODY_CMD,
			 smtp_arg->body);
		/* send the the body field */
		if (send(thread->u.fd, buffer, strlen(buffer), 0) == -1)
			smtp_arg->stage = ERROR;

		/* send the sending dot */
		if (send(thread->u.fd, SMTP_SEND_CMD, strlen(SMTP_SEND_CMD), 0)
		    == -1)
			smtp_arg->stage = ERROR;
		break;

	case QUIT:
		if (send(thread->u.fd, SMTP_QUIT_CMD, strlen(SMTP_QUIT_CMD), 0)
		    == -1)
			smtp_arg->stage = ERROR;
		break;

	case ERROR:
		DBG("Can not send data to remote SMTP server [%s:%d].",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		/* we just cleanup the room */
		free_smtp_all(smtp_arg);
		close(thread->u.fd);
		FREE(buffer);
		return 0;
		break;
	}

// printf("Sending : %s", buffer);

	/* Registering next smtp command processing thread */
	thread_add_read(thread->master, smtp_read_cmd_thread, smtp_arg,
			thread->u.fd, conf_data->smtp_connection_to);

	FREE(buffer);
	return 0;
}

/* SMTP checkers threads */
static int
smtp_check_thread(thread * thread)
{
	smtp_thread_arg *smtp_arg;
	int status;

	smtp_arg = THREAD_ARG(thread);

	status =
	    tcp_socket_state(thread->u.fd, thread, conf_data->smtp_server,
			     htons(SMTP_PORT)
			     , smtp_check_thread);

	switch (status) {
	case connect_error:
		DBG("Error connecting SMTP server [%s:%d].",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		free_smtp_all(smtp_arg);
		break;

	case connect_timeout:
		DBG("Timeout writing data to SMTP server [%s:%d].",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		free_smtp_all(smtp_arg);
		break;

	case connect_success:
		/* Remote SMTP server is connected.
		 * Register the next step thread smtp_cmd_thread.
		 */
		DBG("Remote SMTP server [%s:%d] connected.",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		thread_add_write(thread->master, smtp_send_cmd_thread, smtp_arg,
				 thread->u.fd, conf_data->smtp_connection_to);
		break;
	}

	return 0;
}

static int
smtp_connect_thread(thread * thread)
{
	smtp_thread_arg *smtp_arg;
	enum connect_result status;
	int fd;

	smtp_arg = THREAD_ARG(thread);

	/* Return if no smtp server is defined */
	if (conf_data->smtp_server == 0) {
		free_smtp_all(smtp_arg);
		return 0;
	}

	if ((fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		DBG("SMTP connect fail to create socket.");
		return 0;
	}

	status = tcp_connect(fd, conf_data->smtp_server, htons(SMTP_PORT));

	switch (status) {
	case connect_error:
		DBG("SMTP connection ERROR to [%s:%d].",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		free_smtp_all(smtp_arg);
		close(fd);
		return 0;
		break;

	case connect_timeout:
		DBG("Timeout connecting SMTP server [%s:%d].",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		free_smtp_all(smtp_arg);
		close(fd);
		return 0;
		break;

	case connect_success:
		DBG("SMTP connection SUCCESS to [%s:%d].",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		break;

		/* Checking non-blocking connect, we wait until socket is writable */
	case connect_in_progress:
		DBG("SMTP connection to [%s:%d] now IN_PROGRESS.",
		    inet_ntop2(conf_data->smtp_server), SMTP_PORT);
		break;
	}

	/* connection have succeeded or still in progress */
	thread_add_write(thread->master, smtp_check_thread, smtp_arg, fd,
			 conf_data->smtp_connection_to);
	return 1;
}

void
smtp_alert(thread_master * master, real_server * rs, vrrp_rt * vrrp,
	   const char *subject, const char *body)
{
	smtp_thread_arg *smtp_arg;

	/* Only send mail if email specified */
	if (!LIST_ISEMPTY(conf_data->email)) {
		/* allocate & initialize smtp argument data structure */
		smtp_arg = (smtp_thread_arg *) MALLOC(sizeof (smtp_thread_arg));
		smtp_arg->subject = (char *) MALLOC(MAX_HEADERS_LENGTH);
		smtp_arg->body = (char *) MALLOC(MAX_BODY_LENGTH);
		smtp_arg->buffer = (char *) MALLOC(SMTP_BUFFER_MAX);

		smtp_arg->stage = CONNECTION;	/* first smtp command set to HELO */

		/* format subject if rserver is specified */
		if (rs)
			snprintf(smtp_arg->subject, MAX_HEADERS_LENGTH,
				 "[%s] Realserver %s:%d - %s",
				 conf_data->lvs_id, inet_ntop2(SVR_IP(rs))
				 , ntohs(SVR_PORT(rs))
				 , subject);
		else if (vrrp)
			snprintf(smtp_arg->subject, MAX_HEADERS_LENGTH,
				 "[%s] VRRP Instance %s - %s",
				 conf_data->lvs_id, vrrp->iname, subject);
		else if (conf_data->lvs_id)
			snprintf(smtp_arg->subject, MAX_HEADERS_LENGTH,
				 "[%s] %s", conf_data->lvs_id, subject);
		else
			snprintf(smtp_arg->subject, MAX_HEADERS_LENGTH, "%s",
				 subject);

		strncpy(smtp_arg->body, body, MAX_BODY_LENGTH);

		thread_add_event(master, smtp_connect_thread, smtp_arg, 0);
	}
}
