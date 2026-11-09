#include "EventCodec.h"
namespace helix::services::detail
{
	v1::EventEnvelope Encode(const Event& e)
	{
		v1::EventEnvelope w;
		w.set_version(e.version);
		w.set_event_id(e.eventId);
		w.set_key(e.key);
		w.set_type(e.type);
		w.set_payload(e.payload);
		w.set_producer_node_id(e.producerNodeId);
		w.set_occurred_unix_ms(e.occurredUnixMs);
		return w;
	}
	Event Decode(const v1::EventEnvelope& w)
	{
		return {w.version(), w.event_id(), w.key(), w.type(), w.payload(), w.producer_node_id(), w.occurred_unix_ms()};
	}
	bool Valid(const v1::EventEnvelope& w, std::uint32_t maxBytes)
	{
		return w.version() == 1 && !w.event_id().empty() && !w.key().empty() && !w.type().empty() &&
			w.ByteSizeLong() <= maxBytes;
	}
}
