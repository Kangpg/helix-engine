#pragma once
#include <helix/services/event.h>
#include <memory>

namespace helix::services
{
	struct RpcOptions
	{
		std::string address;
		std::uint32_t maxPending = 0;
		std::uint32_t maxMessageBytes = 0;
		std::uint32_t deadlineMs = 0;
	};
	// Local/private development endpoint with insecure gRPC credentials.
	// Cancellation/deadline does not roll back an event already accepted by Kafka.
	class EventRpcServer final
	{
	public:
		EventRpcServer(RpcOptions options, IEventPublisher& publisher);
		~EventRpcServer();
		void Start();
		// Stop accepting requests, then wait for accepted publisher completions.
		// Stop the publisher first to drain accepted events before destroying it.
		void Stop();
		int Port() const noexcept;
	private:
		class Impl;
		std::unique_ptr<Impl> _impl;
	};
	class EventRpcClient final
	{
	public:
		explicit EventRpcClient(RpcOptions options);
		~EventRpcClient();
		// Blocking request/reply with a mandatory deadline. Invoke outside network
		// callbacks and world ticks; no hidden retry of an uncertain publication.
		PublishResult Publish(const Event& event);
	private:
		class Impl;
		std::unique_ptr<Impl> _impl;
	};
}
