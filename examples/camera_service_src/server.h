#pragma once

#include "session.h"

#include <nghttp2/nghttp2.h>

#include <boost/system/error_code.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/http.hpp>

#include <iostream>
#include <memory>
#include <utility>
#include <functional>

#define ARRLEN(x) (sizeof(x) / sizeof(x[0]))

#define MAKE_NV(NAME, VALUE)                                                   \
	{                                                                            \
	(uint8_t *)NAME, (uint8_t *)VALUE, sizeof(NAME) - 1, sizeof(VALUE) - 1,    \
		NGHTTP2_NV_FLAG_NONE                                                   \
}

class Server
{
public:
	enum class CONNECTION_TYPE
	{
		SECURED,
		PLAIN
	};

	Server(boost::asio::io_context& io_context,
		const boost::asio::ip::tcp::resolver::results_type& endpoint,
		CONNECTION_TYPE conn_type,
		const boost::asio::ip::tcp::resolver::results_type& camera_endpoint,
		const boost::asio::ip::tcp::resolver::results_type& rtsp_endpoints);

	void Start()
	{
		io_context_.run();
	}

private:
	void do_connect(const boost::asio::ip::tcp::resolver::results_type& endpoints);

private:
	CONNECTION_TYPE conn_type_;
	boost::asio::io_context& io_context_;
	boost::asio::ssl::context ssl_ctx_;

	boost::asio::ip::tcp::resolver::results_type camera_endpoints_;
	boost::asio::ip::tcp::resolver::results_type camera_rtsp_endpoint_;
};