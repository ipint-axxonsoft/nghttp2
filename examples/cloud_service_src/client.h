#pragma once

#include "vms_connection.h"

#include <nghttp2/nghttp2.h>

#include <boost/asio.hpp>

#include <memory>

#define ARRLEN(x) (sizeof(x) / sizeof(x[0]))

#define MAKE_NV(NAME, VALUE, VALUELEN)                                         \
	{                                                                            \
	uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, VALUELEN,             \
		NGHTTP2_NV_FLAG_NONE                                                   \
	}

#define MAKE_NV2(NAME, VALUE)                                                  \
	{                                                                            \
	(uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1,    \
		NGHTTP2_NV_FLAG_NONE                                                   \
	}

namespace
{
	using boost::asio::ip::tcp;
}

struct MyDataSource
{
	const uint8_t* data;
	size_t dataLen;
};

class Client
{
public:
	Client(boost::asio::io_context& io_context, short http2_port, short requests_listening_port, short rtsp_listening_port);

private:
	void do_accept();
	void do_requests_port_accept();
	void init_nghttp2();
	void open_http2_connection();
	void send_client_connection_header();

	void do_rtsp_connections_accept();

public:

	int session_send();
	void do_read();

	void close() {}
	int32_t do_write_request(const uint8_t* data, size_t len);
	int32_t init_ws_stream();
	void do_write_rtsp_data(const uint8_t* data, size_t len, int32_t stream_id);

	void remove_vms_connection(int32_t http2_stream_id);

	//private:
	boost::asio::io_context& io_context_;
	tcp::acceptor acceptor_;
	tcp::acceptor requests_listening_port_acceptor_;
	tcp::acceptor rtsp_listening_port_acceptor_;

	std::unique_ptr<boost::asio::ip::tcp::socket> socket_;

	nghttp2_session* http2_session_;

	enum { MAX_LENGTH = 65536};
	char data_[MAX_LENGTH];

	std::vector<std::shared_ptr<VmsConnection>> rconnections;
	std::vector<std::shared_ptr<RtspConnection>> rtspConns;
};


ssize_t send_callback(nghttp2_session* session, const uint8_t* data, size_t length, int flags, void* user_data);
int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
int on_header_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags, void* user_data);
int on_data_chunk_recv_callback(nghttp2_session* http2_session, const uint8_t flags, int32_t stream_id, const uint8_t* data, size_t length, void* user_data);
ssize_t on_data_source_read_callback(nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data);
ssize_t on_rtsp_data_source_read_callback(nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data);

