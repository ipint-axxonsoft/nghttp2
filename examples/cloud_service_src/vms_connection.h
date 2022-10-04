#pragma once

#include <websocket_cpp/ws.hpp>

#include <boost/asio.hpp>

#include <memory>

namespace
{
	using boost::asio::ip::tcp;
}

extern const uint32_t RTSP_STREAM_ID;

class Client;
struct VmsConnection : public std::enable_shared_from_this<VmsConnection>
{
	VmsConnection(tcp::socket socket, Client* parent);
	void Start();
	~VmsConnection();
	void do_read();
	void do_write(size_t len);
	void forward_response(int status, const uint8_t* data, size_t datalen);

	//private:
	tcp::socket socket_;
	Client* parent_;
	enum {MAX_LENGTH = 65536};
	char data_[MAX_LENGTH];
	boost::asio::streambuf http2_data_buffer_;
	int32_t http2_session_id_; // TODO: rename on http2_stream_id_;
	int32_t response_status_code_ = 200; // let's use 200 as a default
	std::string www_auth_;

    //size_t expected_data_length_to_recv = 0;
    bool is_the_last_data_frame_received = false;
};

struct RtspConnection : public std::enable_shared_from_this<RtspConnection>
{
	RtspConnection(tcp::socket socket, int32_t http2_stream_id, Client* parent);
	~RtspConnection();

	void Start();
	void do_read();

	void forward_data_from_camera(const uint8_t* data, size_t datalen);

	inline int32_t StreamID() const
	{
		return http2_stream_id_;
	}

private:
	void onWsWrappedDataReady(const char* data, size_t dataLen);
	void onWsIncomingDataReady(const char* data, size_t dataLen);

public:

	// TODO: make all members private:
	tcp::socket socket_;
private:
	int32_t http2_stream_id_;
	std::string remote_endpoint_;

public:
	Client* parent_;
	enum {MAX_LENGTH = 65536};
	char data_[MAX_LENGTH];

	ws::Client wsClient_;
	char wsOutputBuffer[MAX_LENGTH];
	size_t wrappedDataLen = 0;
};



