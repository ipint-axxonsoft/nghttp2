#include "vms_connection.h"

#include "log.h"
#include "client.h"

#include <boost/beast/http.hpp>
#include <boost/format.hpp>

#include <iostream>

const uint32_t RTSP_STREAM_ID = 1;

VmsConnection::VmsConnection(tcp::socket socket, Client* parent)
	: socket_(std::move(socket))
	, parent_(parent)
{
}

void VmsConnection::Start()
{
	do_read();
};

VmsConnection::~VmsConnection()
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__;
}

void VmsConnection::do_read()
{
	socket_.async_read_some(boost::asio::buffer(data_, MAX_LENGTH),
		[self = shared_from_this()](boost::system::error_code ec, std::size_t length)
		{
			if(!ec)
			{
				// TODO: it's need to refactore this to wait for all data in size specified in HTTP 1.1 Content-Length will be received
				BOOST_LOG_TRIVIAL(trace) << "Received request to forward with len " << length;
				self->http2_session_id_ = self->parent_->do_write_request((const uint8_t*)self->data_, length);
				return self->do_read();
			}
			else if(ec == boost::asio::error::eof)
			{
				BOOST_LOG_TRIVIAL(debug) << "VMS connection closed on: " << self->socket_.remote_endpoint().address().to_string()
					<< ":" << self->socket_.remote_endpoint().port();
			}
			else
			{
				BOOST_LOG_TRIVIAL(error) << "Error while reading on socket " << self->socket_.remote_endpoint().address().to_string()
					<< ":" << self->socket_.remote_endpoint().port() << ". Errc and msg: " << ec.value() << " " << ec.message();
			}

			self->parent_->remove_vms_connection(self->http2_session_id_);
		});
}

void VmsConnection::forward_response(int status, const uint8_t* data, size_t datalen)
{
	BOOST_LOG_TRIVIAL(trace) << "Received data to forward with datalen: " << datalen;

	// Set up the response
	using namespace boost::beast;
	auto res = std::make_shared<http::response<http::string_body>>();
	res->version(11);
	res->set(http::field::server, "MUX/DEMUX service");
	res->result(status);
	res->set(http::field::content_type, "application/soap+xml; charset=utf-8;");

	if (!www_auth_.empty())
	{
		res->set(http::field::www_authenticate, www_auth_);
	}

	if(datalen)
	{
		res->set(http::field::content_length, datalen);
		res->body().assign((const char*)data, datalen);
	}

	http::async_write(socket_, *res, [res](error_code ec, size_t bytes_transf)
	{
		if(ec)
			BOOST_LOG_TRIVIAL(error) << "An error occurred while forwarding a response";
		else
		{
			BOOST_LOG_TRIVIAL(trace) << "Forwarded response in size: " << bytes_transf;
		}
	});
}

RtspConnection::RtspConnection(tcp::socket socket, int32_t http2_stream_id, Client* parent)
	: socket_(std::move(socket))
	, http2_stream_id_(http2_stream_id)
	, parent_(parent)
	, wsClient_(
			std::bind(&RtspConnection::onWsIncomingDataReady, this, std::placeholders::_1, std::placeholders::_2),
			std::bind(&RtspConnection::onWsWrappedDataReady, this, std::placeholders::_1, std::placeholders::_2))
{
	remote_endpoint_ = "Failed to determine remote endpont";

	try
	{
		remote_endpoint_ = (boost::format("%1%:%2%")
				% socket_.remote_endpoint().address().to_string()
				% socket_.remote_endpoint().port())
			.str();
	}
	catch(const std::exception& e)
	{
		BOOST_LOG_TRIVIAL(warning) << "Failed to determine remote endpoint address for logging. What: " << e.what();
	}

}

RtspConnection::~RtspConnection()
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__;
}

void RtspConnection::Start()
{
	do_read();
}

void RtspConnection::do_read()
{
	socket_.async_read_some(boost::asio::buffer(data_, MAX_LENGTH),
		[self = shared_from_this()](boost::system::error_code ec, std::size_t length)
	{
		if(!ec)
		{
			BOOST_LOG_TRIVIAL(trace) << "Received RTSP data to forward with len " << length;

			self->wsClient_.WrapData(self->data_, length);

			return self->do_read();
		}
		else if(ec == boost::asio::error::eof)
		{
			BOOST_LOG_TRIVIAL(debug) << "RTSP connection closed on: " << self->remote_endpoint_;
		}
		else
		{
			BOOST_LOG_TRIVIAL(error) << "Error while reading on RTSP socket " << self->remote_endpoint_
				<< ". Errc and msg: " << ec.value() << " " << ec.message();
		}

		//TODO: self->parent_->remove_rtsp_connection(RTSP_STREAM_ID);
	});
}

void RtspConnection::forward_data_from_camera(const uint8_t* data, size_t datalen)
{
	wsClient_.SubmitChunk((const char*)data, datalen);
}

void RtspConnection::onWsWrappedDataReady(const char* data, size_t dataLen)
{
	BOOST_LOG_TRIVIAL(trace) << "Wrapped data is ready in length: " << dataLen;
	parent_->do_write_rtsp_data((const uint8_t*)data, dataLen, http2_stream_id_);

	if(parent_->session_send())
	{
		BOOST_LOG_TRIVIAL(error) << "Sending RTSP wrapped data finished with an error";
	}
}

void RtspConnection::onWsIncomingDataReady(const char* data, size_t dataLen)
{
	if(socket_.is_open())
	{
		boost::asio::async_write(socket_, boost::asio::buffer(data, dataLen),
			[](boost::system::error_code ec, size_t bytes)
		{
			if(ec)
			{
				BOOST_LOG_TRIVIAL(error) << "An error occurred while forwarding RTSP data from a camera: "
					<< ec.message() << ". Errc: " << ec.value();
				return;
			}

			BOOST_LOG_TRIVIAL(trace) << "Forwarded RTSP data in size: " << bytes;
		});
	}
	else
	{
		BOOST_LOG_TRIVIAL(error) << "A RTSP socket no open";
	}
}
