#include "EngineConfig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace helix::config
{
	namespace
	{
		std::string Trim(const std::string& s)
		{
			std::size_t b = 0, e = s.size();
			while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) { ++b; }
			while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) { --e; }
			return s.substr(b, e - b);
		}

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(),
			               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		std::string Upper(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(),
			               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			return s;
		}

		// '=' 뒤의 원시 값에서 따옴표를 풀고 인라인 주석(#, ;)을 제거한다.
		// 따옴표 안의 #/; 는 보존한다. 예: `"auto"   # comment` → `auto`
		std::string ParseValueToken(std::string v)
		{
			v = Trim(v);
			if (!v.empty() && (v.front() == '"' || v.front() == '\''))
			{
				const char q = v.front();
				const std::size_t end = v.find(q, 1);
				if (end != std::string::npos)
				{
					return v.substr(1, end - 1);  // 따옴표 안만; 뒤(주석 포함) 버림
				}
				return v.substr(1);               // 닫는 따옴표 없음 — 최선으로
			}
			const std::size_t h = v.find_first_of("#;");
			if (h != std::string::npos)
			{
				v = v.substr(0, h);
			}
			return Trim(v);
		}

		std::uint32_t ParseUInt(const std::string& s, std::uint32_t def)
		{
			try { return static_cast<std::uint32_t>(std::stoul(s)); }
			catch (...) { return def; }
		}

		bool ParseBool(const std::string& s, bool def)
		{
			const std::string l = Lower(Trim(s));
			if (l == "true" || l == "1" || l == "yes" || l == "on") { return true; }
			if (l == "false" || l == "0" || l == "no" || l == "off") { return false; }
			return def;
		}
	}

	std::string ConfigLoader::Value(const std::string& section, const std::string& key,
	                                const std::string& def) const
	{
		// 1) 환경변수 오버라이드
		const std::string envName = "HELIX_" + Upper(section) + "__" + Upper(key);
		if (const char* e = std::getenv(envName.c_str()))
		{
			return std::string(e);
		}
		// 2) 파일값
		const auto it = _kv.find(Lower(section) + "." + Lower(key));
		if (it != _kv.end())
		{
			return it->second;
		}
		// 3) 기본값
		return def;
	}

	std::optional<EngineConfig> ConfigLoader::Load(const std::string& path)
	{
		_kv.clear();
		_error.clear();

		// 파일 파싱(없으면 기본값으로 진행 — 에러 아님).
		std::ifstream in(path);
		if (in.is_open())
		{
			std::string line;
			std::string section;
			while (std::getline(in, line))
			{
				std::string t = Trim(line);
				if (t.empty() || t[0] == '#' || t[0] == ';') { continue; }
				if (t.front() == '[' && t.back() == ']')
				{
					section = Lower(Trim(t.substr(1, t.size() - 2)));
					continue;
				}
				const std::size_t eq = t.find('=');
				if (eq == std::string::npos) { continue; }
				const std::string key = Lower(Trim(t.substr(0, eq)));
				_kv[section + "." + key] = ParseValueToken(t.substr(eq + 1));
			}
		}

		EngineConfig c;
		c.ip               = Value("network", "ip", c.ip);
		c.port             = static_cast<std::uint16_t>(ParseUInt(Value("network", "port", std::to_string(c.port)), c.port));
		c.transport        = Lower(Value("network", "transport", c.transport));
		c.backend          = Lower(Value("network", "backend", c.backend));
		c.maxConnections   = ParseUInt(Value("network", "max_connections", std::to_string(c.maxConnections)), c.maxConnections);
		c.backlog          = ParseUInt(Value("network", "backlog", std::to_string(c.backlog)), c.backlog);
		c.ioThreads        = ParseUInt(Value("network", "io_threads", std::to_string(c.ioThreads)), c.ioThreads);
		c.concurrentAccepts= ParseUInt(Value("network", "concurrent_accepts", std::to_string(c.concurrentAccepts)), c.concurrentAccepts);

		c.recvBufSize      = ParseUInt(Value("buffers", "recv_buffer_size", std::to_string(c.recvBufSize)), c.recvBufSize);
		c.maxSendQueue     = ParseUInt(Value("buffers", "max_send_queue", std::to_string(c.maxSendQueue)), c.maxSendQueue);

		c.frameHeaderSize  = ParseUInt(Value("framing", "header_size", std::to_string(c.frameHeaderSize)), c.frameHeaderSize);
		c.frameBigEndian   = ParseBool(Value("framing", "big_endian", c.frameBigEndian ? "true" : "false"), c.frameBigEndian);
		c.maxMessageSize   = ParseUInt(Value("framing", "max_message_size", std::to_string(c.maxMessageSize)), c.maxMessageSize);

		c.logLevel         = Lower(Value("logging", "level", c.logLevel));
		c.logSink          = Lower(Value("logging", "sink", c.logSink));
		c.logFile          = Value("logging", "file", c.logFile);

		c.drainTimeoutMs   = ParseUInt(Value("shutdown", "drain_timeout_ms", std::to_string(c.drainTimeoutMs)), c.drainTimeoutMs);

		c.clusterEnabled   = ParseBool(Value("cluster", "enabled", c.clusterEnabled ? "true" : "false"), c.clusterEnabled);
		c.clusterBackend   = Lower(Value("cluster", "backend", c.clusterBackend));
		c.clusterUrl       = Value("cluster", "url", c.clusterUrl);
		c.nodeId           = ParseUInt(Value("cluster", "node_id", std::to_string(c.nodeId)), c.nodeId);
		c.sessionTtlSec    = ParseUInt(Value("cluster", "session_ttl_sec", std::to_string(c.sessionTtlSec)), c.sessionTtlSec);
		c.heartbeatMs      = ParseUInt(Value("cluster", "heartbeat_ms", std::to_string(c.heartbeatMs)), c.heartbeatMs);
		const auto mmoLimit = [this](const char* key, std::uint32_t fallback)
		{
			const auto text = Value("mmo", key, std::to_string(fallback));
			std::uint32_t value = 0;
			const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
			return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : 0u;
		};
		c.worldMaxEntities = mmoLimit("max_entities", c.worldMaxEntities);
		c.worldMaxPendingCommands = mmoLimit("max_pending_commands", c.worldMaxPendingCommands);
		c.worldMaxCommandsPerTick = mmoLimit("max_commands_per_tick", c.worldMaxCommandsPerTick);
		c.worldTickMs = mmoLimit("tick_ms", c.worldTickMs);
		c.worldExecution = Lower(Value("mmo", "execution", c.worldExecution));
		c.worldWorkerThreads = mmoLimit("worker_threads", c.worldWorkerThreads);
		c.worldBatchSize = mmoLimit("batch_size", c.worldBatchSize);
		c.worldMaxHitsPerTick = mmoLimit("max_hits_per_tick", c.worldMaxHitsPerTick);
		const auto mmoDouble = [this](const char* key, double fallback)
		{
			const auto text = Value("mmo", key, std::to_string(fallback));
			double value = 0;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value) ? value : -1.0;
		};
		c.worldCellSize = mmoDouble("cell_size", c.worldCellSize);
		c.worldVisibilityRadius = mmoDouble("visibility_radius", c.worldVisibilityRadius);
		c.protocolSerializer = Lower(Value("protocol", "serializer", c.protocolSerializer));
		const auto rioLimit = [this](const char* key, std::uint32_t fallback)
		{
			const auto text = Value("rio", key, std::to_string(fallback));
			std::uint32_t value = 0;
			const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
			return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : 0u;
		};
		c.rioBufferSize = rioLimit("buffer_size", c.rioBufferSize);
		c.rioCompletionQueueSize = rioLimit("completion_queue_size", c.rioCompletionQueueSize);

		const auto serviceLimit = [this](const char* section, const char* key, std::uint32_t fallback)
		{
			const auto text = Value(section, key, std::to_string(fallback));
			std::uint32_t value = 0;
			const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
			return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : 0u;
		};
		c.serviceRpcListen = Value("services", "rpc_listen", c.serviceRpcListen);
		c.serviceRpcTarget = Value("services", "rpc_target", c.serviceRpcTarget);
		c.serviceDeadlineMs = serviceLimit("services", "deadline_ms", c.serviceDeadlineMs);
		c.serviceMaxPending = serviceLimit("services", "max_pending", c.serviceMaxPending);
		c.serviceMaxMessageBytes = serviceLimit("services", "max_message_bytes", c.serviceMaxMessageBytes);
		c.kafkaBrokers = Value("kafka", "brokers", c.kafkaBrokers);
		c.kafkaTopic = Value("kafka", "topic", c.kafkaTopic);
		c.kafkaClientId = Value("kafka", "client_id", c.kafkaClientId);
		c.kafkaConsumerGroup = Value("kafka", "consumer_group", c.kafkaConsumerGroup);
		c.kafkaDeliveryTimeoutMs = serviceLimit("kafka", "delivery_timeout_ms", c.kafkaDeliveryTimeoutMs);
		c.kafkaShutdownTimeoutMs = serviceLimit("kafka", "shutdown_timeout_ms", c.kafkaShutdownTimeoutMs);
		c.kafkaPollIntervalMs = serviceLimit("kafka", "poll_interval_ms", c.kafkaPollIntervalMs);

		// --- 검증 ---
		std::ostringstream err;
		for (const auto limit : {c.serviceDeadlineMs, c.serviceMaxPending, c.serviceMaxMessageBytes,
			c.kafkaDeliveryTimeoutMs, c.kafkaShutdownTimeoutMs, c.kafkaPollIntervalMs})
			if (!limit || limit > 2147483647u) { err << "services/kafka limits must be 1..INT_MAX; "; break; }
		if (c.serviceMaxMessageBytes < 1000 || c.kafkaPollIntervalMs > c.kafkaDeliveryTimeoutMs || c.kafkaPollIntervalMs > c.kafkaShutdownTimeoutMs)
			err << "services.max_message_bytes must be >= 1000; kafka.poll_interval_ms must be <= delivery_timeout_ms and shutdown_timeout_ms; ";
		if (c.serviceRpcListen.empty() || c.serviceRpcTarget.empty() || c.kafkaBrokers.empty() ||
			c.kafkaTopic.empty() || c.kafkaClientId.empty() || c.kafkaConsumerGroup.empty())
			err << "services/kafka endpoints and identifiers are required; ";
		if (c.protocolSerializer.empty()) { err << "protocol.serializer is required; "; }
		if (c.worldExecution != "sequential" && c.worldExecution != "parallel") { err << "mmo.execution must be sequential|parallel; "; }
		if (c.worldWorkerThreads == 0 || c.worldBatchSize == 0 || c.worldMaxHitsPerTick == 0 ||
			c.worldCellSize <= 0 || c.worldVisibilityRadius < 0)
			{ err << "mmo parallel worker/batch/hit/cell limits must be positive; visibility_radius must be >= 0; "; }
		if (c.rioBufferSize == 0 || c.rioCompletionQueueSize == 0 ||
			(c.backend == "iocp_rio" && static_cast<std::uint64_t>(c.rioCompletionQueueSize) < 2ull * c.maxConnections))
			{ err << "rio limits must be > 0 and completion_queue_size >= 2 * max_connections for iocp_rio; "; }
		if (c.worldMaxEntities == 0 || c.worldMaxPendingCommands == 0 || c.worldMaxCommandsPerTick == 0 || c.worldTickMs == 0)
		{
			err << "mmo limits and tick_ms must be > 0; ";
		}
		if (c.port == 0) { err << "network.port must be 1..65535; "; }
		if (c.transport != "tcp" && c.transport != "kcp" && c.transport != "quic")
		{
			err << "network.transport must be tcp|kcp|quic (got '" << c.transport << "'); ";
		}
		if (c.backend != "auto" && c.backend != "iocp" && c.backend != "iocp_rio" && c.backend != "epoll")
		{
			err << "network.backend must be auto|iocp|iocp_rio|epoll (got '" << c.backend << "'); ";
		}
		if (c.frameHeaderSize != 2 && c.frameHeaderSize != 4)
		{
			err << "framing.header_size must be 2 or 4; ";
		}
		if (c.maxMessageSize == 0) { err << "framing.max_message_size must be > 0; "; }
		if (c.logLevel != "debug" && c.logLevel != "info" && c.logLevel != "warn" && c.logLevel != "error")
		{
			err << "logging.level must be debug|info|warn|error; ";
		}
		if (c.logSink != "stdout" && c.logSink != "file" && c.logSink != "both")
		{
			err << "logging.sink must be stdout|file|both; ";
		}
		if (c.clusterEnabled)
		{
			if (c.clusterBackend != "redis")
			{
				err << "cluster.backend must be 'redis' (got '" << c.clusterBackend << "'); ";
			}
			if (c.nodeId >= (1u << 24))
			{
				err << "cluster.node_id must be < 16777216 (24-bit, embedded in SessionId); ";
			}
			if (c.sessionTtlSec == 0) { err << "cluster.session_ttl_sec must be > 0; "; }
			if (c.heartbeatMs == 0 || c.heartbeatMs >= c.sessionTtlSec * 1000)
			{
				err << "cluster.heartbeat_ms must be > 0 and < session_ttl_sec*1000; ";
			}
		}

		if (!err.str().empty())
		{
			_error = err.str();
			return std::nullopt;
		}
		return c;
	}
}
