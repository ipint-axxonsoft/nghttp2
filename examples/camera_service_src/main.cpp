#include "server.h"
#include "log.h"

#include <boost/asio.hpp>
#include <boost/program_options.hpp>

#include <iostream>

namespace po = boost::program_options;

/* Function used to validate command line arguments. */
void validate(const po::variables_map& vm)
{
	auto protocol_value = vm["http2-protocol"].as<std::string>();
	if (protocol_value != "tls" && protocol_value != "plain")
		throw po::validation_error(po::validation_error::invalid_option_value);
}

int main(int argc, char* argv[])
{
	using boost::asio::ip::tcp;

	std::cout << "Started" << std::endl;
	mylog::init();

	boost::asio::io_service ioservice;
	boost::asio::io_service::work work(ioservice);

	try
	{
		po::options_description config{"Options"};
		config.add_options()
		("help", "Help screen")
		("cloud-ip", po::value<std::string>()->required(), "Cloud server's IP")
		("http2-port", po::value<std::string>()->required(), "HTTP2 port")
		("http2-protocol", po::value<std::string>()->default_value("tls"), "'tls' or 'plain'")
		("camera-ip", po::value<std::string>()->required(), "Camera IP")
		("camera-http-port", po::value<std::string>()->default_value("80"), "HTTP port")
		("camera-rtsp-port", po::value<std::string>()->default_value("554"), "RTSP port");

		po::variables_map vm;
		po::store(parse_command_line(argc, argv, config), vm);
		po::notify(vm);

		if(vm.count("help"))
		{
			std::cerr << config << std::endl;
			return 1;
		}
		
		validate(vm);

		BOOST_LOG_TRIVIAL(info) << "*** New run ***";

		boost::asio::io_context io_context;

		tcp::resolver resolver(io_context);
		auto http2_host_endpoint = resolver.resolve(vm["cloud-ip"].as<std::string>(),
			vm["http2-port"].as<std::string>());
		auto camera_endpoint = resolver.resolve(vm["camera-ip"].as<std::string>(),
			vm["camera-http-port"].as<std::string>());
		auto rtsp_endpoint = resolver.resolve(vm["camera-ip"].as<std::string>(),
			vm["camera-rtsp-port"].as<std::string>());

		auto connType = vm["http2-protocol"].as<std::string>() == "tls"
			? Server::CONNECTION_TYPE::SECURED : Server::CONNECTION_TYPE::PLAIN;

		Server s(io_context, http2_host_endpoint, connType, camera_endpoint, rtsp_endpoint);
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