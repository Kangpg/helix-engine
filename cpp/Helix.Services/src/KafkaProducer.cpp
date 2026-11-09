#include "KafkaInternal.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace helix::services
{
	class KafkaProducer::Impl
	{
	public:
		struct Pending { std::string id; DeliveryCallback completion; };
		KafkaOptions _options;
		rd_kafka_t* _kafka = nullptr;
		std::mutex _mutex;
		std::mutex _stopMutex;
		std::atomic<bool> _stopped{false};
		std::thread _poller;
		explicit Impl(KafkaOptions o) : _options(std::move(o))
		{
			auto conf = detail::Config(_options);
			detail::Set(conf.get(), "queue.buffering.max.messages", std::to_string(_options.maxPending));
			detail::Set(conf.get(), "delivery.timeout.ms", std::to_string(_options.deliveryTimeoutMs));
			detail::Set(conf.get(), "enable.idempotence", "true");
			rd_kafka_conf_set_dr_msg_cb(conf.get(), [](rd_kafka_t*, const rd_kafka_message_t* m, void*)
			{
				std::unique_ptr<Pending> pending(static_cast<Pending*>(m->_private));
				if (!pending) return;
				PublishResult result{m->err ? PublishStatus::Unavailable : PublishStatus::Delivered,
					pending->id, m->partition, m->offset, m->err ? rd_kafka_message_errstr(m) : ""};
				try { pending->completion(std::move(result)); } catch (...) { /* C ABI boundary */ }
			});
			_kafka = detail::Create(RD_KAFKA_PRODUCER, std::move(conf));
			try
			{
				_poller = std::thread([this] { while (!_stopped.load()) rd_kafka_poll(_kafka, static_cast<int>(_options.pollIntervalMs)); });
			}
			catch (...) { rd_kafka_destroy(_kafka); throw; }
		}
		~Impl() { Stop(); }
		void Stop()
		{
			std::lock_guard stopLock(_stopMutex);
			{
				std::lock_guard lock(_mutex);
				if (_stopped.exchange(true)) return;
			}
			if (_poller.joinable()) _poller.join();
			if (rd_kafka_flush(_kafka, static_cast<int>(_options.shutdownTimeoutMs)) != RD_KAFKA_RESP_ERR_NO_ERROR)
			{
				rd_kafka_purge(_kafka, RD_KAFKA_PURGE_F_QUEUE | RD_KAFKA_PURGE_F_INFLIGHT);
				// Purge completion callbacks are queued locally; serve them before destroy.
				while (rd_kafka_outq_len(_kafka)) rd_kafka_poll(_kafka, 0);
			}
			rd_kafka_destroy(_kafka);
			_kafka = nullptr;
		}
		EnqueueResult Publish(const Event& event, DeliveryCallback completion)
		{
			if (!completion) return EnqueueResult::Invalid;
			auto wire = detail::Encode(event);
			if (!detail::Valid(wire, _options.maxMessageBytes)) return EnqueueResult::Invalid;
			std::string bytes;
			if (!wire.SerializeToString(&bytes)) return EnqueueResult::Invalid;
			auto pending = std::make_unique<Pending>(Pending{event.eventId, std::move(completion)});
			std::lock_guard lock(_mutex);
			if (_stopped.load()) return EnqueueResult::Stopped;
			auto error = rd_kafka_producev(_kafka, RD_KAFKA_V_TOPIC(_options.topic.c_str()),
				RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY), RD_KAFKA_V_VALUE(bytes.data(), bytes.size()),
				RD_KAFKA_V_KEY(event.key.data(), event.key.size()), RD_KAFKA_V_OPAQUE(pending.get()), RD_KAFKA_V_END);
			if (error) return error == RD_KAFKA_RESP_ERR__QUEUE_FULL ? EnqueueResult::QueueFull : EnqueueResult::Invalid;
			pending.release();
			return EnqueueResult::Accepted;
		}
	};
	KafkaProducer::KafkaProducer(KafkaOptions o) : _impl(std::make_unique<Impl>(std::move(o))) {}
	KafkaProducer::~KafkaProducer() = default;
	EnqueueResult KafkaProducer::TryPublish(const Event& e, DeliveryCallback cb) { return _impl->Publish(e, std::move(cb)); }
	void KafkaProducer::Stop() { _impl->Stop(); }
}
