#include "log.h"

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>


#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/thread.hpp>

namespace logging = boost::log;
namespace src = boost::log::sources;
namespace sinks = boost::log::sinks;
namespace keywords = boost::log::keywords;
namespace expr = boost::log::expressions;


void mylog::init()
{
	logging::add_file_log
	(
		keywords::file_name = "uplink_camera_service_%N.log",
		keywords::rotation_size = 100 * 1024 * 1024,
		keywords::time_based_rotation = sinks::file::rotation_at_time_point(0, 0, 0),
		keywords::format =
			(
				expr::stream
				<< "[" << expr::attr< boost::posix_time::ptime >("TimeStamp")
				<< "][" << expr::attr<logging::attributes::current_thread_id::value_type>("ThreadID")
				<< "][" << logging::trivial::severity
				<< "]: " << expr::smessage
			),
		keywords::open_mode = std::ios_base::app,
		keywords::auto_flush = true
	);

	logging::core::get()->set_filter
	(
		logging::trivial::severity >= logging::trivial::trace
	);

	logging::add_common_attributes();
}
