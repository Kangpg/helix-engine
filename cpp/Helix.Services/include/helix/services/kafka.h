#pragma once
#include <helix/services/event.h>
#include <memory>

namespace helix::services
{
	struct KafkaOptions
	{
		std::string brokers;
		std::string topic;
		std::string clientId;
		std::string consumerGroup;
		std::uint32_t maxPending = 0;
		std::uint32_t maxMessageBytes = 0;
		std::uint32_t deliveryTimeoutMs = 0;
		std::uint32_t shutdownTimeoutMs = 0;
		std::uint32_t pollIntervalMs = 0;
	};
	// Kafka security/authentication is deliberately not configured by this local
	// adapter. Use only a trusted private PLAINTEXT broker. No gameplay state is
	// mutated by these adapters; applications enqueue commands to their owner.
	class KafkaProducer final : public IEventPublisher
	{
	public:
		explicit KafkaProducer(KafkaOptions options);
		~KafkaProducer();
		EnqueueResult TryPublish(const Event&, DeliveryCallback) override;
		// Call from the owning thread, never from a delivery callback. Joins poller,
		// flushes, then reports failures for any purged records. Idempotent.
		void Stop();
	private:
		class Impl;
		std::unique_ptr<Impl> _impl;
	};
	enum class ConsumeResult { Idle, Committed, Retry, Error };
	class KafkaConsumer final
	{
	public:
		explicit KafkaConsumer(KafkaOptions options);
		~KafkaConsumer();
		// Single owner thread, non-reentrant. Handler success permits offset commit.
		// False/exception/decode failure seeks back before another message can commit.
		// Applications must deduplicate eventId: side effects and offset commit are
		// not atomic. A failed seek poisons this consumer until it is recreated.
		ConsumeResult Poll(std::uint32_t timeoutMs, const std::function<bool(const Event&)>& handler);
		const std::string& GetError() const noexcept;
	private:
		class Impl;
		std::unique_ptr<Impl> _impl;
	};
}
