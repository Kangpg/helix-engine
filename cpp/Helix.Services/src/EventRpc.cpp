#include <helix/services/event_rpc.h>
#include "EventCodec.h"
#include "helix_services.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <chrono>

namespace helix::services
{
	namespace
	{
		void Validate(const RpcOptions& o)
		{
			if (o.address.empty()) throw std::invalid_argument("RPC address is required");
			detail::Positive(o.maxPending); detail::Positive(o.maxMessageBytes); detail::Positive(o.deadlineMs);
		}
		grpc::Status Status(PublishStatus status, const std::string& error)
		{
			switch (status)
			{
			case PublishStatus::Delivered: return grpc::Status::OK;
			case PublishStatus::Invalid: return {grpc::StatusCode::INVALID_ARGUMENT, error};
			case PublishStatus::QueueFull: return {grpc::StatusCode::RESOURCE_EXHAUSTED, error};
			case PublishStatus::Deadline: return {grpc::StatusCode::DEADLINE_EXCEEDED, error};
			default: return {grpc::StatusCode::UNAVAILABLE, error};
			}
		}
	}
	class EventRpcServer::Impl final : public v1::EventIngress::CallbackService
	{
	public:
		RpcOptions _options;
		IEventPublisher& _publisher;
		std::atomic<std::uint32_t> _pending{0};
		std::unique_ptr<grpc::Server> _server;
		int _port = 0;
		Impl(RpcOptions o, IEventPublisher& p) : _options(std::move(o)), _publisher(p) { Validate(_options); }
		grpc::ServerUnaryReactor* Publish(grpc::CallbackServerContext* context,
			const v1::EventEnvelope* request, v1::PublishReply* reply) override
		{
			auto* reactor = context->DefaultReactor();
			if (!detail::Valid(*request, _options.maxMessageBytes))
			{ reactor->Finish({grpc::StatusCode::INVALID_ARGUMENT, "Invalid event envelope"}); return reactor; }
			auto count = _pending.load();
			while (count < _options.maxPending && !_pending.compare_exchange_weak(count, count + 1)) {}
			if (count >= _options.maxPending)
			{ reactor->Finish({grpc::StatusCode::RESOURCE_EXHAUSTED, "RPC admission queue is full"}); return reactor; }
			try
			{
				auto result = _publisher.TryPublish(detail::Decode(*request), [this, reply, reactor](PublishResult result)
				{
					if (result.status == PublishStatus::Delivered)
					{
						reply->set_event_id(result.eventId);
						reply->set_partition(result.partition);
						reply->set_offset(result.offset);
					}
					_pending.fetch_sub(1);
					reactor->Finish(Status(result.status, result.error));
				});
				if (result != EnqueueResult::Accepted)
				{
					_pending.fetch_sub(1);
					auto status = result == EnqueueResult::QueueFull ? PublishStatus::QueueFull :
						result == EnqueueResult::Invalid ? PublishStatus::Invalid : PublishStatus::Stopped;
					reactor->Finish(Status(status, "Publisher rejected admission"));
				}
			}
			catch (...)
			{
				_pending.fetch_sub(1);
				reactor->Finish({grpc::StatusCode::INTERNAL, "Publisher failed"});
			}
			return reactor;
		}
		void Stop()
		{
			if (_server)
			{
				_server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(_options.deadlineMs));
				_server->Wait();
				_server.reset();
			}
		}
		~Impl() { Stop(); }
	};
	EventRpcServer::EventRpcServer(RpcOptions o, IEventPublisher& p) : _impl(std::make_unique<Impl>(std::move(o), p)) {}
	EventRpcServer::~EventRpcServer() = default;
	void EventRpcServer::Start()
	{
		if (_impl->_server) throw std::logic_error("RPC server already started");
		grpc::ServerBuilder builder;
		builder.SetMaxReceiveMessageSize(static_cast<int>(_impl->_options.maxMessageBytes));
		builder.AddListeningPort(_impl->_options.address, grpc::InsecureServerCredentials(), &_impl->_port);
		builder.RegisterService(_impl.get());
		_impl->_server = builder.BuildAndStart();
		if (!_impl->_server || !_impl->_port) throw std::runtime_error("Cannot bind RPC server");
	}
	void EventRpcServer::Stop() { _impl->Stop(); }
	int EventRpcServer::Port() const noexcept { return _impl->_port; }
	class EventRpcClient::Impl
	{
	public:
		RpcOptions _options;
		std::unique_ptr<v1::EventIngress::Stub> _stub;
		explicit Impl(RpcOptions o) : _options(std::move(o))
		{
			Validate(_options);
			grpc::ChannelArguments args;
			args.SetMaxSendMessageSize(static_cast<int>(_options.maxMessageBytes));
			_stub = v1::EventIngress::NewStub(grpc::CreateCustomChannel(_options.address, grpc::InsecureChannelCredentials(), args));
		}
	};
	EventRpcClient::EventRpcClient(RpcOptions o) : _impl(std::make_unique<Impl>(std::move(o))) {}
	EventRpcClient::~EventRpcClient() = default;
	PublishResult EventRpcClient::Publish(const Event& e)
	{
		auto request = detail::Encode(e);
		if (!detail::Valid(request, _impl->_options.maxMessageBytes)) return {PublishStatus::Invalid, e.eventId};
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(_impl->_options.deadlineMs));
		v1::PublishReply reply;
		auto result = _impl->_stub->Publish(&context, request, &reply);
		if (result.ok()) return {PublishStatus::Delivered, reply.event_id(), reply.partition(), reply.offset(), {}};
		auto status = result.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ? PublishStatus::Deadline :
			result.error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED ? PublishStatus::QueueFull :
			result.error_code() == grpc::StatusCode::INVALID_ARGUMENT ? PublishStatus::Invalid : PublishStatus::Unavailable;
		return {status, e.eventId, -1, -1, result.error_message()};
	}
}
