#include "server.h"

#include "log.h"

int main(int argc, char* argv[])
{
	std::cout << "Started" << std::endl;
	mylog::init();

	boost::asio::io_service ioservice;
	boost::asio::io_service::work work(ioservice);

	try
	{
		if(argc != 6)
		{
			std::cerr << "Usage: uplink_camera_service <cloud server ip address> <cloud server http2 port> <camera ip> <camera http port> <camera rtsp port>\n";
			return 1;
		}

		BOOST_LOG_TRIVIAL(info) << "*** New run ***";

		boost::asio::io_context io_context;

		tcp::resolver resolver(io_context);
		auto http2_host_endpoint = resolver.resolve(argv[1], argv[2]);
		auto camera_endpoint = resolver.resolve(argv[3], argv[4]);
		auto rtsp_endpoint = resolver.resolve(argv[3], argv[5]);

		Server s(io_context, http2_host_endpoint, camera_endpoint, rtsp_endpoint);
		s.Start();
	}
	catch(std::exception& e)
	{
		BOOST_LOG_TRIVIAL(fatal) << "Finished with an exception:: " << e.what() << "\n";
		std::cerr << "Finished with an exception: " << e.what() << std::endl;
	}

	BOOST_LOG_TRIVIAL(info) << "*** Stopped ***\n";
	std::cout << "finished\n";
}

Server::Server(boost::asio::io_context& io_context,
	const tcp::resolver::results_type& endpoints,
	const tcp::resolver::results_type& camera_endpoints,
	const tcp::resolver::results_type& rtsp_endpoint)
	: io_context_(io_context), socket_(io_context)
	, camera_endpoints_(camera_endpoints)
	, camera_rtsp_endpoint_(rtsp_endpoint)
{
	do_connect(endpoints);
}

void Server::do_connect(const tcp::resolver::results_type& endpoints)
{
	boost::asio::async_connect(socket_, endpoints,
		[this](boost::system::error_code ec, tcp::endpoint)
	{
		if(ec)
		{
			BOOST_LOG_TRIVIAL(fatal) << "Failed to connect: " << ec.message();
			return;
		}

		std::make_shared<Session>(std::move(socket_), camera_endpoints_, camera_rtsp_endpoint_, io_context_)->Start();
	});
}


