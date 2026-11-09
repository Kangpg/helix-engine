#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace helix::services
{
	struct Event
	{
		std::uint32_t version = 1;
		std::string eventId;
		std::string key;
		std::string type;
		std::string payload;
		std::uint32_t producerNodeId = 0;
		std::uint64_t occurredUnixMs = 0;
	};
	enum class PublishStatus { Delivered, Invalid, QueueFull, Unavailable, Deadline, Stopped };
	struct PublishResult
	{
		PublishStatus status = PublishStatus::Unavailable;
		std::string eventId;
		std::int32_t partition = -1;
		std::int64_t offset = -1;
		std::string error;
	};
	enum class EnqueueResult { Accepted, Invalid, QueueFull, Stopped };
	using DeliveryCallback = std::function<void(PublishResult)>;
	class IEventPublisher
	{
	public:
		virtual ~IEventPublisher() = default;
		// Nonblocking admission. Accepted invokes completion exactly once, including
		// on shutdown; rejected calls never invoke it. Completions must be short.
		// A publisher must outlive its accepted completions.
		virtual EnqueueResult TryPublish(const Event& event, DeliveryCallback completion) = 0;
	};
}
