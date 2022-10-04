#include "session.h"

#include "log.h"
#include "server.h"

#include <boost/beast/http.hpp>
#include <boost/format.hpp>

#include <vector>

const uint32_t RTSP_STREAM_ID = 1;

struct MyDataSource
{
	const uint8_t* data = nullptr;
	size_t dataLen = 0;
	size_t copiedDataLen = 0;
};

struct MyRtspDataSource
{
	uint8_t* data = nullptr;
	size_t dataLen = 0;
	size_t copiedDataLen = 0;
};

int send_response(nghttp2_session* Session, int32_t stream_id, nghttp2_nv* nva, size_t nvlen)
{
	int rv;

	rv = nghttp2_submit_response(Session, stream_id, nva, nvlen, NULL);

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(fatal) << "Fatal error: " << nghttp2_strerror(rv);
		return -1;
	}

	return 0;
}

int my_on_request_recv(nghttp2_session* http2_session, Session* /*session_instance*/, int32_t stream_id)
{
	nghttp2_nv hdrs[] = {MAKE_NV(":status", "200")};

	if(send_response(http2_session, stream_id, hdrs, ARRLEN(hdrs)) != 0)
	{
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}

	return NGHTTP2_NO_ERROR;
}

int on_data_chunk_recv_callback(nghttp2_session* /*http2_session*/, const uint8_t /*flags*/, int32_t stream_id, const uint8_t* data, size_t length, void* user_data)
{
	auto* s = (Session*)user_data;

	auto rtsp_conn_it = std::find_if(s->rtsp_conns_.begin(), s->rtsp_conns_.end(),
										[stream_id](const std::shared_ptr<RTSPConnection>& r)
										{
											return r->StreamID() == stream_id;
										}
									);

	if (rtsp_conn_it != s->rtsp_conns_.end())
	{
		if((*rtsp_conn_it)->isReady())
		{
			(*rtsp_conn_it)->process_rtsp_data_from_vms(data, length);
		}
		else
		{
			BOOST_LOG_TRIVIAL(fatal) << "Rtsp connection is NOT ready!";
			return -1;
		}

		return NGHTTP2_NO_ERROR;
	}

	auto conn_it = std::find_if(s->http_conns_.begin(), s->http_conns_.end(),
			[stream_id](const std::shared_ptr<HTTPConnection>& hc)
	{
		return hc->StreamID() == stream_id;
	});

	HTTPConnection* conn = nullptr;

	if(conn_it == s->http_conns_.end())
	{
		s->http_conns_.push_back(std::make_shared<HTTPConnection>(stream_id, s->camera_endpoints_, s));
		conn = s->http_conns_.back().get();
		conn->start();
	}
	else
	{
		conn = (*conn_it).get();
	}

	conn->appendData((const char*)data, length);

	return NGHTTP2_NO_ERROR;
}

int on_connect_request_recv(nghttp2_session* http2_session, Session* session_instance, int32_t streamID)
{
	nghttp2_nv hdrs[] = {MAKE_NV(":status", "200")};

	int rv = nghttp2_submit_headers(http2_session, NGHTTP2_DATA_FLAG_NO_END_STREAM, streamID, NULL, hdrs, ARRLEN(hdrs), session_instance);

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(fatal) << "What: " << nghttp2_strerror(rv);
		return rv;
	}

	return NGHTTP2_NO_ERROR;
}

int on_frame_recv_callback(nghttp2_session* http2_session, const nghttp2_frame* frame, void* user_data)
{
	Session* session_instance = (Session*)user_data;

	switch(frame->hd.type)
	{
		case NGHTTP2_HEADERS:
		{
			/* Check that the client request has finished */
			if(frame->hd.flags & NGHTTP2_FLAG_END_STREAM) // if it is a some control packet like UPDATE or something process it quickly and return STATUS OK
			{
				return my_on_request_recv(http2_session, session_instance, frame->hd.stream_id);
			}
			/*else // and if it is a tunnelled HTTP 1.1 request, header frames should be followed by data frames, so the END stream flag should be false
			{
			}*/

			break;
		}

		case NGHTTP2_DATA:
		{
			if(nghttp2_session_want_write(http2_session))
			{
				auto rv = session_instance->session_send();

				if(rv != 0)
				{
					BOOST_LOG_TRIVIAL(error) << "Session failed to write something on DATA receive: " << nghttp2_strerror(rv);
				}
			}

			auto stream_id = frame->hd.stream_id;
			auto it = std::find_if(session_instance->http_conns_.begin(), session_instance->http_conns_.end(),
					[stream_id](const std::shared_ptr<HTTPConnection>& c)
			{
				return c->StreamID() == stream_id;
			});

			if(it != session_instance->http_conns_.end())
			{
				if(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
				{
					BOOST_LOG_TRIVIAL(trace) << "Need to forward a request. Whole buffer size: " << (*it)->Length();
					(*it)->forward_request();
				}
				else
				{
					BOOST_LOG_TRIVIAL(trace) << "Flag END_STREAM not received. Waiting for more data. Current buffer size: "
						<< (*it)->Length();
				}
			}

			break;
		}

		case NGHTTP2_WINDOW_UPDATE:
		{
			auto stream_id = frame->hd.stream_id;
			auto it = std::find_if(session_instance->rtsp_conns_.begin(), session_instance->rtsp_conns_.end(),
						[stream_id](const std::shared_ptr<RTSPConnection>& c)
						{
							return c->StreamID() == stream_id;
						});

			if (it != session_instance->rtsp_conns_.end())
			{
				(*it)->do_submit_data();
			}
		}

		default:

			break;
	}

	return NGHTTP2_NO_ERROR;
}

int on_header_callback(nghttp2_session* http2_session, const nghttp2_frame* frame,
	const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t /*flags*/, void* user_data)
{
	auto* session_instance = (Session*)user_data;

	if(0 == strncmp(reinterpret_cast<const char*>(name), ":method", namelen))
	{
		if (0 == strncmp(reinterpret_cast<const char*>(value), "CONNECT", valuelen))
		{			
			on_connect_request_recv(http2_session, session_instance, frame->hd.stream_id);

			// setup RTSP connection with a camera
			session_instance->connect_rtsp(frame->hd.stream_id);
		}
		else /*if (0 == strncmp(reinterpret_cast<const char*>(value), "POST", valuelen) == 0)*/
		{
			session_instance->http_conns_.push_back(
				std::make_shared<HTTPConnection>(frame->hd.stream_id, session_instance->camera_endpoints_, session_instance));
			session_instance->http_conns_.back()->start();
		}
	}
	else
	{
		auto conn_it = std::find_if(session_instance->http_conns_.begin(), session_instance->http_conns_.end(),
			[stream_id = frame->hd.stream_id](const std::shared_ptr<HTTPConnection>& hc)
			{
				return hc->StreamID() == stream_id;
			});

		if (conn_it != session_instance->http_conns_.end())
		{
			if (0 == strncmp(reinterpret_cast<const char*>(name), ":path", namelen))
			{
				(*conn_it)->SetPath(std::string{reinterpret_cast<const char*>(value), valuelen});
			}
			else if (0 == strncmp(reinterpret_cast<const char*>(name), "authorization", namelen))
			{
				(*conn_it)->SetAuthentication(std::string{reinterpret_cast<const char*>(value), valuelen});
			}
		}
	}

	return NGHTTP2_NO_ERROR;
}

ssize_t on_datasource_read(nghttp2_session* /*Session*/, int32_t /*stream_id*/, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* /*user_data*/)
{
	BOOST_LOG_TRIVIAL(trace) << "on_datasource_read. Max data lenth: " << length;

	auto myds = (MyDataSource*)source->ptr;

	auto copyingLen = std::min(myds->dataLen - myds->copiedDataLen, length);
	memcpy(buf, myds->data + myds->copiedDataLen, copyingLen);
	myds->copiedDataLen += copyingLen;

	if(myds->dataLen == myds->copiedDataLen)
	{
		*data_flags = NGHTTP2_DATA_FLAG_EOF;
	}

	return (ssize_t)copyingLen;
}

int on_stream_close_callback(nghttp2_session* /*session*/, int32_t stream_id, uint32_t /*error_code*/, void* user_data)
{
	auto* session_instance = (Session*)user_data;
	auto it = std::find_if(session_instance->http_conns_.begin(), session_instance->http_conns_.end(),
			[stream_id](const std::shared_ptr<HTTPConnection>& c)
	{
		return c->StreamID() == stream_id;
	});

	if(it != session_instance->http_conns_.end())
	{
		BOOST_LOG_TRIVIAL(trace) << "Delete HTTPConnection with stream ID: " << (*it)->StreamID();
		session_instance->http_conns_.erase(it);
	}

	return NGHTTP2_NO_ERROR;
}

ssize_t send_callback(nghttp2_session* /*session_data*/, const uint8_t* data, size_t length, int /*flags*/, void* user_data)
{
	Session* session_instance = (Session*)user_data;

	auto s = session_instance->socket_.write_some(boost::asio::buffer(data, length));

	BOOST_LOG_TRIVIAL(trace) << "nghttp2 writes in size: " << length;

	if(s != length)
		BOOST_LOG_TRIVIAL(warning) << "nghttp2 wanted to write: " << length << ". Sent bytes: " << s;

	return (ssize_t)s;

	/*
	boost::asio::async_write(session_instance->socket_,
		boost::asio::buffer(data, length),
		[self = session_instance->copy()](const boost::system::error_code & ec, std::size_t bytes_transferred)
	{
		//self->do_read();
	}
	);

	return (ssize_t)length;
	*/
}

ssize_t on_rtsp_datasource_read(nghttp2_session* Session, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data);

Session::Session(tcp::socket socket,
	const tcp::resolver::results_type& camera_endpoints,
	const tcp::resolver::results_type& camera_rtsp_endpoint,
	boost::asio::io_context& io_ctx)
	: socket_(std::move(socket)),
		camera_endpoints_(camera_endpoints),
		camera_rtsp_endpoint_(camera_rtsp_endpoint),
		io_context_(io_ctx)
{
	BOOST_LOG_TRIVIAL(debug) << "New connection: " << socket_.remote_endpoint().address().to_string() << ":" << socket_.remote_endpoint().port();


	nghttp2_session_callbacks* mycallbacks;
	nghttp2_session_callbacks_new(&mycallbacks);

	nghttp2_session_callbacks_set_send_callback(mycallbacks, send_callback);


	nghttp2_session_callbacks_set_on_frame_recv_callback(mycallbacks, on_frame_recv_callback);
	nghttp2_session_callbacks_set_on_header_callback(mycallbacks, on_header_callback);
	nghttp2_session_callbacks_set_on_stream_close_callback(mycallbacks, on_stream_close_callback);

	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(mycallbacks, on_data_chunk_recv_callback);

	nghttp2_session_server_new(&http2_session_, mycallbacks, this);

	nghttp2_session_callbacks_del(mycallbacks);
}

Session::~Session()
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__;
}

int Session::send_server_connection_header()
{
	nghttp2_settings_entry iv[2] =
	{
		{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
		{NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1}
	};

	int rv;

	rv = nghttp2_submit_settings(http2_session_, NGHTTP2_FLAG_NONE, iv, ARRLEN(iv));

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "Fatal error: " << nghttp2_strerror(rv);
		return -1;
	}

	return 0;
}

int Session::send_data(int32_t stream_id, const uint8_t* data, size_t datalen)
{
	nghttp2_data_provider data_prd;

	nghttp2_data_source dsrc;

	MyDataSource dsrcPtr;
	dsrcPtr.data = data;
	dsrcPtr.dataLen = datalen;

	dsrc.ptr = &dsrcPtr;
	dsrc.fd = 0;

	data_prd.source = dsrc;
	data_prd.read_callback = &on_datasource_read;

	int rv = nghttp2_submit_data(http2_session_, NGHTTP2_FLAG_END_STREAM, stream_id, &data_prd);

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "Submitting data finished with an error: " << nghttp2_strerror(rv);
		return rv;
	}

	rv = session_send();

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "Sending data finished with an error: " << nghttp2_strerror(rv);
		return rv;
	}

	return 0;
}

int Session::forward_response(int32_t stream_id, short status_code, const uint8_t* data, size_t datalen, std::vector<std::pair<std::string, std::string>> headers)
{
	MyDataSource data_source;
	data_source.data = data;
	data_source.dataLen = datalen;

	nghttp2_data_source dsrc;
	dsrc.ptr = &data_source;

	nghttp2_data_provider data_prd;
	data_prd.source = dsrc;
	data_prd.read_callback = &on_datasource_read;

	auto status_code_str = std::to_string(status_code);
	std::vector<nghttp2_nv> hdrs
	{
		{(uint8_t*)":status", (uint8_t*)status_code_str.data(), 7, status_code_str.length()}
	};

	for (auto& p : headers)
	{
		hdrs.push_back(nghttp2_nv{(uint8_t*)p.first.c_str(), (uint8_t*)p.second.c_str(),
			p.first.size(), p.second.size(), NGHTTP2_NV_FLAG_NONE});
	}

	auto rv = nghttp2_submit_response(http2_session_, stream_id, hdrs.data(), hdrs.size(), &data_prd);

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "What: " << nghttp2_strerror(rv) << std::endl;
		return rv;
	}

	/*if(send_response(http2_session_, stream_id, hdrs, ARRLEN(hdrs)) != 0)
	{
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	}*/

	return session_send();
}

int Session::session_send()
{
	int rv;
	rv = nghttp2_session_send(http2_session_);

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "Session send finished with an error: " << nghttp2_strerror(rv);
		return -1;
	}

	return 0;
}

void Session::Start()
{
	if(send_server_connection_header() != 0
		|| session_send() != 0)
	{

		BOOST_LOG_TRIVIAL(fatal) << "Here need to delete session_data, it's not implemented";
		//delete_http2_session_data(session_data);
		throw std::runtime_error("Error while sending http settings");
	}

	// start read on the http requests port
	do_read();
}

void Session::do_read()
{
	auto self(shared_from_this());
	socket_.async_read_some(boost::asio::buffer(data_, max_length),
		[this, self](boost::system::error_code ec, std::size_t length)
	{

		if(ec == boost::asio::error::eof)
		{
			BOOST_LOG_TRIVIAL(info) << "HTTP/2 client closed the connection";
			return;
		}
		else if(!ec)
		{
			ssize_t readlen;
			readlen = nghttp2_session_mem_recv(http2_session_, (const uint8_t*)data_, length);

			if(readlen < 0)
			{
				throw std::runtime_error(std::string("Fatal error: ") + nghttp2_strerror((int)readlen));
			}

			if(session_send() != 0)
			{
				throw std::runtime_error("Error in session_send");
			}
		}
		else
		{
			BOOST_LOG_TRIVIAL(error) << "Error in reading: " << ec.message();
		}

		do_read();
	});
}

void Session::connect_rtsp(int32_t http2_stream_id)
{
	// TODO: make the method async

	using socket_t = boost::asio::ip::tcp::socket;
	auto rtspSocket = std::make_shared<socket_t>(io_context_);

	if(camera_rtsp_endpoint_.size() == 1)
	{
		auto it = camera_rtsp_endpoint_.begin();
		BOOST_LOG_TRIVIAL(debug) << "Trying to connect to RTSP endpoint="
			<< it->endpoint().address().to_string()
			<< ":" << it->endpoint().port();
	}

	boost::system::error_code ec;
	boost::asio::connect(*rtspSocket, camera_rtsp_endpoint_, ec);

	if(ec)
	{
		BOOST_LOG_TRIVIAL(fatal) << "Failed to connect to a camera's RTSP port: " << ec.message();
		return;
	}
	else
	{
		BOOST_LOG_TRIVIAL(trace) << "Connected to RTSP endpoint";
	}

	rtsp_conns_.push_back(std::make_shared<RTSPConnection>(http2_stream_id, std::move(rtspSocket), this));
	rtsp_conns_.back()->do_read_rtsp_data_from_camera();

	// I think it's better to use blocking version of the connect method instead this
	/*
	boost::asio::async_connect(*rtspSocket, camera_rtsp_endpoint_,
		[socket = rtspSocket, this](const boost::system::error_code & ec, tcp::endpoint) mutable
	{
		if(ec)
		{
			BOOST_LOG_TRIVIAL(fatal) << "Failed to connect to a camera's RTSP port: " << ec.message();
			return;
		}

		std::swap(camera_rtsp_socket_, socket);

		do_read_rtsp_data_from_camera();
	});
	*/
}

void Session::do_read_on_camera_socket(int32_t stream_id)
{
	/*
	auto buff = innerBuffer_.prepare(max_length);
	camera_socket_->async_read_some(buff,
		[self = shared_from_this(), stream_id](const boost::system::error_code & error, std::size_t bytes_transferred)
	{
		if(error == boost::asio::error::eof)
		{
			BOOST_LOG_TRIVIAL(trace) << self->local_endpoint_ << ": Connection closed by a camera";
			return;
		}
		else if(error)
		{
			BOOST_LOG_TRIVIAL(error) << self->local_endpoint_ << ": Error occurred while reading a response from a camera! Msg: " << error.message()
				<< ". Code: " << error.value();
			throw std::runtime_error("do_read_on_camera_socket()");
		}

		self->innerBuffer_.commit(bytes_transferred);

		BOOST_LOG_TRIVIAL(trace) << "Recv a response in size: " << bytes_transferred;

		boost::beast::http::response_parser<boost::beast::http::string_body> http_parser;
		boost::beast::error_code ec;

		// parse a response's header
		auto header_consumed = http_parser.put(boost::asio::buffer(self->innerBuffer_.data(), self->innerBuffer_.size()), ec);

		// TODO: add @ec handling correctly. There could be an error because not enough data received
		if(ec) throw std::runtime_error("parsing HTTP 1.1 response header error");

		// parse a response's body
		auto body_consumed = http_parser.put(boost::asio::buffer(self->innerBuffer_.data() + header_consumed, self->innerBuffer_.size() - header_consumed), ec);

		if(ec) throw std::runtime_error("parsing HTTP 1.1 response body error");

		if(!http_parser.is_done() || !http_parser.is_header_done())
		{
			BOOST_LOG_TRIVIAL(debug) << self->local_endpoint_ << ": Not enough data. Waiting for more data!";
			return self->do_read_on_camera_socket(stream_id);
		}

		if(self->forward_response(stream_id, http_parser.get().result_int(),
				(uint8_t*)http_parser.get().body().data(), http_parser.get().body().size()))
		{
			BOOST_LOG_TRIVIAL(error) << self->local_endpoint_ << ": Forwarding a response for a streamID: " << stream_id << " finished with an error";
		}

		self->innerBuffer_.consume(header_consumed + body_consumed);

		//s->send_data(stream_id, (const uint8_t*)s->data_, bytes_transferred);
	});
	*/
}

void Session::do_write(std::size_t length)
{
	auto self(shared_from_this());
	boost::asio::async_write(socket_, boost::asio::buffer(data_, length),
		[this, self](boost::system::error_code ec, std::size_t /*length*/)
	{
		if(!ec)
		{
			//do_read();
		}
	});
}

RTSPConnection::RTSPConnection(int32_t streamID, std::shared_ptr<tcp::socket>&& socket, Session* parent)
	: stream_id_(streamID)
	, rtsp_socket_(socket)
	, parent_(parent)
	, ws_(std::make_unique<ws::Server>(std::bind(&RTSPConnection::wsIncomingDataReadyCb, this, std::placeholders::_1, std::placeholders::_2),
				std::bind(&RTSPConnection::wsWrappedDataReadyCb, this, std::placeholders::_1, std::placeholders::_2)))
{
	local_endpoint_ = "Failed to determine local endpoint";

	try
	{
		local_endpoint_ = (boost::format("%1%:%2%") % rtsp_socket_->local_endpoint().address().to_string()
				% rtsp_socket_->local_endpoint().port())
			.str();
	}
	catch(const boost::system::system_error& e)
	{
		BOOST_LOG_TRIVIAL(warning) << local_endpoint_ << "! What: " << e.what();
	}
}

void RTSPConnection::process_rtsp_data_from_vms(const uint8_t* data, size_t length)
{
	// feed incoming data to ws::server
	ws_->SubmitChunk((const char*)data, length);
}

void RTSPConnection::do_read_rtsp_data_from_camera()
{
	rtsp_socket_->async_read_some(boost::asio::buffer(rtsp_read_buffer_, max_length),
		[self = shared_from_this()](boost::system::error_code ec, std::size_t length)
	{
		if(ec == boost::asio::error::eof)
		{
			BOOST_LOG_TRIVIAL(info) << self->local_endpoint_ << " Camera closed RTSP connection";
			// TODO: remove this stream instance too
			return;
		}
		else if(ec)
		{
			BOOST_LOG_TRIVIAL(error) <<  self->local_endpoint_ << " Reading RTSP from a camera finished with an error: "
				<< ec.message() << ". Errc: " << ec.value();
			return;
		}

		BOOST_LOG_TRIVIAL(trace) <<  self->local_endpoint_ << " Read RTSP data from a camera with size: " << length;

		// put incoming data from a camera in the write buffer
		auto buf = self->rtsp_write_buffer_.prepare(length);
		memcpy(buf.data(), self->rtsp_read_buffer_, length);
		self->rtsp_write_buffer_.commit(length);

		self->do_submit_data();

		self->parent_->io_context_.post([ = ]()
		{
			// schedule next read
			self->do_read_rtsp_data_from_camera();
		});
	});
}

void RTSPConnection::do_submit_data()
{
	int32_t window_size = nghttp2_session_get_stream_remote_window_size(parent_->http2_session_, stream_id_);

	BOOST_LOG_TRIVIAL(trace) <<  local_endpoint_ << " RTSP inner buffer size and window size: "
		<< rtsp_write_buffer_.size() << " " << window_size;

	int32_t copying = std::min(std::min(static_cast<int32_t>(rtsp_write_buffer_.size()), window_size - 1000), 65536 - 1000);

	if (copying <= 0)
	{
		BOOST_LOG_TRIVIAL(trace) << "Skipping submitting data";
		return;
	}

	ws_->WrapData((const char*)rtsp_write_buffer_.data().data(), copying);

	window_size -= copying;
	rtsp_write_buffer_.consume(copying);

	if (rtsp_write_buffer_.size() > 0)
	{
		parent_->io_context_.post([ = ]
			{
				do_submit_data();
			});
	}
}

void RTSPConnection::wsWrappedDataReadyCb(const char* data, size_t dataLen)
{
	MyRtspDataSource* data_source = new MyRtspDataSource;
	data_source->data = new uint8_t[dataLen];
	memcpy(data_source->data, data, dataLen);
	data_source->dataLen = dataLen;

	nghttp2_data_source dsrc;
	dsrc.ptr = data_source;

	nghttp2_data_provider data_prd;
	data_prd.source = dsrc;
	data_prd.read_callback = &on_rtsp_datasource_read;

	auto rv = nghttp2_submit_data(parent_->http2_session_, NGHTTP2_DATA_FLAG_NO_END_STREAM, stream_id_, &data_prd);

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "Forwarding RTSP data from a camera finished with an error: " << nghttp2_strerror(rv);
		if (data_source)
		{
			if (data_source->data)
			{
				delete data_source;
				data_source = nullptr;
			}
			delete data_source;
			data_source = nullptr;
		}
		return;
	}

	parent_->session_send();
}

ssize_t on_rtsp_datasource_read(nghttp2_session* Session, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data)
{
	BOOST_LOG_TRIVIAL(trace) << "on_rtsp_datasource_read(). Max data lenth: " << length;

	auto myds = (MyRtspDataSource*)source->ptr;

	auto copyingLen = std::min(myds->dataLen - myds->copiedDataLen, length);

	memcpy(buf, myds->data + myds->copiedDataLen, copyingLen);
	myds->copiedDataLen += copyingLen;

	if(myds->dataLen == myds->copiedDataLen)
	{
		*data_flags = NGHTTP2_DATA_FLAG_EOF;
		delete myds->data;
		myds->data = nullptr;
		delete myds;
		myds = nullptr;
	}

	return (ssize_t)copyingLen;
}

void RTSPConnection::wsIncomingDataReadyCb(const char* data, size_t dataLen)
{
	boost::asio::async_write(*rtsp_socket_, boost::asio::buffer(data, dataLen),
		[](boost::system::error_code ec, std::size_t length)
	{
		if(ec)
		{
			BOOST_LOG_TRIVIAL(error) << "Error in write";
			return;
		}

		BOOST_LOG_TRIVIAL(trace) << "Send RTSP data to a camera in size: " << length;
	});
}

HTTPConnection::HTTPConnection(int32_t streamID, tcp::resolver::results_type camera_endpoints, Session* parent)
	: stream_id_(streamID)
	, camera_endpoints_(camera_endpoints)
	, parent_(parent)
{
}

void HTTPConnection::read_from_camera()
{
	auto buff = innerBuffer_.prepare(max_length);
	socket_->async_read_some(buff,
		[self = shared_from_this()](const boost::system::error_code & error, std::size_t bytes_transferred)
	{
		if(error == boost::asio::error::eof)
		{
			BOOST_LOG_TRIVIAL(trace) << self->local_endpoint_ << ": Connection closed by a camera";
			return;
		}
		else if(error)
		{
			BOOST_LOG_TRIVIAL(error) << self->local_endpoint_ << ": Error occurred while reading a response from a camera! Msg: " << error.message()
				<< ". Code: " << error.value();
			throw std::runtime_error("do_read_on_camera_socket()");
		}

		self->innerBuffer_.commit(bytes_transferred);

		BOOST_LOG_TRIVIAL(trace) << "Recv a response in size: " << bytes_transferred;

		boost::beast::http::response_parser<boost::beast::http::string_body> http_parser;
		boost::beast::error_code ec;

		// parse a response's header
		auto header_consumed = http_parser.put(boost::asio::buffer(self->innerBuffer_.data(), self->innerBuffer_.size()), ec);

		// TODO: add @ec handling correctly. There could be an error because not enough data received
		if(ec) throw std::runtime_error("parsing HTTP 1.1 response header error");

		// parse a response's body
		auto body_consumed = http_parser.put(boost::asio::buffer(self->innerBuffer_.data() + header_consumed, self->innerBuffer_.size() - header_consumed), ec);

		if(ec) throw std::runtime_error("parsing HTTP 1.1 response body error");

		if (!http_parser.is_header_done())
		{
			BOOST_LOG_TRIVIAL(debug) << self->local_endpoint_ << ": Header not parsed yet. Waiting for more data!";
			return self->read_from_camera();
		}

		if (!http_parser.content_length() && header_consumed == self->innerBuffer_.size())
		{
			BOOST_LOG_TRIVIAL(warning) << self->local_endpoint_ << ": Header parsed but no Content-Length from a device. Not waiting for HTTP body!";
		}
		else if(!http_parser.is_done())
		{
			BOOST_LOG_TRIVIAL(debug) << self->local_endpoint_ << ": Not enough data. Waiting for more data!";
			return self->read_from_camera();
		}

		std::vector<std::pair<std::string, std::string>> headers;
		for(auto it = http_parser.get().begin(); it != http_parser.get().end(); ++it)
		{
			static const std::string WWW_AUTH = "WWW-Authenticate";
			if (it->name_string() == WWW_AUTH
					&& (it->value().to_string().find("Digest") != std::string::npos
						|| it->value().to_string().find("digest") != std::string::npos)) // this check just to make sure this is Digest auth data. Some devices may add also Basic auth data
			{
				headers.push_back(std::make_pair(WWW_AUTH, it->value().to_string()));
			}
		}

		if(self->parent_->forward_response(self->stream_id_, http_parser.get().result_int(),
				(uint8_t*)http_parser.get().body().data(), http_parser.get().body().size(), headers))
		{
			BOOST_LOG_TRIVIAL(error) << self->local_endpoint_ << ": Forwarding a response for a streamID: "
				<< self->stream_id_ << " finished with an error";
		}

		self->innerBuffer_.consume(header_consumed + body_consumed);
	});
}

void HTTPConnection::start()
{
	socket_ = std::make_unique<tcp::socket>(parent_->io_context_);

	boost::system::error_code ec;
	// TODO: refactore this with async connect version
	boost::asio::connect(*socket_, camera_endpoints_, ec);

	if(ec == boost::system::errc::success)
	{
		local_endpoint_ = "Failed to determine local endpoint";

		try
		{
			local_endpoint_ = (boost::format("%1%:%2%") % socket_->local_endpoint().address().to_string()
					% socket_->local_endpoint().port())
				.str();
			
			remote_ipv4_ = socket_->remote_endpoint().address().to_string();
		}
		catch(const boost::system::system_error& e)
		{
			BOOST_LOG_TRIVIAL(warning) << local_endpoint_ << "! What: " << e.what();
		}
	}
}

void HTTPConnection::forward_request()
{
	using namespace boost::beast;
	int version{11};
	auto req = std::make_shared<http::request<http::string_body>>(http::verb::post, path_, version);
	req->set(http::field::user_agent, "MUX/DEMUX service");	
	req->set(http::field::content_type, "application/soap+xml; charset=utf-8");
	req->set(http::field::host, remote_ipv4_);

	if (!auth_.empty())
	{
		req->set(http::field::authorization, auth_);
	}

	if (length_)
	{
		req->set(http::field::content_length, length_);
		req->body().assign((const char*)buffer_, length_);
	}
	else
	{
		BOOST_LOG_TRIVIAL(warning) << "A data length of a requst's body to forward is 0! Something went wrong!";
	}
	
	http::async_write(*socket_, *req,
		[req, self = shared_from_this()](const error_code& ec, size_t len)
		{
			if(!ec)
			{
				self->read_from_camera();
				BOOST_LOG_TRIVIAL(trace) << boost::format("[%1%] Write data in a camera socket with size: ")
					% self->local_endpoint_ << len;
			}
			else
			{
				BOOST_LOG_TRIVIAL(error) << boost::format("[%1%] Failed to write in a camera socket. What: ")
					% self->local_endpoint_ << ec.message();
			}
		}
	);
}

void HTTPConnection::appendData(const char* data, size_t length)
{
	memcpy(buffer_ + length_, data, length);
	length_ += length;
}
