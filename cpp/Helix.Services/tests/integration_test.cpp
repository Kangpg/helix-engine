#include "../samples/Options.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace helix::services;
static void Check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
static Event Expected(int i)
{
	return {1, "event-" + std::to_string(i), "player-123", "inventory.changed",
		std::string("binary\0payload", 14) + std::to_string(i), 0xffffff, 18446744073709551000ull};
}
static void Match(const Event& event, int i)
{
	auto expected = Expected(i);
	Check(event.version == expected.version && event.eventId == expected.eventId && event.key == expected.key &&
		event.type == expected.type && event.payload == expected.payload && event.producerNodeId == expected.producerNodeId &&
		event.occurredUnixMs == expected.occurredUnixMs, "event envelope/order mismatch at " + std::to_string(i));
}
static ConsumeResult Await(KafkaConsumer& consumer, const std::function<bool(const Event&)>& handler)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
	while (std::chrono::steady_clock::now() < deadline)
	{
		auto result = consumer.Poll(100, handler);
		if (result == ConsumeResult::Error) throw std::runtime_error(consumer.GetError());
		if (result != ConsumeResult::Idle) return result;
	}
	throw std::runtime_error("Timed out awaiting Kafka event");
}
int main(int argc, char** argv)
{
	try
	{
		Check(argc == 2, "usage: helix_services_integration_test <broker-host:port>");
		helix::config::EngineConfig config;
		config.kafkaBrokers = argv[1];
		config.kafkaTopic = "helix.integration." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
		config.kafkaConsumerGroup = config.kafkaTopic + ".primary";
		config.kafkaDeliveryTimeoutMs = 15000;
		config.serviceDeadlineMs = 20000;
		config.serviceRpcListen = "127.0.0.1:0";
		KafkaProducer producer(sample::Kafka(config));
		EventRpcServer server(sample::Rpc(config, true), producer);
		server.Start();
		config.serviceRpcTarget = "127.0.0.1:" + std::to_string(server.Port());
		EventRpcClient client(sample::Rpc(config, false));
		std::int64_t previousOffset = -1;
		int partition = -1;
		for (int i = 0; i < 8; ++i)
		{
			auto result = client.Publish(Expected(i));
			Check(result.status == PublishStatus::Delivered, "broker acknowledgement: " + result.error);
			Check(result.eventId == Expected(i).eventId && result.offset > previousOffset, "acknowledgement metadata");
			if (partition >= 0) Check(result.partition == partition, "same-key partition ordering");
			partition = result.partition; previousOffset = result.offset;
		}
		std::cout << "gRPC -> Kafka broker acknowledgement and key order passed\n" << std::flush;
		{
			KafkaConsumer consumer(sample::Kafka(config));
			Check(Await(consumer, [](const Event& event) { Match(event, 0); return false; }) == ConsumeResult::Retry, "handler rejection");
			Check(Await(consumer, [&](const Event& event) { Match(event, 0); consumer.Poll(1, [](const Event&) { return true; }); return true; }) == ConsumeResult::Retry, "reentrant handler retry");
			Check(Await(consumer, [](const Event& event) -> bool { Match(event, 0); throw std::runtime_error("transient failure"); }) == ConsumeResult::Retry, "handler exception retry");
			for (int i = 0; i < 8; ++i)
				Check(Await(consumer, [i](const Event& event) { Match(event, i); return true; }) == ConsumeResult::Committed, "commit after success");
		}
		std::cout << "binary envelope, rejection/exception retry and commits passed\n" << std::flush;
		{
			KafkaConsumer restarted(sample::Kafka(config));
			for (int i = 0; i < 20; ++i)
				Check(restarted.Poll(100, [](const Event&) { throw std::runtime_error("committed event replayed"); return false; }) == ConsumeResult::Idle, "restart replayed committed record");
			Check(client.Publish(Expected(8)).status == PublishStatus::Delivered, "post-restart publication");
			Check(Await(restarted, [](const Event& event) { Match(event, 8); return true; }) == ConsumeResult::Committed, "resume committed group");
		}
		config.kafkaConsumerGroup += ".replay";
		{
			KafkaConsumer replay(sample::Kafka(config));
			for (int i = 0; i < 9; ++i)
				Check(Await(replay, [i](const Event& event) { Match(event, i); return true; }) == ConsumeResult::Committed, "new-group replay");
		}
		producer.Stop();
		server.Stop();
		std::cout << "consumer restart and independent-group replay passed\nservice integration passed\n";
		return 0;
	}
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
