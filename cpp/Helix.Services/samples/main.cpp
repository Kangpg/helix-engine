#include "Options.h"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
	using namespace helix::services;
	if (argc < 3)
	{
		std::cout << "helix_service_demo <serve|publish|consume> <config.toml> [event_id key type payload]\n"
			<< "serve: press Enter to drain and stop. consume: processes until interrupted.\n"
			<< "Use trusted private endpoints; this sample uses insecure gRPC and PLAINTEXT Kafka.\n";
		return argc == 2 && std::string(argv[1]) == "--help" ? 0 : 1;
	}
	try
	{
		helix::config::ConfigLoader loader;
		auto config = loader.Load(argv[2]);
		if (!config) throw std::runtime_error(loader.GetError());
		const std::string mode = argv[1];
		if (mode == "serve")
		{
			KafkaProducer producer(sample::Kafka(*config));
			EventRpcServer server(sample::Rpc(*config, true), producer);
			server.Start();
			std::cout << "Listening on " << server.Port() << "; Enter to stop\n" << std::flush;
			std::string line;
			std::getline(std::cin, line);
			producer.Stop();
			server.Stop();
		}
		else if (mode == "publish" && argc == 7)
		{
			EventRpcClient client(sample::Rpc(*config, false));
			auto result = client.Publish({1, argv[3], argv[4], argv[5], argv[6], config->nodeId, 0});
			if (result.status != PublishStatus::Delivered) throw std::runtime_error(result.error);
			std::cout << result.eventId << " partition=" << result.partition << " offset=" << result.offset << '\n';
		}
		else if (mode == "consume")
		{
			KafkaConsumer consumer(sample::Kafka(*config));
			for (;;)
			{
				auto result = consumer.Poll(config->kafkaPollIntervalMs, [](const Event& event)
				{
					std::cout << event.eventId << " key=" << event.key << " type=" << event.type << '\n' << std::flush;
					return true;
				});
				if (result == ConsumeResult::Error || result == ConsumeResult::Retry)
					throw std::runtime_error(consumer.GetError());
			}
		}
		else throw std::invalid_argument("Invalid mode or arguments; use --help");
		return 0;
	}
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
