// Which machine of the G2 family the board presents.
//
// detect_model() at 0x30050864 decides and stores its answer to 0x30119848. It
// tests MBAR+$1D0 (UIPCR) bit 0 FIRST: set selects model 3, the Engine, and the
// panel strap is never read. Otherwise panel_id() at 0x3005BFFE returns bits
// 5:4 of the CS5 latch, and 0b00 gives model 0, a plain G2; 0b11 gives model 1,
// the G2X; 0b10 gives model 2, the Rack. 0b01 stores nothing and the OS stops
// on OS-HARDWARE ERR at 0x3001B86C.
//
// So the two straps are not independent, and this header is the one place that
// says how they combine. The Sim answers UIPCR and the Latches answer CS5; each
// takes a Model and derives its own bits from it.

#pragma once

#include <cstdint>
#include <string>

namespace g2
{
	enum class Model : uint8_t
	{
		G2,
		G2X,
		Rack,
		Engine
	};

	// Bits 5:4 of the CS5 latch, unshifted. The Engine's code is the plain G2's
	// because the UIPCR bit decides before panel_id() is called, so the strap it
	// would have returned reaches no decision.
	constexpr uint8_t panelStrapCode(const Model _model)
	{
		switch(_model)
		{
		case Model::G2X:  return 0x3u;
		case Model::Rack: return 0x2u;
		case Model::G2:
		case Model::Engine:
			break;
		}
		return 0x0u;
	}

	constexpr bool isEngineStrapSet(const Model _model)
	{
		return _model == Model::Engine;
	}

	constexpr const char* modelName(const Model _model)
	{
		switch(_model)
		{
		case Model::G2:     return "g2";
		case Model::G2X:    return "g2x";
		case Model::Rack:   return "rack";
		case Model::Engine: return "engine";
		}
		return "g2x";
	}

	// Every name a caller may give, in one string, so a rejection can print the
	// whole set without a second list that could disagree with this one.
	constexpr const char* g_modelNames = "g2, g2x, rack, engine";

	// False leaves _model untouched. A caller that ignores the result gets the
	// silent default that this whole mechanism exists to prevent, so treat the
	// return value as the point of the call.
	inline bool modelFromName(const std::string& _name, Model& _model)
	{
		std::string folded;
		folded.reserve(_name.size());
		for(const char c : _name)
			folded += char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);

		for(const Model candidate : {Model::G2, Model::G2X, Model::Rack, Model::Engine})
		{
			if(folded == modelName(candidate))
			{
				_model = candidate;
				return true;
			}
		}

		return false;
	}
}
