#pragma once

#include <websocket_cpp/ws.hpp>

#include <nghttp2/nghttp2.h>

#include <boost/asio.hpp>

#include <memory>
#include <vector>
#include <string>

extern const uint32_t RTSP_STREAM_ID;

namespace
{
	using boost::asio::ip::tcp;
}

class Session;

class RTSPConnection : public std::enable_shared_from_this<RTSPConnection>
{
public:
	RTSPConnection(int32_t streamID, std::shared_ptr<tcp::socket>&& socket, Session* parent);
	RTSPConnection(RTSPConnection&&) = default;

	inline int32_t StreamID() const
	{
		return stream_id_;
	}

	void process_rtsp_data_from_vms(const uint8_t* data, size_t length);

	inline bool isReady() const
	{
		return rtsp_socket_ != nullptr;
	}

	void do_read_rtsp_data_from_camera();

	void do_submit_data();

private:
	void wsIncomingDataReadyCb(const char* data, size_t dataLen);
	void wsWrappedDataReadyCb(const char* data, size_t dataLen);

private:
	int32_t stream_id_ = -1;
	std::shared_ptr<tcp::socket> rtsp_socket_;
	Session* parent_ = nullptr;
	std::unique_ptr<ws::Server> ws_;

	std::string local_endpoint_;

	enum { max_length = 65536 };
	char rtsp_read_buffer_[max_length];

	boost::asio::streambuf rtsp_write_buffer_;
};

class HTTPConnection : public std::enable_shared_from_this<HTTPConnection>
{
public:
	HTTPConnection(int32_t streamID, tcp::resolver::results_type camera_endpoints, Session* parent);

	inline int32_t StreamID() const
	{
		return stream_id_;
	}

	inline void SetPath(const std::string& path) { path_ = path; }
	inline void SetAuthentication(const std::string& auth) { auth_ = auth; }

	void start();
	void forward_request();
	void read_from_camera();
	void appendData(const char* data, size_t length);

	inline const char* Data() const
	{
		return buffer_;
	}

	inline size_t Length() const
	{
		return length_;
	}

private:
	int32_t stream_id_ = -1;
	std::string path_;
	std::string auth_;

	const tcp::resolver::results_type camera_endpoints_;
	std::unique_ptr<tcp::socket> socket_;
	Session* parent_ = nullptr;
	std::string local_endpoint_;

	std::string remote_ipv4_;

	enum { max_length = 65536 };
	char buffer_[max_length];
	size_t length_ = 0;
	boost::asio::streambuf innerBuffer_;	
};

class Session
	: public std::enable_shared_from_this<Session>
{
public:
	Session(tcp::socket socket,
		const tcp::resolver::results_type& camera_endpoints,
		const tcp::resolver::results_type& camera_rtsp_endpoint,
		boost::asio::io_context& io_ctx);

	~Session();

	std::shared_ptr<Session> copy()
	{
		return shared_from_this();
	}

	int send_server_connection_header();
	int send_data(int32_t stream_id, const uint8_t* data, size_t datalen);
	int forward_response(int32_t stream_id, short status_code, const uint8_t* data, size_t datalen, std::vector<std::pair<std::string, std::string>> hdrs);
	/* Serialize the frame and send (or buffer) the data to	bufferevent. */
	int session_send();
	void Start();

public:
	void do_read();
	void do_write(std::size_t length);

	void do_read_on_camera_socket(int32_t stream_id);

	void process_rtsp_data_from_vms(const uint8_t* data, size_t length);

	void connect_rtsp(int32_t http2_stream_id);

public:
	tcp::socket socket_;
	enum { max_length = 65536 };
	char data_[max_length];

	nghttp2_session* http2_session_;

	const tcp::resolver::results_type camera_endpoints_;
	const tcp::resolver::results_type camera_rtsp_endpoint_;

	std::string local_endpoint_;

	boost::asio::io_context& io_context_;

	std::shared_ptr<tcp::socket> camera_socket_;
	std::vector<std::shared_ptr<RTSPConnection>> rtsp_conns_;

	std::vector<std::shared_ptr<HTTPConnection>> http_conns_;
};
