#pragma once
#include <helix/services/event.h>
#include "helix_services.pb.h"
#include <limits>
#include <stdexcept>

namespace helix::services::detail
{
	v1::EventEnvelope Encode(const Event& event);
	Event Decode(const v1::EventEnvelope& wire);
	bool Valid(const v1::EventEnvelope& wire, std::uint32_t maxBytes);
	inline int Positive(std::uint32_t value)
	{
		if (!value || value > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
			throw std::invalid_argument("Service limits must be 1..INT_MAX");
		return static_cast<int>(value);
	}
}
