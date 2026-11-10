#include "helix/protocol/flatbuffers_serializer.h"
#include "helix_mmo_generated.h"

namespace helix::protocol
{
	namespace
	{
		namespace F = Helix::Protocol::FlatWire;
		flatbuffers::Offset<F::Position> Write(flatbuffers::FlatBufferBuilder& b, const std::optional<messages::Position>& p)
			{ return p ? F::CreatePosition(b, p->x, p->y, p->z) : flatbuffers::Offset<F::Position>{}; }
		std::optional<messages::Position> Read(const F::Position* p) { return p ? std::optional(messages::Position{ p->x(), p->y(), p->z() }) : std::nullopt; }
		const F::Packet* Verify(std::span<const std::byte> bytes)
		{
			if (bytes.size() < 8 || bytes.size() >= FLATBUFFERS_MAX_BUFFER_SIZE) { return nullptr; }
			flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
			if (!F::VerifyPacketBuffer(verifier)) { return nullptr; }
			return F::GetPacket(bytes.data());
		}
		// 스레드별 빌더 재사용: 호출마다 내부 버퍼를 힙 할당하지 않는다(Clear는 용량을 유지).
		// Encode는 빌더 사용 중 사용자 코드를 호출하지 않으므로 같은 스레드 재진입이 없다.
		// 버퍼는 스레드가 인코딩한 가장 큰 패킷 크기로 스레드 종료까지 유지된다.
		flatbuffers::FlatBufferBuilder& Builder()
		{
			thread_local flatbuffers::FlatBufferBuilder builder;
			builder.Clear();
			return builder;
		}
		void Finish(flatbuffers::FlatBufferBuilder& b, std::uint32_t version, std::uint64_t id, F::Body type, flatbuffers::Offset<void> body, std::string& bytes)
		{
			F::FinishPacketBuffer(b, F::CreatePacket(b, version, id, type, body));
			bytes.assign(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
		}
	}
	bool FlatBuffersSerializer::EncodeClient(const messages::ClientPacket& p, std::string& bytes) const
	{
		bytes.clear(); auto& b = Builder();
		F::Body type; flatbuffers::Offset<void> body;
		if (const auto* m = std::get_if<messages::Move>(&p.body)) { type = F::Body_Move; body = F::CreateMove(b, Write(b, m->destination)).Union(); }
		else if (const auto* a = std::get_if<messages::Attack>(&p.body)) { type = F::Body_Attack; body = F::CreateAttack(b, a->targetEntityId).Union(); }
		else if (std::holds_alternative<messages::Look>(p.body)) { type = F::Body_Look; body = F::CreateLook(b).Union(); }
		else { return false; }
		Finish(b, p.protocolVersion, p.requestId, type, body, bytes); return true;
	}
	bool FlatBuffersSerializer::DecodeClient(std::span<const std::byte> bytes, messages::ClientPacket& p) const
	{
		p = {}; const auto* w = Verify(bytes); if (!w || !w->body()) { return false; }
		messages::ClientPacket value{ w->protocol_version(), w->request_id(), {} };
		switch (w->body_type())
		{
		case F::Body_Move: value.body = messages::Move{ Read(w->body_as_Move()->destination()) }; break;
		case F::Body_Attack: value.body = messages::Attack{ w->body_as_Attack()->target_entity_id() }; break;
		case F::Body_Look: value.body = messages::Look{}; break;
		default: return false;
		}
		p = std::move(value); return true;
	}
	bool FlatBuffersSerializer::EncodeServer(const messages::ServerPacket& p, std::string& bytes) const
	{
		bytes.clear(); auto& b = Builder();
		F::Body type; flatbuffers::Offset<void> body;
		if (const auto* v = std::get_if<messages::Welcome>(&p.body)) { type = F::Body_Welcome; body = F::CreateWelcome(b, v->entityId, v->worldId).Union(); }
		else if (const auto* v = std::get_if<messages::Reply>(&p.body)) { type = F::Body_Reply; body = F::CreateReply(b, static_cast<F::ReplyStatus>(v->status)).Union(); }
		else if (const auto* v = std::get_if<messages::State>(&p.body))
		{
			type = F::Body_State; const auto pos = Write(b, v->position); const auto ids = b.CreateVector(v->visibleEntityIds);
			body = F::CreateState(b, v->entityId, pos, v->health, ids, v->tick).Union();
		}
		else { return false; }
		Finish(b, p.protocolVersion, p.requestId, type, body, bytes); return true;
	}
	bool FlatBuffersSerializer::DecodeServer(std::span<const std::byte> bytes, messages::ServerPacket& p) const
	{
		p = {}; const auto* w = Verify(bytes); if (!w || !w->body()) { return false; }
		messages::ServerPacket value{ w->protocol_version(), w->request_id(), {} };
		switch (w->body_type())
		{
		case F::Body_Welcome: value.body = messages::Welcome{ w->body_as_Welcome()->entity_id(), w->body_as_Welcome()->world_id() }; break;
		case F::Body_Reply:
			if (w->body_as_Reply()->status() > F::ReplyStatus_Pending) { return false; }
			value.body = messages::Reply{ static_cast<messages::ReplyStatus>(w->body_as_Reply()->status()) }; break;
		case F::Body_State:
		{
			const auto* s = w->body_as_State(); messages::State state{ s->entity_id(), Read(s->position()), s->health(), {}, s->tick() };
			if (const auto* ids = s->visible_entity_ids()) { state.visibleEntityIds.assign(ids->begin(), ids->end()); }
			value.body = std::move(state); break;
		}
		default: return false;
		}
		p = std::move(value); return true;
	}
}
