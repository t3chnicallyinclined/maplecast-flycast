// Minimal WebSocket client test — connects to ws://127.0.0.1:7200 and prints received frames
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <cstdio>
#include <string>
#include <atomic>

using WsClient = websocketpp::client<websocketpp::config::asio_client>;

static std::atomic<int> frames{0};

int main()
{
	WsClient client;
	client.clear_access_channels(websocketpp::log::alevel::all);
	client.clear_error_channels(websocketpp::log::elevel::all);
	client.init_asio();

	client.set_open_handler([](websocketpp::connection_hdl) {
		printf("CONNECTED\n");
		fflush(stdout);
	});
	client.set_fail_handler([&](websocketpp::connection_hdl hdl) {
		auto con = client.get_con_from_hdl(hdl);
		printf("FAILED: %s (HTTP %d)\n", con->get_ec().message().c_str(), con->get_response_code());
		fflush(stdout);
	});
	client.set_close_handler([](websocketpp::connection_hdl) {
		printf("CLOSED\n");
		fflush(stdout);
	});
	client.set_message_handler([](websocketpp::connection_hdl, WsClient::message_ptr msg) {
		int n = frames.fetch_add(1);
		if (n < 5 || n % 60 == 0)
			printf("Frame %d: %zu bytes (%s)\n", n, msg->get_payload().size(),
				msg->get_opcode() == websocketpp::frame::opcode::binary ? "binary" : "text");
		fflush(stdout);
	});

	websocketpp::lib::error_code ec;
	auto con = client.get_connection("ws://127.0.0.1:7200", ec);
	if (ec) { printf("get_connection error: %s\n", ec.message().c_str()); return 1; }

	client.connect(con);
	printf("Running...\n");
	fflush(stdout);
	client.run();
	return 0;
}
