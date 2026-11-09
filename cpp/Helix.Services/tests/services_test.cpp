#include "../samples/Options.h"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace helix::services;
static void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
class Publisher final : public IEventPublisher
{
public:
	std::mutex mutex;
	std::condition_variable ready;
	DeliveryCallback delayed;
	EnqueueResult TryPublish(const Event& event, DeliveryCallback cb) override
	{
		if (event.key == "full") return EnqueueResult::QueueFull;
		if (event.key == "stopped") return EnqueueResult::Stopped;
		if (event.key == "throw") throw std::runtime_error("Publisher failure");
		if (event.key == "delay")
		{
			std::lock_guard lock(mutex);
			delayed = std::move(cb);
			ready.notify_all();
		}
		else cb({event.key == "fail" ? PublishStatus::Unavailable : PublishStatus::Delivered, event.eventId, 2, 42, "delivery failure"});
		return EnqueueResult::Accepted;
	}
	void Complete()
	{
		DeliveryCallback cb;
		{ std::lock_guard lock(mutex); cb = std::move(delayed); }
		if (cb) cb({PublishStatus::Delivered, "late", 1, 7, {}});
	}
	~Publisher() { Complete(); }
};
static void Env(const char* name, const std::string& value)
{
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	if (value.empty()) unsetenv(name); else setenv(name, value.c_str(), 1);
#endif
}
static void ConfigTest()
{
	const char* key = "HELIX_SERVICES__MAX_PENDING";
	const std::string previous = std::getenv(key) ? std::getenv(key) : "";
	struct Restore { const char* key; std::string previous; ~Restore() { Env(key, previous); } } restore{key, previous};
	Env(key, "");
	helix::config::ConfigLoader loader;
	auto base = loader.Load("");
	Check(base.has_value() && base->serviceMaxPending == 1024, "config defaults");
	auto path = std::filesystem::temp_directory_path() / ("helix-services-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".toml");
	struct Remove { std::filesystem::path path; ~Remove() { std::filesystem::remove(path); } } remove{path};
	{ std::ofstream file(path); file << "[services]\nmax_pending=3\nrpc_target=\"127.0.0.1:7201\"\n[kafka]\ntopic=\"configured\"\n"; }
	auto config = loader.Load(path.string());
	Check(config && sample::Kafka(*config).maxPending == 3 && sample::Kafka(*config).topic == "configured" &&
		sample::Rpc(*config, false).address == "127.0.0.1:7201", "config file consumption");
	Env(key, "7");
	Check(loader.Load(path.string())->serviceMaxPending == 7, "config env priority");
	for (const auto* invalid : {"0", "-1", "7junk", "2147483648", "4294967296"})
	{ Env(key, invalid); Check(!loader.Load(path.string()), "config invalid limit accepted"); }
}
static void RpcTest()
{
	helix::config::EngineConfig config;
	config.serviceMaxPending = 1;
	config.serviceMaxMessageBytes = 4096;
	config.serviceRpcListen = "127.0.0.1:0";
	Publisher publisher;
	EventRpcServer server(sample::Rpc(config, true), publisher);
	server.Start();
	config.serviceRpcTarget = "127.0.0.1:" + std::to_string(server.Port());
	EventRpcClient client(sample::Rpc(config, false));
	Event event{1, "contract", "normal", "test", std::string("a\0b", 3), 3, 123};
	auto delivered = client.Publish(event);
	Check(delivered.status == PublishStatus::Delivered && delivered.offset == 42 && delivered.partition == 2 &&
		delivered.eventId == event.eventId, "RPC delivery reply");
	for (auto test : {std::pair{"full", PublishStatus::QueueFull}, {"stopped", PublishStatus::Unavailable},
		{"fail", PublishStatus::Unavailable}, {"throw", PublishStatus::Unavailable}})
	{ event.key = test.first; Check(client.Publish(event).status == test.second, "RPC failure mapping"); }
	event.version = 9;
	Check(client.Publish(event).status == PublishStatus::Invalid, "unknown event version");
	event.version = 1;
	event.key = "delay";
	config.serviceDeadlineMs = 300;
	EventRpcClient deadlineClient(sample::Rpc(config, false));
	Check(deadlineClient.Publish(event).status == PublishStatus::Deadline, "RPC deadline");
	{
		std::unique_lock lock(publisher.mutex);
		Check(publisher.ready.wait_for(lock, std::chrono::seconds(2), [&] { return !!publisher.delayed; }), "RPC admission before deadline");
	}
	event.key = "normal";
	Check(client.Publish(event).status == PublishStatus::QueueFull, "bounded in-flight RPCs");
	publisher.Complete(); // Deadline did not roll back accepted publication.
	Check(client.Publish(event).status == PublishStatus::Delivered, "RPC capacity released after late delivery");
	server.Stop();
}
static void KafkaFailureTest()
{
	helix::config::EngineConfig config;
	config.kafkaBrokers = "127.0.0.1:1";
	config.serviceMaxPending = 1;
	config.kafkaShutdownTimeoutMs = 20;
	config.kafkaDeliveryTimeoutMs = 1000;
	KafkaProducer producer(sample::Kafka(config));
	std::atomic<int> callbacks{0};
	std::atomic<bool> failed{false};
	Event event{1, "shutdown", "same-key", "test", "payload", 1, 1};
	Check(producer.TryPublish(event, [&](PublishResult result) { ++callbacks; failed = result.status != PublishStatus::Delivered; }) == EnqueueResult::Accepted, "Kafka admission");
	Check(producer.TryPublish(event, [](PublishResult) {}) == EnqueueResult::QueueFull, "Kafka bounded queue");
	producer.Stop();
	producer.Stop();
	Check(callbacks == 1 && failed, "shutdown reports each purged event once");
	Check(producer.TryPublish(event, [](PublishResult) {}) == EnqueueResult::Stopped, "publication after stop");
	event.version = 2;
	Check(producer.TryPublish(event, [](PublishResult) {}) == EnqueueResult::Invalid, "invalid publication");
}
int main()
{
	try { ConfigTest(); RpcTest(); KafkaFailureTest(); std::cout << "service-contracts passed\n"; return 0; }
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
