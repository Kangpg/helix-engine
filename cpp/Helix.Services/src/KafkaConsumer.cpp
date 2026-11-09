#include "KafkaInternal.h"
#include <exception>

namespace helix::services
{
	class KafkaConsumer::Impl
	{
	public:
		KafkaOptions _options;
		rd_kafka_t* _kafka = nullptr;
		std::string _error;
		bool _poisoned = false;
		bool _polling = false;
		explicit Impl(KafkaOptions o) : _options(std::move(o))
		{
			auto conf = detail::Config(_options);
			if (_options.consumerGroup.empty()) throw std::invalid_argument("Kafka consumerGroup is required");
			detail::Set(conf.get(), "group.id", _options.consumerGroup);
			detail::Set(conf.get(), "enable.auto.commit", "false");
			detail::Set(conf.get(), "enable.auto.offset.store", "false");
			detail::Set(conf.get(), "auto.offset.reset", "earliest");
			_kafka = detail::Create(RD_KAFKA_CONSUMER, std::move(conf));
			rd_kafka_poll_set_consumer(_kafka);
			auto* topics = rd_kafka_topic_partition_list_new(1);
			rd_kafka_topic_partition_list_add(topics, _options.topic.c_str(), RD_KAFKA_PARTITION_UA);
			auto result = rd_kafka_subscribe(_kafka, topics);
			rd_kafka_topic_partition_list_destroy(topics);
			if (result)
			{
				rd_kafka_destroy(_kafka);
				throw std::runtime_error(rd_kafka_err2str(result));
			}
		}
		~Impl() { rd_kafka_consumer_close(_kafka); rd_kafka_destroy(_kafka); }
		ConsumeResult Retry(const rd_kafka_message_t* message)
		{
			auto* offsets = rd_kafka_topic_partition_list_new(1);
			auto* offset = rd_kafka_topic_partition_list_add(offsets, rd_kafka_topic_name(message->rkt), message->partition);
			offset->offset = message->offset;
			auto* failure = rd_kafka_seek_partitions(_kafka, offsets, static_cast<int>(_options.deliveryTimeoutMs));
			_poisoned = failure != nullptr || offset->err != RD_KAFKA_RESP_ERR_NO_ERROR;
			if (failure) { _error += "; seek failed: "; _error += rd_kafka_error_string(failure); rd_kafka_error_destroy(failure); }
			if (offset->err) { _error += "; seek failed: "; _error += rd_kafka_err2str(offset->err); }
			rd_kafka_topic_partition_list_destroy(offsets);
			return _poisoned ? ConsumeResult::Error : ConsumeResult::Retry;
		}
		ConsumeResult Poll(std::uint32_t timeout, const std::function<bool(const Event&)>& handler)
		{
			if (_polling) throw std::logic_error("KafkaConsumer::Poll is not reentrant");
			if (_poisoned) return ConsumeResult::Error;
			_polling = true;
			struct Reset { bool& value; ~Reset() { value = false; } } reset{_polling};
			detail::Positive(timeout);
			using Message = std::unique_ptr<rd_kafka_message_t, decltype(&rd_kafka_message_destroy)>;
			Message message(rd_kafka_consumer_poll(_kafka, static_cast<int>(timeout)), rd_kafka_message_destroy);
			if (!message) return ConsumeResult::Idle;
			_error.clear();
			if (message->err) { _error = rd_kafka_message_errstr(message.get()); return ConsumeResult::Error; }
			v1::EventEnvelope wire;
			bool handled = false;
			if (message->len <= _options.maxMessageBytes && wire.ParseFromArray(message->payload, static_cast<int>(message->len)) &&
				detail::Valid(wire, _options.maxMessageBytes))
			{
				try { handled = handler && handler(detail::Decode(wire)); }
				catch (const std::exception& e) { _error = e.what(); }
				catch (...) { _error = "Handler threw"; }
			}
			if (!handled)
			{
				if (_error.empty()) _error = "Event decode or handler rejected the record";
				return Retry(message.get());
			}
			// commit_message commits this record's offset + 1, only after handler success.
			auto result = rd_kafka_commit_message(_kafka, message.get(), 0);
			if (result) { _error = rd_kafka_err2str(result); return Retry(message.get()); }
			return ConsumeResult::Committed;
		}
	};
	KafkaConsumer::KafkaConsumer(KafkaOptions o) : _impl(std::make_unique<Impl>(std::move(o))) {}
	KafkaConsumer::~KafkaConsumer() = default;
	ConsumeResult KafkaConsumer::Poll(std::uint32_t ms, const std::function<bool(const Event&)>& cb) { return _impl->Poll(ms, cb); }
	const std::string& KafkaConsumer::GetError() const noexcept { return _impl->_error; }
}
