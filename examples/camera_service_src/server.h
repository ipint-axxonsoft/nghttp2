#pragma once

#include "session.h"

#include <nghttp2/nghttp2.h>

#include <boost/system/error_code.hpp>
#include <boost/asio.hpp>
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

namespace
{
	using boost::asio::ip::tcp;
}

class Server
{
public:
	Server(boost::asio::io_context& io_context,
		const tcp::resolver::results_type& endpoint,
		const tcp::resolver::results_type& camera_endpoint,
		const tcp::resolver::results_type& rtsp_endpoints);

	void Start()
	{
		io_context_.run();
	}

private:
	void do_connect(const tcp::resolver::results_type& endpoints);

private:
	boost::asio::io_context& io_context_;
	tcp::socket socket_;

	tcp::resolver::results_type camera_endpoints_;
	tcp::resolver::results_type camera_rtsp_endpoint_;
};

