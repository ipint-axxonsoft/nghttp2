
#include "client.h"

#include "log.h"

#include <boost/asio.hpp>
#include <boost/program_options.hpp>

#include <iostream>

namespace po = boost::program_options;

/* Function used to check that 'opt1' and 'opt2' are not specified
   at the same time. */
void conflicting_options(const po::variables_map& vm, 
	const char* opt1, const char* opt2)
{
	if (vm.count(opt1) && !vm[opt1].defaulted() 
		&& vm.count(opt2) && !vm[opt2].defaulted())
		{
			throw std::logic_error(std::string("Conflicting options '")
				+ opt1 + "' and '" + opt2 + "'.");
		}
}

int main(int argc, char* argv[])
{
	std::cout << "Starting" << std::endl;

	mylog::init();

	try
	{
		po::options_description config{"Options"};
		config.add_options()
		("help", "Help screen")
		("http2-tls-port", po::value<unsigned short>(), "HTTP2 TLS port")
		("http2-port", po::value<unsigned short>(), "HTTP2 non TLS port")
		("http-port", po::value<unsigned short>()->default_value(8080), "HTTP port")
		("rtsp-port", po::value<unsigned short>()->default_value(8554), "RTSP port");

		po::variables_map vm;
		po::store(parse_command_line(argc, argv, config), vm);
		po::notify(vm);

		if(vm.count("help")
			|| (!vm.count("http2-tls-port") && !vm.count("http2-port")))
		{
			std::cerr << config << std::endl;
			return 1;
		}

		// also check if client has not set http2-tls-port and http2-port at the same time
		conflicting_options(vm, "http2-tls-port", "http2-port");

		BOOST_LOG_TRIVIAL(info) << "*** New run ***";

		boost::asio::io_context io_context;

		std::unique_ptr<Client> c;
		if (vm.count("http2-tls-port")) //secured h2 connections will be used
		{
			c = std::make_unique<Client>(io_context,
				Client::SecuredConnection{vm["http2-tls-port"].as<unsigned short>()},
				vm["http-port"].as<unsigned short>(),
				vm["rtsp-port"].as<unsigned short>());
		}
		else
		{
			c = std::make_unique<Client>(io_context,
				Client::PlainConnection{vm["http2-port"].as<unsigned short>()},
				vm["http-port"].as<unsigned short>(),
				vm["rtsp-port"].as<unsigned short>());
		}

		std::thread t([&io_context]()
		{
			io_context.run();
		});

		t.join();
		c->close();

		BOOST_LOG_TRIVIAL(info) << "*** Service stopped ***\n";
	}
	catch(const std::exception& e)
	{
		BOOST_LOG_TRIVIAL(fatal) << "Finished with an exception: " << e.what() << "\n";
		std::cerr << "Exception: " << e.what() << '\n';
	}

	std::cout << "Finished!" << std::endl;
	return 0;
}