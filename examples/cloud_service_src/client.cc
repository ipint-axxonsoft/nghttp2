#include <nghttp2/nghttp2.h>

#include "log.h"
#include "client.h"

#include <boost/system/error_code.hpp>
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/format.hpp>

#include <iostream>
#include <memory>
#include <utility>
#include <functional>

Client::Client(boost::asio::io_context& io_context, short http2_port, short requests_listening_port, short rtsp_listening_port)
	: io_context_(io_context)
	, acceptor_(io_context_, tcp::endpoint(tcp::v4(), http2_port))
	, requests_listening_port_acceptor_(io_context, tcp::endpoint(tcp::v4(), requests_listening_port))
	, rtsp_listening_port_acceptor_(io_context, tcp::endpoint(tcp::v4(), rtsp_listening_port))
{
	BOOST_LOG_TRIVIAL(info) << "ports: http2=" << http2_port
		<< "; http_port=" << requests_listening_port
		<< "; rtsp_port=" << rtsp_listening_port;

	do_accept();
}


void Client::do_accept()
{
	acceptor_.async_accept(
		[this](boost::system::error_code ec, tcp::socket socket)
		{
			if(!ec)
			{
				BOOST_LOG_TRIVIAL(debug) << "HTTP/2 TCP connection accepted from " << socket.remote_endpoint().address().to_string()
					<< ":" << socket.remote_endpoint().port();
				socket_.reset(new boost::asio::ip::tcp::socket(std::move(socket)));

				init_nghttp2();
				open_http2_connection();

				do_read();

				do_requests_port_accept();
				do_rtsp_connections_accept();
			}
		}
	);
}

void Client::do_requests_port_accept()
{
	BOOST_LOG_TRIVIAL(debug) << "Start listening for a requests connection";
	requests_listening_port_acceptor_.async_accept(
		[this](boost::system::error_code ec, tcp::socket socket)
	{
		if(!ec)
		{
			BOOST_LOG_TRIVIAL(debug) << "New request connection accepted from " << socket.remote_endpoint().address().to_string()
				<< ":" << socket.remote_endpoint().port();

			auto rc = std::make_shared<VmsConnection>(std::move(socket), this);
			rc->Start();
			rconnections.push_back(rc);
		}

		do_requests_port_accept();
	}
	);
}


void Client::init_nghttp2()
{
	nghttp2_session_callbacks* callbacks;
	nghttp2_session_callbacks_new(&callbacks);
	nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
	nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
	nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_recv_callback);
	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
	nghttp2_session_client_new(&http2_session_, callbacks, this);
	nghttp2_session_callbacks_del(callbacks);

	int rv = 0;
	nghttp2_option* options = nullptr;
	rv = nghttp2_option_new(&options); // TODO: check the result
	nghttp2_option_set_no_auto_window_update(options, 1);
	nghttp2_option_del(options);

	const int32_t connection_level = 0;
	const int32_t _1Mb = 1 * 1024 * 1024;
	rv = nghttp2_session_set_local_window_size(http2_session_, NGHTTP2_FLAG_NONE, connection_level, _1Mb);
	if (rv != NGHTTP2_NO_ERROR)
	{
		BOOST_LOG_TRIVIAL(warning) << "Failed to set local window size!";
	}
}

void Client::open_http2_connection()
{
	send_client_connection_header();
}

void Client::send_client_connection_header()
{
	nghttp2_settings_entry iv[1] =
	{
		{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
	};

	int rv;
	/* client 24 bytes magic string will be sent by nghttp2 library */
	rv = nghttp2_submit_settings(http2_session_, NGHTTP2_FLAG_NONE, iv, ARRLEN(iv));

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "Could not submit SETTINGS: " << nghttp2_strerror(rv);
		throw std::runtime_error("Error while submitting settings");
	}

	rv = session_send();

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "session_send() error: " << nghttp2_strerror(rv);
		throw std::runtime_error("Error while setting session settings");
	}
}

void Client::do_rtsp_connections_accept()
{
	BOOST_LOG_TRIVIAL(debug) << "Start listening for RTSP connections";
	rtsp_listening_port_acceptor_.async_accept(
		[this](boost::system::error_code ec, tcp::socket socket)
	{
		if(!ec)
		{
			BOOST_LOG_TRIVIAL(debug) << "A new RTSP connection accepted from " << socket.remote_endpoint().address().to_string()
				<< ":" << socket.remote_endpoint().port();

			auto rtsp_stream_id{0};

			try
			{
				rtsp_stream_id = init_ws_stream();
			}
			catch(std::runtime_error ec)
			{
				BOOST_LOG_TRIVIAL(error) << ec.what();
				return;
			}

			auto rc = std::make_shared<RtspConnection>(std::move(socket), rtsp_stream_id, this);
			rc->Start();
			rtspConns.push_back(rc);
		}

		do_rtsp_connections_accept();
	}
	);
}

int Client::session_send()
{
	int rv;
	rv = nghttp2_session_send(http2_session_);

	if(rv != 0)
	{
		BOOST_LOG_TRIVIAL(error) << "nghttp2_session_send failed: " << nghttp2_strerror(rv) << std::endl;
		return -1;
	}

	return 0;
}

void Client::do_read()
{
	socket_->async_read_some(boost::asio::buffer(data_, MAX_LENGTH),
		[this](boost::system::error_code ec, std::size_t length)
	{
		if(ec)
		{
			BOOST_LOG_TRIVIAL(error) << "Reading finished with an error";
			return;
		}

		BOOST_LOG_TRIVIAL(trace) << "Received data in bytes: " << length;
		ssize_t readlen = nghttp2_session_mem_recv(http2_session_, (const uint8_t*)data_, length);

		if(readlen < 0)
		{
			BOOST_LOG_TRIVIAL(error) << "Error: " << nghttp2_strerror((int)readlen);
			//delete_http2_session_data(session_data);
			return;
		}

		do_read();
	}
	);
}

// Returns a created stream's ID
int32_t Client::do_write_request(const uint8_t* data, size_t len)
{
	boost::beast::http::request_parser<boost::beast::http::string_body> http_parser;

	boost::beast::error_code ec;
	auto consumed = http_parser.put(boost::asio::buffer(data, len), ec);

	// TODO: add @ec handling correctly. There could be an error because not enough data received
	if(ec) throw std::runtime_error("parsing HTTP 1.1 header error");

	// now parse http body
	consumed = http_parser.put(boost::asio::buffer(data + consumed, len - consumed), ec);

	if(ec) throw std::runtime_error("parsing HTTP 1.1 body error");

	if(!http_parser.is_done() || !http_parser.is_header_done())
		throw std::runtime_error("parsing HTTP 1.1 request is not done!");
	
	std::vector<nghttp2_nv> hdrs =
	{
		MAKE_NV2(":method", "POST"),
		MAKE_NV2(":scheme", "http"),
		//MAKE_NV2(":protocol", "websocket"),
		//MAKE_NV2(":path", http_parser.get().target().data()),
		{(uint8_t*)":path", (uint8_t*)http_parser.get().target().data(), 5, http_parser.get().target().length()},
		MAKE_NV2("host", "192.168.42.170:13080") // TODO: fill this field with real data
	};

	static const std::string HTTP_AUTHORIZATION{"Authorization"};
	std::string auth_value;
	for (auto it = http_parser.get().begin(); it != http_parser.get().end(); ++it)
	{
		if (it->name() == boost::beast::http::field::authorization)
		{
			auth_value = it->value().to_string();
			break;
		}
	}

	if (!auth_value.empty())
	{
		hdrs.push_back(nghttp2_nv{(uint8_t*)HTTP_AUTHORIZATION.c_str(), (uint8_t*)auth_value.c_str(),
			HTTP_AUTHORIZATION.size(), auth_value.size(), NGHTTP2_NV_FLAG_NONE});
	}

	// TODO: free memory
	auto* mydsptr = new MyDataSource;
	mydsptr->dataLen = http_parser.get().body().length();
	mydsptr->data = (const uint8_t*)http_parser.get().body().data();

	nghttp2_data_source dsource;
	dsource.ptr = mydsptr;

	nghttp2_data_provider dprd;
	dprd.source = dsource;
	dprd.read_callback = &on_data_source_read_callback;

	int32_t stream_id = nghttp2_submit_request(http2_session_, NULL, hdrs.data(), hdrs.size(), &dprd, this);

	if(stream_id < 0)
	{
		BOOST_LOG_TRIVIAL(error) << "Could not submit HTTP request: " << nghttp2_strerror(stream_id);
		return stream_id;
	}

	if(session_send() != 0)
	{
		//delete_http2_session_data(session_data);
		throw std::runtime_error("Session send finished with an error");
	}

	return stream_id;
}

int32_t Client::init_ws_stream()
{
	nghttp2_nv hdrs[] =
	{
		MAKE_NV2(":method", "CONNECT"),
		MAKE_NV2(":scheme", "http"),
		MAKE_NV2(":authority", "172.19.214.55:13080"),
		MAKE_NV2(":protocol", "websocket"),
		MAKE_NV2(":path", "/")
	};

	int32_t stream_id = nghttp2_submit_headers(http2_session_, NGHTTP2_DATA_FLAG_NO_END_STREAM, -1,
			NULL, hdrs, ARRLEN(hdrs), this);

	if(stream_id < 0)
	{
		throw std::runtime_error((boost::format("Could not submit RTSP CONNECT request: %1%") % nghttp2_strerror(stream_id))
			.str());
	}

	if(session_send() != 0)
	{
		//delete_http2_session_data(session_data);
		throw std::runtime_error("Session send finished with an error");
	}

	const int32_t _1Mb = 1 * 1024 * 1024;
	int rv = nghttp2_session_set_local_window_size(http2_session_, NGHTTP2_FLAG_NONE, stream_id, _1Mb);
	if (rv != NGHTTP2_NO_ERROR)
	{
		BOOST_LOG_TRIVIAL(warning) << "Failed to set local window size!";
	}

	return stream_id;
}

void Client::do_write_rtsp_data(const uint8_t* data, size_t len, int32_t stream_id)
{
	BOOST_LOG_TRIVIAL(trace) << "it's about to writting some RTSP data";

	// TODO: free memory
	auto* mydsptr = new MyDataSource;

	mydsptr->dataLen = len;
	mydsptr->data = data;

	nghttp2_data_source dsource;
	dsource.ptr = mydsptr;

	nghttp2_data_provider dprd;
	dprd.source = dsource;
	dprd.read_callback = &on_rtsp_data_source_read_callback;

	int ri = nghttp2_submit_data(http2_session_, NGHTTP2_DATA_FLAG_NO_END_STREAM,
			stream_id, &dprd);

	if(ri)
	{
		BOOST_LOG_TRIVIAL(error) << "RTSP stream submitting data finished with an error: " << ri;
	}
}

void Client::remove_vms_connection(int32_t http2_session_id)
{
	rconnections.erase(std::remove_if(rconnections.begin(), rconnections.end(),
			[http2_session_id](std::shared_ptr<VmsConnection>& vc)
	{
		return vc->http2_session_id_ == http2_session_id;
	})
	);
}

ssize_t send_callback(nghttp2_session* session, const uint8_t* data,	size_t length, int flags, void* user_data)
{
	BOOST_LOG_TRIVIAL(trace) << "nghttp2 wanna write something with len: " << length;

	auto* client_instance = (Client*) user_data;
	boost::asio::async_write(*client_instance->socket_, boost::asio::buffer(data, length),
		[client_instance](boost::system::error_code ec, std::size_t length)
	{
		BOOST_LOG_TRIVIAL(trace) << "Send through send_callback: " << length;

		if(ec)
			BOOST_LOG_TRIVIAL(error) << "Error in write";
	}
	);

	return (ssize_t)length;
}

int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
	BOOST_LOG_TRIVIAL(trace) << "Frame received";

	auto* client_instance = (Client*) user_data;

	switch(frame->hd.type)
	{
		case NGHTTP2_SETTINGS:
		{
			BOOST_LOG_TRIVIAL(trace) << "Received SETTINGS, sending response";
			auto rv = client_instance->session_send();

			if(rv != 0)
			{
				BOOST_LOG_TRIVIAL(error) << "SETTINGS response finished with an error: " << nghttp2_strerror(rv);
			}
		}
		break;

		case NGHTTP2_DATA:
		{
			BOOST_LOG_TRIVIAL(trace) << "Received DATA frame";

			if(nghttp2_session_want_write(session))
			{
				auto rv = client_instance->session_send();

				if(rv != 0)
				{
					BOOST_LOG_TRIVIAL(error) << "Session wanted to write something but finished with an error: " << nghttp2_strerror(rv);
				}
			}

			auto stream_id = frame->hd.stream_id;
			auto it = std::find_if(client_instance->rconnections.begin(), client_instance->rconnections.end(),
					[stream_id](const std::shared_ptr<VmsConnection>& rc)
			{
				return rc->http2_session_id_ == stream_id;
			});

			if(it != client_instance->rconnections.end())
			{
				if(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
				{
					BOOST_LOG_TRIVIAL(trace) << "on_frame_recv_callback() called, need to forward response with status code: " << (*it)->response_status_code_
						<< ". Whole buffer size: " << (*it)->http2_data_buffer_.size();
					(*it)->forward_response((*it)->response_status_code_, (const uint8_t*)(*it)->http2_data_buffer_.data().data(),
						(*it)->http2_data_buffer_.size());
					(*it)->http2_data_buffer_.consume((*it)->http2_data_buffer_.size());
				}
				else
				{
					BOOST_LOG_TRIVIAL(trace) << "on_frame_recv_callback() called, flag END_STREAM not received. Waiting for more data. Current buffer size: " << (*it)->http2_data_buffer_.size();
				}
			}

			break;
		}

		default:
			break;
	}

	return 0;
}

int on_header_recv_callback(nghttp2_session* /*session*/, const nghttp2_frame* frame, const uint8_t* name, size_t /*namelen*/, const uint8_t* value, size_t valuelen, uint8_t /*flags*/, void* user_data)
{
	auto client_instance = (Client*)user_data;
	auto rc_it = std::find_if(client_instance->rconnections.begin(), client_instance->rconnections.end(),
			[frame](std::shared_ptr<VmsConnection> rc)
	{
		return rc->http2_session_id_ == frame->hd.stream_id;
	});

	if(rc_it == client_instance->rconnections.end())
	{
		BOOST_LOG_TRIVIAL(warning) << "Not found a stream ID = " << frame->hd.stream_id;
		return 0; // should we return an error code instead 0 here???
	}

	if(0 == strcmp((const char*)name, ":status"))
	{
		(*rc_it)->response_status_code_ = atoi((const char*)value);
	}
	else if (0 == strcmp((const char*)name, "www-authenticate"))
	{
		(*rc_it)->www_auth_ = std::string{(const char*)value, valuelen};
	}

	return NGHTTP2_NO_ERROR;
}

int on_data_chunk_recv_callback(nghttp2_session* http2_session, const uint8_t flags, int32_t stream_id, const uint8_t* data, size_t length, void* user_data)
{
	auto client_instance = (Client*)user_data;

	int32_t local_session_window_size = nghttp2_session_get_local_window_size(http2_session);
	int32_t local_stream_window_size = nghttp2_session_get_stream_local_window_size(http2_session, stream_id);

	BOOST_LOG_TRIVIAL(trace) << "Session and stream window size: " << local_session_window_size << " " << local_stream_window_size;

	auto rtsp_session_it = std::find_if(client_instance->rtspConns.begin(), client_instance->rtspConns.end(),
			[stream_id](const std::shared_ptr<RtspConnection>& rc)
	{
		return rc->StreamID() == stream_id;
	});

	if(rtsp_session_it != client_instance->rtspConns.end())
	{
		(*rtsp_session_it)->forward_data_from_camera(data, length);
		return NGHTTP2_NO_ERROR;
	}

	auto it = std::find_if(client_instance->rconnections.begin(), client_instance->rconnections.end(),
			[stream_id](const std::shared_ptr<VmsConnection>& rc)
	{
		return rc->http2_session_id_ == stream_id;
	});

	if(it != client_instance->rconnections.end())
	{
		// here we just copy incoming data into the inner buffer
		// if you are searching for the place where HTTP requests are forwarded, look in @on_frame_recv_callback
		auto buf = (*it)->http2_data_buffer_.prepare(length);
		memcpy(buf.data(), data, length);
		(*it)->http2_data_buffer_.commit(length);

		BOOST_LOG_TRIVIAL(trace) << "Received data chunk for " << (*it)->http2_session_id_ << " in size: " << length;
	}

	nghttp2_session_consume(http2_session, stream_id, length); // let to know the library data consumed to send WINDOW_UPDATE
	if(nghttp2_session_want_write(client_instance->http2_session_))
	{
		auto rv = client_instance->session_send();

		if(rv != 0)
		{
			BOOST_LOG_TRIVIAL(error) << "Session wanted to write something but finished with an error: " << nghttp2_strerror(rv);
		}
	}

	return NGHTTP2_NO_ERROR;
}

ssize_t on_data_source_read_callback(nghttp2_session* /*session*/, int32_t /*stream_id*/, uint8_t* buf, size_t /*length*/,
	uint32_t* data_flags, nghttp2_data_source* source, void* /*user_data*/)
{
	BOOST_LOG_TRIVIAL(trace) << "Data source read callback called";

	auto myds = (MyDataSource*)source->ptr;

	memcpy(buf, myds->data, myds->dataLen);
	*data_flags = NGHTTP2_DATA_FLAG_EOF;

	return myds->dataLen;
}

ssize_t on_rtsp_data_source_read_callback(nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data)
{
	BOOST_LOG_TRIVIAL(trace) << "RTSP data source read callback called";

	auto myds = (MyDataSource*)source->ptr;

	//assert(length * 1024 >= myds->dataLen);

	memcpy(buf, myds->data, myds->dataLen);
	*data_flags = NGHTTP2_DATA_FLAG_EOF;

	return (ssize_t)myds->dataLen;
}

int main(int argc, char* argv[])
{
	std::cout << "Started..." << std::endl;

	mylog::init();

	try
	{
		if(argc != 4)
		{
			std::cerr << "Usage: uplink_cloud_service <listen http2 port> <listen http port> <listen rtsp port>\n";
			return 1;
		}

		BOOST_LOG_TRIVIAL(info) << "*** New run ***";

		short http2_port = std::atoi(argv[1]);
		short http_port = std::atoi(argv[2]);
		short rtsp_port = std::atoi(argv[3]);

		std::cout << "ports: http2=" << http2_port
			<< "; http_port=" << http_port
			<< "; rtsp_port=" << rtsp_port << std::endl;

		boost::asio::io_context io_context;

		Client c(io_context, http2_port, http_port, rtsp_port);

		std::thread t([&io_context]()
		{
			io_context.run();
		});

		t.join();
		c.close();

		BOOST_LOG_TRIVIAL(info) << "*** Service stopped ***\n";
	}
	catch(std::exception& e)
	{
		BOOST_LOG_TRIVIAL(fatal) << "Finished with an exception: " << e.what() << "\n";
		std::cerr << "Exception: " << e.what() << std::endl;
	}

	std::cout << "Finished!" << std::endl;
	return 0;
}