#pragma once
#include <helix/services/kafka.h>
#include "EventCodec.h"
#include <librdkafka/rdkafka.h>
#include <memory>

namespace helix::services::detail
{
	using Conf = std::unique_ptr<rd_kafka_conf_t, decltype(&rd_kafka_conf_destroy)>;
	inline void Set(rd_kafka_conf_t* conf, const char* name, const std::string& value)
	{
		char error[512];
		if (rd_kafka_conf_set(conf, name, value.c_str(), error, sizeof(error)) != RD_KAFKA_CONF_OK)
			throw std::invalid_argument(error);
	}
	inline Conf Config(const KafkaOptions& o)
	{
		Positive(o.maxPending); Positive(o.maxMessageBytes); Positive(o.deliveryTimeoutMs);
		Positive(o.shutdownTimeoutMs); Positive(o.pollIntervalMs);
		if (o.pollIntervalMs > o.deliveryTimeoutMs || o.pollIntervalMs > o.shutdownTimeoutMs)
			throw std::invalid_argument("Kafka poll interval must not exceed delivery/shutdown timeout");
		if (o.brokers.empty() || o.topic.empty() || o.clientId.empty())
			throw std::invalid_argument("Kafka brokers/topic/clientId are required");
		Conf conf(rd_kafka_conf_new(), rd_kafka_conf_destroy);
		Set(conf.get(), "bootstrap.servers", o.brokers);
		Set(conf.get(), "client.id", o.clientId);
		Set(conf.get(), "message.max.bytes", std::to_string(o.maxMessageBytes));
		return conf;
	}
	inline rd_kafka_t* Create(rd_kafka_type_t type, Conf conf)
	{
		char error[512];
		auto* kafka = rd_kafka_new(type, conf.get(), error, sizeof(error));
		if (!kafka) throw std::runtime_error(error);
		conf.release(); // rd_kafka_new takes configuration ownership on success.
		return kafka;
	}
}
