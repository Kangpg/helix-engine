#pragma once

// =============================================================================
// Phase 3 — 엔진 설정 (파일 + 환경변수 오버라이드 + 검증)
//
// 레이어링(12-factor): 코드 기본값 → 설정 파일 → 환경변수 오버라이드.
//   환경변수 규칙: HELIX_<SECTION>__<KEY> (대문자). 예: HELIX_NETWORK__PORT=6000
//
// 파서는 TOML/INI 공통 부분집합(`[section]` + `key = value`, `#`/`;` 주석)을
// 지원하는 경량 구현이다. 추후 toml++ 등으로 백엔드만 교체 가능(스키마 동일).
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace helix::config
{
	struct EngineConfig
	{
		// [network]
		std::string ip = "0.0.0.0";
		std::uint16_t port = 5000;
		std::string transport = "tcp";       // tcp | kcp | quic  (Phase 7)
		std::string backend = "auto";        // auto | iocp | iocp_rio | epoll  (TCP 전용)
		std::uint32_t maxConnections = 10000;
		std::uint32_t backlog = 128;
		std::uint32_t ioThreads = 0;          // 0 = hardware_concurrency
		std::uint32_t concurrentAccepts = 16;
		// [rio] (native Windows iocp_rio only)
		std::uint32_t rioBufferSize = 16384;
		std::uint32_t rioCompletionQueueSize = 32768;

		// [buffers] / [framing]
		std::uint32_t recvBufSize = 8192;
		std::uint32_t maxSendQueue = 1024;
		std::uint32_t frameHeaderSize = 4;    // 2 | 4
		bool frameBigEndian = true;
		std::uint32_t maxMessageSize = 1u << 20;

		// [logging]
		std::string logLevel = "info";        // debug | info | warn | error
		std::string logSink = "stdout";       // stdout | file | both
		std::string logFile = "helix.log";

		// [shutdown]
		std::uint32_t drainTimeoutMs = 5000;  // graceful drain 대기 한계

		// [cluster] — scale-out (Phase 6). 기본 비활성(단일 노드).
		bool clusterEnabled = false;
		std::string clusterBackend = "redis";                  // redis (현재 유일)
		std::string clusterUrl = "redis://127.0.0.1:6379/0";
		std::uint32_t nodeId = 0;                              // 이 노드 ID(노드별 고유)
		std::uint32_t sessionTtlSec = 30;                     // 디렉터리 엔트리 TTL
		std::uint32_t heartbeatMs = 10000;                    // TTL 갱신 주기(< ttl)

		// [mmo] — 실행 한도. 기능 규칙은 앱에서 인터페이스로 주입한다.
		std::uint32_t worldMaxEntities = 10000;
		std::uint32_t worldMaxPendingCommands = 4096;
		std::uint32_t worldMaxCommandsPerTick = 1024;
		std::uint32_t worldTickMs = 50;
		// C++ experimental staged world; C# has an independent runtime.
		std::string worldExecution = "sequential"; // sequential | parallel
		std::uint32_t worldWorkerThreads = 4;
		std::uint32_t worldBatchSize = 128;
		double worldCellSize = 16.0;
		double worldVisibilityRadius = 50.0;
		std::uint32_t worldMaxHitsPerTick = 65536;
		// [services] / [kafka] — optional C++ adapters, trusted private endpoints.
		std::string serviceRpcListen = "127.0.0.1:7000";
		std::string serviceRpcTarget = "127.0.0.1:7000";
		std::uint32_t serviceDeadlineMs = 10000;
		std::uint32_t serviceMaxPending = 1024;
		std::uint32_t serviceMaxMessageBytes = 65536;
		std::string kafkaBrokers = "127.0.0.1:9092";
		std::string kafkaTopic = "helix.events";
		std::string kafkaClientId = "helix";
		std::string kafkaConsumerGroup = "helix.sample";
		std::uint32_t kafkaDeliveryTimeoutMs = 5000;
		std::uint32_t kafkaShutdownTimeoutMs = 5000;
		std::uint32_t kafkaPollIntervalMs = 10;
		// [protocol] — selection is consumed by the application composition root.
		std::string protocolSerializer = "protobuf";
	};

	class ConfigLoader
	{
	public:
		// 파일 로드 → 환경변수 오버라이드 → 검증. 파일이 없으면 기본값으로 성공.
		// 검증 실패 시 nullopt 반환(GetError로 사유 확인).
		[[nodiscard]] std::optional<EngineConfig> Load(const std::string& path);

		[[nodiscard]] const std::string& GetError() const noexcept { return _error; }

	private:
		// env(HELIX_SECTION__KEY) → 파일값 → 기본값 순으로 조회.
		[[nodiscard]] std::string Value(const std::string& section,
		                                const std::string& key,
		                                const std::string& def) const;

		std::unordered_map<std::string, std::string> _kv;  // "section.key" → value
		std::string _error;
	};
}
