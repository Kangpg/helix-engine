#pragma once
#include <helix/engine_config.h>
#include <helix/services/kafka.h>
#include <helix/services/event_rpc.h>
namespace helix::services::sample
{
	inline KafkaOptions Kafka(const config::EngineConfig& c)
	{
		return {c.kafkaBrokers, c.kafkaTopic, c.kafkaClientId, c.kafkaConsumerGroup,
			c.serviceMaxPending, c.serviceMaxMessageBytes, c.kafkaDeliveryTimeoutMs,
			c.kafkaShutdownTimeoutMs, c.kafkaPollIntervalMs};
	}
	inline RpcOptions Rpc(const config::EngineConfig& c, bool server)
	{
		return {server ? c.serviceRpcListen : c.serviceRpcTarget,
			c.serviceMaxPending, c.serviceMaxMessageBytes, c.serviceDeadlineMs};
	}
}
