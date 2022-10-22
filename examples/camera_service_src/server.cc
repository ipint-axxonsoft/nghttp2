#include "server.h"

#include "log.h"

#include <boost/asio/ssl.hpp>

namespace
{
	using boost::asio::ip::tcp;
	using ssl_socket = boost::asio::ssl::stream<tcp::socket> ;
}

Server::Server(boost::asio::io_context& io_context,
	const tcp::resolver::results_type& endpoints,
	CONNECTION_TYPE conn_type,
	const tcp::resolver::results_type& camera_endpoints,
	const tcp::resolver::results_type& rtsp_endpoint)
	: conn_type_(conn_type)
	, io_context_(io_context)
	, ssl_ctx_(boost::asio::ssl::context::sslv23)
	, camera_endpoints_(camera_endpoints)
	, camera_rtsp_endpoint_(rtsp_endpoint)
{
	ssl_ctx_.set_verify_mode(boost::asio::ssl::verify_none);
	do_connect(endpoints);
}

void Server::do_connect(const tcp::resolver::results_type& endpoints)
{
	if (conn_type_ == CONNECTION_TYPE::SECURED)
	{ 
		auto socket = std::make_shared<ssl_socket>(io_context_, ssl_ctx_);
		boost::system::error_code ec;
		boost::asio::connect(socket->lowest_layer(), endpoints, ec);
		if (ec)
		{
			BOOST_LOG_TRIVIAL(fatal) << "Failed to connect: " << ec.message();
			return;
		}

		socket->handshake(ssl_socket::client, ec);
		if (ec)
		{
			BOOST_LOG_TRIVIAL(fatal) << "SSL handshake failed: " << ec.message();
			return;
		}

		std::make_shared<Session>(socket,
			camera_endpoints_, camera_rtsp_endpoint_, io_context_)->Start();

		return;
	}

	auto socket = std::make_shared<tcp::socket>(io_context_);
	boost::asio::async_connect(*socket, endpoints,
		[this, socket](boost::system::error_code ec, tcp::endpoint)
	{
		if(ec)
		{
			BOOST_LOG_TRIVIAL(fatal) << "Failed to connect: " << ec.message();
			return;
		}

		std::make_shared<Session>(socket,
			camera_endpoints_, camera_rtsp_endpoint_, io_context_)->Start();
	});
}