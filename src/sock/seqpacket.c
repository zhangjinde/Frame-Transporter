#include "../_ft_internal.h"

// TODO: Conceptual: Consider implementing readv() and writev() versions for more complicated frame formats (eh, on linux it is implemented as memcpy)
// TODO: SO_KEEPALIVE
// TODO: SO_LINGER (in a non-blocking mode?, valid for SEQPACKET?)
// TODO: SO_PRIORITY
// TODO: SO_RCVBUF
// TODO: SO_SNDBUF (??)
// TODO: SO_RCVLOWAT (consider using)
// TODO: SO_RCVTIMEO & SO_SNDTIMEO

static void _ft_seqpacket_on_read(struct ev_loop * loop, struct ev_io * watcher, int revents);
static void _ft_seqpacket_on_write(struct ev_loop * loop, struct ev_io * watcher, int revents);

enum _ft_seqpacket_read_event
{
	READ_WANT_READ = 1,
};

enum _ft_seqpacket_write_event
{
	WRITE_WANT_WRITE = 1,
	CONNECT_WANT_WRITE = 16,
};

static void _ft_seqpacket_read_set_event(struct ft_seqpacket * this, enum _ft_seqpacket_read_event event);
static void _ft_seqpacket_read_unset_event(struct ft_seqpacket * this, enum _ft_seqpacket_read_event event);
static void _ft_seqpacket_write_set_event(struct ft_seqpacket * this, enum _ft_seqpacket_write_event event);
static void _ft_seqpacket_write_unset_event(struct ft_seqpacket * this, enum _ft_seqpacket_write_event event);

static void _ft_seqpacket_on_write_event(struct ft_seqpacket * this);

///

#define TRACE_FMT "fd:%d Rs:%c Rp:%c Rt:%c Re:%c Rw:%c Ws:%c Wo:%c Wr:%c We:%c%c Ww:%c c:%c a:%c E:(%d)"
#define TRACE_ARGS \
	this->read_watcher.fd, \
	(this->flags.read_shutdown) ? 'Y' : '.', \
	(this->flags.read_partial) ? 'Y' : '.', \
	(this->flags.read_throttle) ? 'Y' : '.', \
	(this->read_events & READ_WANT_READ) ? 'R' : '.', \
	(ev_is_active(&this->read_watcher)) ? 'Y' : '.', \
	(this->flags.write_shutdown) ? 'Y' : '.', \
	(this->flags.write_open) ? 'Y' : '.', \
	(this->flags.write_ready) ? 'Y' : '.', \
	(this->write_events & WRITE_WANT_WRITE) ? 'W' : '.', \
	(this->write_events & CONNECT_WANT_WRITE) ? 'c' : '.', \
	(ev_is_active(&this->write_watcher)) ? 'Y' : '.', \
	(this->flags.connecting) ? 'Y' : '.', \
	(this->flags.active) ? 'Y' : '.', \
	this->error.sys_errno

///

static bool _ft_seqpacket_init(struct ft_seqpacket * this, struct ft_seqpacket_delegate * delegate, struct ft_context * context, int fd, const struct sockaddr * peer_addr, socklen_t peer_addr_len, int ai_family, int ai_socktype, int ai_protocol)
{
	assert(this != NULL);
	assert(delegate != NULL);
	assert(context != NULL);

	//TODO: This: assert(ai_socktype == SOCK_STREAM);

	if ((ai_family == AF_INET) || (ai_family == AF_INET6))
	{
	// Disable Nagle
#ifdef TCP_NODELAY
		int flag = 1;
		int rc_tcp_nodelay = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag) );
		if (rc_tcp_nodelay == -1) FT_WARN_ERRNO(errno, "Couldn't setsockopt on client connection (TCP_NODELAY)");
#endif

	// Set Congestion Window size
#ifdef TCP_CWND
		int cwnd = 10; // from SPDY best practicies
		int rc_tcp_cwnd = setsockopt(fd, IPPROTO_TCP, TCP_CWND, &cwnd, sizeof(cwnd));
		if (rc_tcp_cwnd == -1) FT_WARN_ERRNO(errno, "Couldn't setsockopt on client connection (TCP_CWND)");
#endif
	}	

	bool res = ft_fd_nonblock(fd);
	if (!res) FT_WARN_ERRNO(errno, "Failed when setting established socket to non-blocking mode");

	this->data = NULL;
	this->protocol = NULL;
	this->delegate = delegate;
	this->context = context;
	this->read_frame = NULL;
	this->write_frames = NULL;
	this->write_frame_last = &this->write_frames;
	this->stats.read_events = 0;
	this->stats.write_events = 0;
	this->stats.write_direct = 0;
	this->stats.read_bytes = 0;
	this->stats.write_bytes = 0;
	this->flags.read_partial = false;
	this->flags.read_shutdown = false;
	this->read_shutdown_at = NAN;
	this->flags.write_shutdown = false;
	this->write_shutdown_at = NAN;
	this->flags.write_open = true;
	this->flags.read_throttle = false;	

	this->error.sys_errno = 0;

	assert(this->context->ev_loop != NULL);
	this->created_at = ev_now(this->context->ev_loop);

	memcpy(&this->ai_addr, peer_addr, peer_addr_len);
	this->ai_addrlen = peer_addr_len;

	this->ai_family = ai_family;
	this->ai_socktype = ai_socktype;
	this->ai_protocol = ai_protocol;

	ev_io_init(&this->read_watcher, _ft_seqpacket_on_read, fd, EV_READ);
	ev_set_priority(&this->read_watcher, -1); // Read has always lower priority than writing
	this->read_watcher.data = this;
	this->read_events = 0;

	ev_io_init(&this->write_watcher, _ft_seqpacket_on_write, fd, EV_WRITE);
	ev_set_priority(&this->write_watcher, 1);
	this->write_watcher.data = this;
	this->write_events = 0;
	this->flags.write_ready = false;

	return true;
}

bool ft_seqpacket_accept(struct ft_seqpacket * this, struct ft_seqpacket_delegate * delegate, struct ft_listener * listening_socket, int fd, const struct sockaddr * peer_addr, socklen_t peer_addr_len)
{
	assert(listening_socket != NULL);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN fd:%d", fd);

	if (listening_socket->ai_socktype != SOCK_STREAM)
	{
		FT_ERROR("Stream can handle only SOCK_STREAM addresses");
		return false;
	}

	FT_DEBUG("Accepting a socket connection");

	bool ok = _ft_seqpacket_init(
		this, delegate, listening_socket->context, 
		fd,
		peer_addr, peer_addr_len,
		listening_socket->ai_family,
		listening_socket->ai_socktype,
		listening_socket->ai_protocol
	);
	if (!ok)
	{
		FT_TRACE(FT_TRACE_ID_SEQPACKET, "END error" TRACE_FMT, TRACE_ARGS);
		return false;
	}

	this->flags.connecting = false;
	this->flags.active = false;
	this->connected_at = this->created_at;

	ok = ft_seqpacket_cntl(this, FT_SEQPACKET_READ_START | FT_SEQPACKET_WRITE_START);
	if (!ok) FT_WARN_P("Failed to set events properly");

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT, TRACE_ARGS);

	return true;
}


bool ft_seqpacket_connect(struct ft_seqpacket * this, struct ft_seqpacket_delegate * delegate, struct ft_context * context, const struct addrinfo * addr)
{
	if (addr->ai_socktype != SOCK_STREAM)
	{
		FT_ERROR("Stream can handle only SOCK_STREAM addresses");
		return false;
	}

	int fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
	if (fd < 0)
	{
		FT_ERROR_ERRNO(errno, "socket(%d, %d, %d)", addr->ai_family, addr->ai_socktype, addr->ai_protocol);
		return false;
	}

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN fd:%d", fd);


	bool ok = _ft_seqpacket_init(
		this, delegate, context,
		fd,
		addr->ai_addr, addr->ai_addrlen,
		addr->ai_family,
		addr->ai_socktype,
		addr->ai_protocol
	);
	if (!ok)
	{
		FT_TRACE(FT_TRACE_ID_SEQPACKET, "END error" TRACE_FMT, TRACE_ARGS);
		return false;
	}

	this->flags.connecting = true;
	this->flags.active = true;

	FT_DEBUG("Initializing a socket connection");

	int rc = connect(fd, addr->ai_addr, addr->ai_addrlen);
	if (rc != 0)
	{
		if (errno != EINPROGRESS)
		{
			FT_ERROR_ERRNO(errno, "connect()");
			FT_TRACE(FT_TRACE_ID_SEQPACKET, "END connect err" TRACE_FMT, TRACE_ARGS);
			return false;
		}
	}

	_ft_seqpacket_write_set_event(this, CONNECT_WANT_WRITE);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT, TRACE_ARGS);

	return true;
}

bool ft_seqpacket_init(struct ft_seqpacket * this, struct ft_seqpacket_delegate * delegate, struct ft_context * context, int fd)
{
	int rc;
	bool ok;

    int sock_type;
    socklen_t sock_type_length = sizeof(sock_type);
    rc = getsockopt(fd, SOL_SOCKET, SO_TYPE, &sock_type, &sock_type_length);
	if (rc != 0)
	{
		FT_ERROR_ERRNO_P(errno, "getsockopt");
		return false;
	}

	if (sock_type != SOCK_STREAM)
	{
		FT_ERROR("Stream can handle only SOCK_STREAM addresses");
		return false;
	}

	struct sockaddr_storage addr;
	socklen_t addrlen;
	rc = getsockname(fd, (struct sockaddr *)&addr, &addrlen);
	if (rc != 0)
	{
		FT_ERROR_ERRNO_P(errno, "getsockname");
		return false;
	}

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN fd:%d", fd);

	ok = _ft_seqpacket_init(
		this, delegate, context,
		fd,
		(const struct sockaddr *) &addr, addrlen,
		addr.ss_family,
		sock_type,
		0 // We don't have better info, we can use SO_PROTOCOL but available only on fresh Linux
	);
	if (!ok)
	{
		FT_TRACE(FT_TRACE_ID_SEQPACKET, "END error" TRACE_FMT, TRACE_ARGS);
		return false;
	}

	this->flags.connecting = false;
	this->flags.active = false;
	this->connected_at = this->created_at;

	ok = ft_seqpacket_cntl(this, FT_SEQPACKET_READ_START | FT_SEQPACKET_WRITE_START);
	if (!ok) FT_WARN_P("Failed to set events properly");

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT, TRACE_ARGS);

	return true;
}

void ft_seqpacket_fini(struct ft_seqpacket * this)
{
	assert(this != NULL);
	assert(this->read_watcher.fd >= 0);
	assert(this->write_watcher.fd == this->read_watcher.fd);

	if(this->delegate->fini != NULL) this->delegate->fini(this);	

	ft_seqpacket_cntl(this, FT_SEQPACKET_READ_STOP | FT_SEQPACKET_WRITE_STOP);

	int rc = close(this->read_watcher.fd);
	if (rc != 0) FT_WARN_ERRNO_P(errno, "close()");

	this->read_watcher.fd = -1;
	this->write_watcher.fd = -1;

	this->flags.read_shutdown = true;
	this->flags.write_shutdown = true;
	this->write_shutdown_at = this->read_shutdown_at = ev_now(this->context->ev_loop);
	this->flags.write_open = false;
	this->flags.write_ready = false;

	size_t cap;
	if (this->read_frame != NULL)
	{
		cap = ft_frame_pos(this->read_frame);
		ft_frame_return(this->read_frame);
		this->read_frame = NULL;

		if (cap > 0) FT_WARN("Lost %zu bytes in read buffer of the socket", cap);
	}

	cap = 0;
	while (this->write_frames != NULL)
	{
		struct ft_frame * frame = this->write_frames;
		this->write_frames = frame->next;

		cap += ft_frame_len(frame);
		ft_frame_return(frame);
	}
	if (cap > 0) FT_WARN("Lost %zu bytes in write buffer of the socket", cap);
}

///

static void _ft_seqpacket_error(struct ft_seqpacket * this, int sys_errno, const char * when)
{
	assert(sys_errno != 0);

	FT_WARN_ERRNO(sys_errno, "System error on stream when %s", when);

	this->error.sys_errno = sys_errno;

	if (this->delegate->error != NULL)
		this->delegate->error(this);

	this->flags.read_shutdown = true;
	this->flags.write_shutdown = true;
	
	this->read_events = 0;
	this->write_events = 0;

	ev_io_stop(this->context->ev_loop, &this->write_watcher);	
	ev_io_stop(this->context->ev_loop, &this->read_watcher);	

	// Perform hard close of the socket
	int rc = shutdown(this->write_watcher.fd, SHUT_RDWR);
	if (rc != 0)
	{
		switch (errno)
		{
			case ENOTCONN:
				break;

			default:
				FT_WARN_ERRNO(errno, "shutdown() in error handling");
		}		
	}

	FT_TRACE(FT_TRACE_ID_SEQPACKET, TRACE_FMT " %s", TRACE_ARGS, when);
}

///

static void _ft_seqpacket_on_connect_event(struct ft_seqpacket * this)
{
	assert(this != NULL);
	assert(this->flags.connecting == true);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, TRACE_FMT, TRACE_ARGS);

	int errno_cmd = -1;
	socklen_t optlen = sizeof(errno_cmd);
	int rc = getsockopt(this->write_watcher.fd, SOL_SOCKET, SO_ERROR, &errno_cmd, &optlen);
	if (rc != 0)
	{
		FT_ERROR_ERRNO(errno, "getsockopt(SOL_SOCKET, SO_ERROR) for async connect");
		return;
	}

	if (errno_cmd != 0)
	{
		_ft_seqpacket_error(this, errno_cmd, "connecting");
		return;
	}

	FT_DEBUG("Connection established");
	_ft_seqpacket_write_unset_event(this, CONNECT_WANT_WRITE);

	ft_seqpacket_cntl(this, FT_SEQPACKET_READ_START | FT_SEQPACKET_WRITE_START);

	this->connected_at = ev_now(this->context->ev_loop);
	this->flags.connecting = false;
	if (this->delegate->connected != NULL) this->delegate->connected(this);

	// Simulate a write event to dump frames in the write queue
	_ft_seqpacket_on_write_event(this);

	return;
}

///

void _ft_seqpacket_read_set_event(struct ft_seqpacket * this, enum _ft_seqpacket_read_event event)
{
	this->read_events |= event;
	if ((this->read_events != 0) && (this->flags.read_throttle == false) && (this->flags.read_shutdown == false))
		ev_io_start(this->context->ev_loop, &this->read_watcher);
}

void _ft_seqpacket_read_unset_event(struct ft_seqpacket * this, enum _ft_seqpacket_read_event event)
{
	this->read_events &= ~event;
	if ((this->read_events == 0) || (this->flags.read_shutdown == true) || (this->flags.read_throttle == true))
		ev_io_stop(this->context->ev_loop, &this->read_watcher);	
}

bool _ft_seqpacket_cntl_read_start(struct ft_seqpacket * this)
{
	assert(this != NULL);
	assert(this->read_watcher.fd >= 0);

	_ft_seqpacket_read_set_event(this, READ_WANT_READ);
	return true;
}


bool _ft_seqpacket_cntl_read_stop(struct ft_seqpacket * this)
{
	assert(this != NULL);
	assert(this->read_watcher.fd >= 0);

	_ft_seqpacket_read_unset_event(this, READ_WANT_READ);
	return true;
}

bool _ft_seqpacket_cntl_read_throttle(struct ft_seqpacket * this, bool throttle)
{
	assert(this != NULL);
	this->flags.read_throttle = throttle;

	if (throttle) _ft_seqpacket_read_unset_event(this, 0);
	else _ft_seqpacket_read_set_event(this, 0);

	return true;
}

static void _ft_seqpacket_read_shutdown(struct ft_seqpacket * this)
{
	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN " TRACE_FMT, TRACE_ARGS);

	// Stop futher reads on the socket
	ft_seqpacket_cntl(this, FT_SEQPACKET_READ_STOP);
	this->read_shutdown_at = ev_now(this->context->ev_loop);
	this->flags.read_shutdown = true;

	// Uplink read frame, if there is one (this can result in a partial frame but that's ok)
	if (ft_frame_pos(this->read_frame) > 0)
	{
		FT_WARN("Partial read due to read shutdown (%zd bytes)", ft_frame_pos(this->read_frame));
		bool upstreamed = this->delegate->read(this, this->read_frame);
		if (!upstreamed) ft_frame_return(this->read_frame);
		this->read_frame = NULL;
	}

	// Uplink (to delegate) end-of-stream
	struct ft_frame * frame = NULL;
	if ((this->read_frame != NULL) && (ft_frame_pos(this->read_frame) == 0))
	{
		// Recycle this->read_frame if there are no data read
		frame = this->read_frame;
		this->read_frame = NULL;

		ft_frame_format_empty(frame);
		ft_frame_set_type(frame, FT_FRAME_TYPE_STREAM_END);
	}

	if (frame == NULL)
	{
		frame = ft_pool_borrow(&this->context->frame_pool, FT_FRAME_TYPE_STREAM_END);
	}

	if (frame == NULL)
	{
		FT_WARN("Failed to submit end-of-stream frame, throttling");
		ft_seqpacket_cntl(this, FT_SEQPACKET_READ_PAUSE);
		//TODO: Re-enable reading when frames are available again -> this is trottling mechanism
		FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " out of frames", TRACE_ARGS);
		return;
	}

	bool upstreamed = this->delegate->read(this, frame);
	if (!upstreamed) ft_frame_return(frame);


	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT, TRACE_ARGS);
}

void _ft_seqpacket_on_read_event(struct ft_seqpacket * this)
{
	bool ok;
	assert(this != NULL);
	assert(this->flags.connecting == false);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN " TRACE_FMT, TRACE_ARGS);

	this->stats.read_events += 1;

	for (unsigned int read_loops = 0; read_loops<ft_config.sock_est_max_read_loops; read_loops += 1)
	{
		if (this->read_frame == NULL)
		{
			if (this->delegate->get_read_frame != NULL)
			{
				this->read_frame = this->delegate->get_read_frame(this);
			}
			else
			{
				this->read_frame = ft_pool_borrow(&this->context->frame_pool, FT_FRAME_TYPE_RAW_DATA);
				if (this->read_frame != NULL) ft_frame_format_simple(this->read_frame);
			}

			if (this->read_frame == NULL)
			{
				FT_WARN("Out of frames when reading, throttling");
				ft_seqpacket_cntl(this, FT_SEQPACKET_READ_PAUSE);
				//TODO: Re-enable reading when frames are available again -> this is trottling mechanism
				FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " out of frames", TRACE_ARGS);
				return;
			}
		}

		struct ft_vec * frame_dvec = ft_frame_get_vec(this->read_frame);
		assert(frame_dvec != NULL);

		size_t size_to_read = frame_dvec->limit - frame_dvec->position;
		assert(size_to_read > 0);

		void * p_to_read = frame_dvec->frame->data + frame_dvec->offset + frame_dvec->position;

		ssize_t rc;
		rc = recv(this->read_watcher.fd, p_to_read, size_to_read, 0);

		if (rc <= 0) // Handle error situation
		{
			if (rc < 0)
			{
				if (errno == EAGAIN)
				{
					_ft_seqpacket_read_set_event(this, READ_WANT_READ);
					FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " EAGAIN", TRACE_ARGS);
					return;
				}

				_ft_seqpacket_error(this, errno, "reading");
			}
			else
			{
				FT_DEBUG("Peer closed a connection");
				this->error.sys_errno = 0;
			}

			_ft_seqpacket_read_shutdown(this);
			FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT, TRACE_ARGS);
			return;
		}


		assert(rc > 0);
		FT_TRACE(FT_TRACE_ID_SEQPACKET, "READ " TRACE_FMT " rc:%zd", TRACE_ARGS, rc);
		this->stats.read_bytes += rc;
		ft_vec_advance(frame_dvec, rc);

		if (frame_dvec->position < frame_dvec->limit)
		{
			// Not all expected data arrived
			if (this->flags.read_partial == true)
			{
				bool upstreamed = this->delegate->read(this, this->read_frame);
				if (upstreamed) this->read_frame = NULL;
			}
			_ft_seqpacket_read_set_event(this, READ_WANT_READ);
			FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " incomplete read (%zd)", TRACE_ARGS, rc);
			return;
		}
		assert(frame_dvec->position == frame_dvec->limit);

		// Current dvec is filled, move to next one
		ok = ft_frame_next_vec(this->read_frame);
		if (!ok)
		{
			// All dvecs in the frame are filled with data
			bool upstreamed = this->delegate->read(this, this->read_frame);
			if (upstreamed) this->read_frame = NULL;
			if ((this->read_events & READ_WANT_READ) == 0)
			{
				// If watcher is stopped, break reading	
				FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " read stopped", TRACE_ARGS);
				return;
			}
		}
		else if (this->flags.read_partial == true)
		{
			bool upstreamed = this->delegate->read(this, this->read_frame);
			if (upstreamed) this->read_frame = NULL;
			if ((this->read_events & READ_WANT_READ) == 0)
			{
				// If watcher is stopped, break reading	
				FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " partial read stopped", TRACE_ARGS);
				return;
			}
		}
	}

	// Maximum read loops within event loop iteration is exceeded
	_ft_seqpacket_read_set_event(this, READ_WANT_READ);
	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " max read loops", TRACE_ARGS);
}

///

void _ft_seqpacket_write_set_event(struct ft_seqpacket * this, enum _ft_seqpacket_write_event event)
{
	this->write_events |= event;
	if (this->write_events != 0) ev_io_start(this->context->ev_loop, &this->write_watcher);
}

void _ft_seqpacket_write_unset_event(struct ft_seqpacket * this, enum _ft_seqpacket_write_event event)
{
	this->write_events &= ~event;
	if (this->write_events == 0) ev_io_stop(this->context->ev_loop, &this->write_watcher);
}


bool _ft_seqpacket_cntl_write_start(struct ft_seqpacket * this)
{
	assert(this != NULL);
	assert(this->write_watcher.fd >= 0);

	_ft_seqpacket_write_set_event(this, WRITE_WANT_WRITE);
	return true;
}


bool _ft_seqpacket_cntl_write_stop(struct ft_seqpacket * this)
{
	assert(this != NULL);
	assert(this->write_watcher.fd >= 0);

	_ft_seqpacket_write_unset_event(this, WRITE_WANT_WRITE);
	return true;
}


static void _ft_seqpacket_write_real(struct ft_seqpacket * this)
{
	ssize_t rc;

	assert(this != NULL);
	assert(this->flags.write_ready == true);
	assert(this->flags.connecting == false);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN " TRACE_FMT " Wf:%p", TRACE_ARGS, this->write_frames);	

	unsigned int write_loop = 0;
	while (this->write_frames != NULL)
	{
		FT_TRACE(FT_TRACE_ID_SEQPACKET, "FRAME " TRACE_FMT " ft:%llx", TRACE_ARGS, (unsigned long long)this->write_frames->type);

		if (write_loop > ft_config.sock_est_max_read_loops)
		{
			// Maximum write loops per event loop iteration reached
			this->flags.write_ready = false;
			_ft_seqpacket_write_set_event(this, WRITE_WANT_WRITE);
			FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " max loops", TRACE_ARGS);
			return; 
		}
		write_loop += 1;

		if (this->write_frames->type == FT_FRAME_TYPE_STREAM_END)
		{
			struct ft_frame * frame = this->write_frames;
			this->write_frames = frame->next;
			ft_frame_return(frame);

			if (this->write_frames != NULL) FT_ERROR("There are data frames in the write queue after end-of-stream.");

			assert(this->flags.write_open == false);
			this->write_shutdown_at = ev_now(this->context->ev_loop);
			this->flags.write_shutdown = true;

			_ft_seqpacket_write_unset_event(this, WRITE_WANT_WRITE);

			int rc = shutdown(this->write_watcher.fd, SHUT_WR);
			if (rc != 0)
			{
				if (errno == ENOTCONN) { /* NO-OP ... this can happen when connection is closed quickly after connecting */ }
				else
					FT_WARN_ERRNO_P(errno, "shutdown()");
			}

			FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " shutdown", TRACE_ARGS);
			return;
		}

		struct ft_vec * frame_dvec = ft_frame_get_vec(this->write_frames);
		assert(frame_dvec != NULL);

		size_t size_to_write = frame_dvec->limit - frame_dvec->position;
		assert(size_to_write > 0);

		const void * p_to_write = frame_dvec->frame->data + frame_dvec->offset + frame_dvec->position;

		rc = send(this->write_watcher.fd, p_to_write, size_to_write, 0);
		if (rc < 0) // Handle error situation
		{
			if (errno == EAGAIN)
			{
				// OS buffer is full, wait for next write event
				this->flags.write_ready = false;
				_ft_seqpacket_write_set_event(this, WRITE_WANT_WRITE);
				return;
			}

			_ft_seqpacket_error(this, errno, "writing");

			FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " write error", TRACE_ARGS);
			return;
		}
		else if (rc == 0)
		{
			//TODO: Test if this is a best possible reaction
			FT_WARN("Zero write occured, will retry soon");
			this->flags.write_ready = false;
			_ft_seqpacket_write_set_event(this, WRITE_WANT_WRITE);

			FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " zero write", TRACE_ARGS);
			return;
		}

		assert(rc > 0);
		FT_TRACE(FT_TRACE_ID_SEQPACKET, "WRITE " TRACE_FMT " rc:%zd", TRACE_ARGS, rc);
		this->stats.write_bytes += rc;
		ft_vec_advance(frame_dvec, rc);
		if (frame_dvec->position < frame_dvec->limit)
		{
			// Not all data has been written, wait for next write event
			this->flags.write_ready = false;
			_ft_seqpacket_write_set_event(this, WRITE_WANT_WRITE);
			FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " partial", TRACE_ARGS);
			return; 
		}
		assert(frame_dvec->position == frame_dvec->limit);

		// Current dvec is filled, move to next one
		bool ok = ft_frame_next_vec(this->write_frames);
		if (!ok)
		{
			// All dvecs in the frame have been written
			struct ft_frame * frame = this->write_frames;

			this->write_frames = frame->next;
			if (this->write_frames == NULL)
			{
				// There are no more frames to write
				this->write_frame_last = &this->write_frames;
			}

			ft_frame_return(frame);
		}
	}

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT, TRACE_ARGS);
	return;
}


void _ft_seqpacket_on_write_event(struct ft_seqpacket * this)
{
	assert(this != NULL);
	assert(this->flags.connecting == false);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN " TRACE_FMT, TRACE_ARGS);

	this->stats.write_events += 1;
	this->flags.write_ready = true;
	
	if (this->write_frames != NULL)
	{
		this->write_events &= ~WRITE_WANT_WRITE; // Just pretend that we are stopping a watcher
		_ft_seqpacket_write_real(this);
		if (this->write_events == 0) // Now do it for real if needed
			_ft_seqpacket_write_unset_event(this, WRITE_WANT_WRITE);
	}
	else
	{
		_ft_seqpacket_write_unset_event(this, WRITE_WANT_WRITE);
	}

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT, TRACE_ARGS);
}


bool ft_seqpacket_write(struct ft_seqpacket * this, struct ft_frame * frame)
{
	assert(this != NULL);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN " TRACE_FMT, TRACE_ARGS);

	if (this->flags.write_open == false)
	{
		FT_WARN("Stream is not open for writing (f:%p ft: %08llx)", frame, (unsigned long long) frame->type);
		return false;
	}

	if (frame->type == FT_FRAME_TYPE_STREAM_END)
		this->flags.write_open = false;

	//Add frame to the write queue
	*this->write_frame_last = frame;
	frame->next = NULL;
	this->write_frame_last = &frame->next;

	if (this->flags.write_ready == false)
	{
		if (this->flags.connecting == false) _ft_seqpacket_write_set_event(this, WRITE_WANT_WRITE);
		FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " queue", TRACE_ARGS);
		return true;
	}

	this->stats.write_direct += 1;

	_ft_seqpacket_write_real(this);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " direct", TRACE_ARGS);
	return true;
}

bool _ft_seqpacket_cntl_write_shutdown(struct ft_seqpacket * this)
{
	assert(this != NULL);
	
	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN " TRACE_FMT, TRACE_ARGS);

	if (this->flags.write_shutdown == true) return true;
	if (this->flags.write_open == false) return true;

	struct ft_frame * frame = ft_pool_borrow(&this->context->frame_pool, FT_FRAME_TYPE_STREAM_END);
	if (frame == NULL)
	{
		FT_WARN("Out of frames when preparing end of stream (write)");
		return false;
	}

	bool ret = ft_seqpacket_write(this, frame);
	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT, TRACE_ARGS);

	return ret;
}

///

static void _ft_seqpacket_on_read(struct ev_loop * loop, struct ev_io * watcher, int revents)
{
	struct ft_seqpacket * this = watcher->data;
	assert(this != NULL);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN " TRACE_FMT " e:%x ei:%u", TRACE_ARGS, revents, ev_iteration(loop));

	if ((revents & EV_ERROR) != 0)
	{
		_ft_seqpacket_error(this, ECONNRESET, "reading (libev)");
		goto end;
	}

	if ((revents & EV_READ) == 0) goto end;

	if (this->flags.read_throttle)
	{
		ev_io_stop(this->context->ev_loop, &this->read_watcher);
		goto end;
	}

	if (this->read_events & READ_WANT_READ)
		_ft_seqpacket_on_read_event(this);

end:
	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " e:%x ei:%u", TRACE_ARGS, revents, ev_iteration(loop));
}


static void _ft_seqpacket_on_write(struct ev_loop * loop, struct ev_io * watcher, int revents)
{
	struct ft_seqpacket * this = watcher->data;
	assert(this != NULL);

	FT_TRACE(FT_TRACE_ID_SEQPACKET, "BEGIN " TRACE_FMT " e:%x ei:%u", TRACE_ARGS, revents, ev_iteration(loop));

	if ((revents & EV_ERROR) != 0)
	{
		_ft_seqpacket_error(this, ECONNRESET, "writing (libev)");
		goto end;
	}

	if ((revents & EV_WRITE) == 0) goto end;

	if (this->write_events & CONNECT_WANT_WRITE)
		_ft_seqpacket_on_connect_event(this);

	if (this->write_events & WRITE_WANT_WRITE)
		_ft_seqpacket_on_write_event(this);

end:
	FT_TRACE(FT_TRACE_ID_SEQPACKET, "END " TRACE_FMT " e:%x ei:%u", TRACE_ARGS, revents, ev_iteration(loop));
}

void ft_seqpacket_diagnose(struct ft_seqpacket * this)
{
	assert(this != NULL);
	fprintf(stderr, TRACE_FMT "\n", TRACE_ARGS);
}
