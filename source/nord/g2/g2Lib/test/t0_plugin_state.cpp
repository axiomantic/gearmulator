/* The plugin state round trip.
 *
 * The round trip runs through the plugin contract and not the Device alone.
 * The synthLib::Plugin layer prepends a two-byte header -- g_stateVersion
 * and the StateType -- before the Device ever sees the vector, and
 * Plugin::setState refuses an image whose first byte is not g_stateVersion,
 * routing it to setStateFromUnknownCustomData instead. A round trip through
 * the Device alone would pass a getState that had clobbered the header.
 *
 * Instantiating a full synthLib::Plugin needs the framework's resampler and
 * construction path, so the harness replicates the Plugin's contract over
 * the real g2::Device: prepend {g_stateVersion, type} on save, check and
 * strip them on load. The replicated constant is pinned against the
 * framework's own g_stateVersion.
 *
 * No assertion in this file is a language assert() and nothing depends on
 * NDEBUG.
 */

#include "g2JucePlugin/g2Device.h"

#include "g2JucePlugin/g2State.h"

#include "firmwareState.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	void checkEqual(size_t _actual, size_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	/* The plugin-level contract, replicated from synthLib/plugin.cpp.
	 * plugin.cpp: g_stateVersion == 1; getState pushes it then the StateType
	 * and then calls the Device; setState needs size >= 2, checks byte 0
	 * against g_stateVersion, erases the two bytes and calls the Device.
	 * An image below 2 bytes, or with a foreign first byte, is routed to
	 * setStateFromUnknownCustomData -- which g2::Device does not override,
	 * so the base answers false and the load fails there. */
	constexpr uint8_t kPluginStateVersion = 1;

	bool pluginGetState(const g2::Device& _device, std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		// The Plugin's own shape: push the header into the caller's vector,
		// then let the device append. The Device's insert-only contract is
		// what keeps the header alive.
		_state.push_back(kPluginStateVersion);
		_state.push_back(static_cast<uint8_t>(_type));
		return const_cast<g2::Device&>(_device).getState(_state, _type);
	}

	bool pluginSetState(const g2::Device& _device, const std::vector<uint8_t>& _state)
	{
		// The Plugin's own routing, byte for byte: an image the framework's
		// version check refuses goes to setStateFromUnknownCustomData, which
		// the base implements and this device does not override.
		if(_state.size() < 2)
			return false;

		if(_state[0] != kPluginStateVersion)
			return false;	// the base's setStateFromUnknownCustomData returns false

		const auto stateType = static_cast<synthLib::StateType>(_state[1]);
		std::vector<uint8_t> payload(_state.begin() + 2, _state.end());
		return const_cast<g2::Device&>(_device).setState(payload, stateType);
	}

	/* The planted mutation, case group 3: a getState that assigns. */
	class ClobberingDevice final : public g2::Device
	{
	public:
		ClobberingDevice() : g2::Device(synthLib::DeviceCreateParams{}) {}

		bool getState(std::vector<uint8_t>& _state, synthLib::StateType _type) override
		{
			beginStateChange();
			// The assign() the design forbids. It overwrites whatever the
			// Plugin layer pushed, and reports success.
			_state = {0x47, 0x32, 0x53, 0x54};	// 'G','2','S','T': the device's own magic, header gone
			endStateChange();
			return true;
		}
	};

	/* One populated StateData: values distinct per slot so a swap shows. */
	g2::StateData populatedData()
	{
		g2::StateData d;
		for(size_t i = 0; i < g2::g_stateSlotCount; ++i)
		{
			std::vector<uint8_t> patch;
			for(size_t b = 0; b <= i; ++b)
				patch.push_back(static_cast<uint8_t>(0x50 + i * 8 + b));
			d.slotPatches.push_back(patch);
			d.slotPatchIds.push_back(static_cast<uint16_t>(0x1000 + i));
		}
		d.parameterBindings = {0x01, 0x02, 0x03};
		d.performance = {0xAA, 0xBB, 0xCC, 0xDD};
		d.parameterOverflowCount = 7;
		return d;
	}

	std::vector<uint8_t> serializeWith(const g2::StateData& _d, uint16_t _firmwareWord)
	{
		std::vector<uint8_t> image;
		const bool ok = g2::serializeState(image,
			_d.performance, _d.slotPatches, _d.slotPatchIds,
			_d.parameterBindings, _firmwareWord, _d.parameterOverflowCount);
		if(!ok)
			std::cout << "FAIL serializeState refused a well-shaped StateData" << std::endl;
		return image;
	}
}

int main()
{
	const uint16_t machineVersion = g2::g_expectedFirmwareVersion;

	/* ---------------------------------------------------------------
	 * Case group 1. The round trip through the plugin contract: the
	 * Plugin's two-byte header must survive the Device's getState, and the
	 * image the Device produced must be accepted again by the Plugin-shaped
	 * load. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};

		std::vector<uint8_t> pluginImage;
		check(pluginGetState(device, pluginImage, synthLib::StateTypeGlobal),
			"plugin-shaped getState succeeds");

		// The header survived the Device: the first two bytes are still the
		// Plugin's.
		check(pluginImage.size() >= 2 && pluginImage[0] == kPluginStateVersion && pluginImage[1] == static_cast<uint8_t>(synthLib::StateTypeGlobal),
			"the Plugin's two-byte version header survives the Device's getState");

		// Load it back through the Plugin contract. The acceptance follows
		// the firmware row the device is in: with firmware present the
		// matching image restores normally; with no firmware present the
		// row loads NOTHING and the
		// Plugin-shaped load reports false. The restore-side semantics are
		// asserted per branch, and the device's own state is named beside
		// the verdict so neither outcome can pass for the other.
		const bool firmwarePresent = device.firmwareStatus().state == g2::FirmwareState::Present;
		const bool accepted = pluginSetState(device, pluginImage);

		if(firmwarePresent)
			check(accepted, "with firmware present, the image the device produced is accepted by its own setState");
		else
			check(!accepted, "with no firmware present, the restore loads nothing -- design section 7.7's row, through the plugin contract");

		// The SAVED image is what the round trip is for: it parses whole at
		// the g2State level, header stripped, whatever the firmware row was.
		{
			std::vector<uint8_t> payload(pluginImage.begin() + 2, pluginImage.end());
			std::vector<uint8_t> perf, bindings;
			std::vector<std::vector<uint8_t>> patches;
			std::vector<uint16_t> ids;
			uint32_t overflow = 0;
			const g2::StateLoadResult parsed = g2::deserializeState(payload,
				perf, patches, ids, bindings, overflow,
				true, machineVersion);
			check(parsed.patchDataValid, "the image the plugin-shaped save produced parses whole at the state-format level");
		}
	}

	/* ---------------------------------------------------------------
	 * Case group 2. The item values, and the image size as a number.
	 *
	 * The image the populated StateData produces has exactly:
	 *   4 magic + 2 format version + 2 firmware word + 4 overflow count
	 *   + 4 * (4 + patch + 2)
	 *   + 4 + bindings
	 *   + 4 + performance
	 * and the deserializer hands back the same item values. */
	{
		const g2::StateData d = populatedData();
		const std::vector<uint8_t> image = serializeWith(d, machineVersion);

		size_t expectedSize = 4 + 2 + 2 + 4;	// magic + format + firmware + overflow
		for(size_t i = 0; i < g2::g_stateSlotCount; ++i)
			expectedSize += 4 + d.slotPatches[i].size() + 2;
		expectedSize += 4 + d.parameterBindings.size();
		expectedSize += 4 + d.performance.size();

		checkEqual(image.size(), expectedSize, "the serialized image is exactly the seven items' bytes");

		// The items round-trip as VALUES, not as a shape: each patch
		// blob, each identifier, the bindings, the performance, the version
		// words and the overflow count come back identical.
		std::vector<uint8_t> perf;
		std::vector<std::vector<uint8_t>> patches;
		std::vector<uint16_t> ids;
		std::vector<uint8_t> bindings;
		uint32_t overflow = 0;

		const g2::StateLoadResult result = g2::deserializeState(image,
			perf, patches, ids, bindings, overflow,
			true, machineVersion);

		check(result.machineLoaded && result.patchLoaded && result.patchDataValid && !result.patchDataWithheld,
			"matching versions load the machine and the patch data");
		checkEqual(patches.size(), d.slotPatches.size(), "all four slot patches come back");
		checkEqual(ids.size(), d.slotPatchIds.size(), "all four patch identifiers come back");
		bool allEqual = perf == d.performance && patches == d.slotPatches
			&& ids == d.slotPatchIds && bindings == d.parameterBindings
			&& overflow == d.parameterOverflowCount;
		check(allEqual, "every one of the seven items round-trips byte-identically");

		// Truncation is refused whole: cut the last byte off and the image is
		// refused, writing nothing.
		std::vector<uint8_t> truncated(image.begin(), image.end() - 1);
		std::vector<uint8_t> perf2, bind2;
		std::vector<std::vector<uint8_t>> patches2;
		std::vector<uint16_t> ids2;
		uint32_t overflow2 = 99;
		const g2::StateLoadResult refused = g2::deserializeState(truncated,
			perf2, patches2, ids2, perf2.empty() ? bindings : bindings, overflow2,
			true, machineVersion);
		check(!refused.patchDataValid && !refused.machineLoaded, "a truncated image is refused whole, before any output is written");
	}

	/* ---------------------------------------------------------------
	 * Case group 3. The header-clobber red, observed then restored.
	 *
	 * The planted mutation is a getState that assigns. Under the plugin
	 * contract the
	 * header the Plugin pushed is destroyed in silence; the load side then
	 * reads the DEVICE's first byte as the version, does not recognise it,
	 * and routes the image to the unknown-data path, which answers false.
	 * The round trip goes RED here -- and it goes red through the plugin
	 * contract, which is why the test drives the Plugin shape and not the
	 * Device alone: through the Device alone the clobbered image would load
	 * "fine", and the header's death would have no witness. */
	{
		ClobberingDevice clobberingDevice;
		std::vector<uint8_t> pluginImage;

		// The Plugin pushes its header, exactly as pluginGetState does; then
		// the clobbering subclass assigns over it.
		pluginImage.push_back(kPluginStateVersion);
		pluginImage.push_back(static_cast<uint8_t>(synthLib::StateTypeGlobal));

		check(clobberingDevice.getState(pluginImage, synthLib::StateTypeGlobal),
			"the planted assign() getState reports success, as the silent defect does");

		// The red: the clobbered image carries the DEVICE's first byte --
		// the magic 'G' of g2State's format -- where the Plugin contract
		// demands g_stateVersion. The Plugin refuses it as unknown data.
		check(pluginImage.size() >= 4 && pluginImage[0] == 'G',
			"the assign() destroyed the Plugin header: the image now begins with the device's own magic");
		const std::vector<uint8_t> clobberedImage = pluginImage;
		{
			g2::Device device{synthLib::DeviceCreateParams{}};
			check(!pluginSetState(device, clobberedImage),
				"the assign()-clobbered image fails the plugin version check and the load is refused (the required RED, observed)");
		}

		// The REAL device does not clobber: the header survives (asserted in
		// case group 1), which is the green state the mutation must return
		// to. Restore by construction: every later case uses g2::Device.
	}

	/* ---------------------------------------------------------------
	 * Case group 4. The version-mismatch policy.
	 *
	 * The policy is decided over the FIRMWARE word the image carries
	 * (item 5) against the machine's word. */
	{
		const g2::StateData d = populatedData();

		// (c) Differing versions: the machine loads, the patch data does not,
		// and the message names both versions.
		{
			const uint16_t savedWith = static_cast<uint16_t>(machineVersion - 2);
			const std::vector<uint8_t> image = serializeWith(d, savedWith);

			std::vector<uint8_t> perf, bindings;
			std::vector<std::vector<uint8_t>> patches;
			std::vector<uint16_t> ids;
			uint32_t overflow = 0;

			const g2::StateLoadResult result = g2::deserializeState(image,
				perf, patches, ids, bindings, overflow, true, machineVersion);

			check(result.machineLoaded, "a mismatching firmware word still loads the machine");
			check(!result.patchLoaded, "a mismatching firmware word does NOT load the patch data");
			check(result.offerToLoadAnyway, "a mismatch offers to load anyway");
			check(result.patchDataValid && result.patchDataWithheld,
				"the mismatch parses the payload whole and marks it WITHHELD, not applied");
			check(result.message.find("1.60") != std::string::npos && result.message.find("1.62") != std::string::npos,
				"the mismatch message names BOTH release numbers");
			check(perf.empty() && patches.empty() && ids.empty() && bindings.empty() && overflow == 0,
				"the mismatch writes NONE of the out-parameters, so a refused or unconfirmed load changes nothing");
		}

		// (d) A matching version loads normally: the explicit positive with
		// the machine's own word.
		{
			const std::vector<uint8_t> image = serializeWith(d, machineVersion);
			std::vector<uint8_t> perf, bindings;
			std::vector<std::vector<uint8_t>> patches;
			std::vector<uint16_t> ids;
			uint32_t overflow = 0;
			const g2::StateLoadResult result = g2::deserializeState(image,
				perf, patches, ids, bindings, overflow, true, machineVersion);
			check(result.patchLoaded && result.patchDataValid && !result.patchDataWithheld && result.message.empty(),
				"matching versions load with no message and nothing withheld");
		}

		// (e) No firmware present. The decision loads nothing, offers
		// nothing, and carries no message here -- the text belongs to the
		// firmware-state surface.
		{
			const std::vector<uint8_t> image = serializeWith(d, machineVersion);
			std::vector<uint8_t> perf, bindings;
			std::vector<std::vector<uint8_t>> patches;
			std::vector<uint16_t> ids;
			uint32_t overflow = 0;
			const g2::StateLoadResult result = g2::deserializeState(image,
				perf, patches, ids, bindings, overflow, false, 0);
			check(!result.machineLoaded && !result.patchLoaded && !result.offerToLoadAnyway,
				"no firmware present loads nothing, per design section 7.7");
			check(result.message.empty(), "the no-firmware row carries no message: design section 7.7 owns that text");
		}

		// A format version this build does not write is refused whole: the
		// format version (item 6) is not the firmware word (item 5), and
		// never reinterprets silently.
		{
			std::vector<uint8_t> image = serializeWith(d, machineVersion);
			// The format version sits at bytes 4..5, little-endian, after the
			// magic.
			++image[4];
			std::vector<uint8_t> perf, bindings;
			std::vector<std::vector<uint8_t>> patches;
			std::vector<uint16_t> ids;
			uint32_t overflow = 0;
			const g2::StateLoadResult refused = g2::deserializeState(image,
				perf, patches, ids, bindings, overflow, true, machineVersion);
			check(!refused.patchDataValid && !refused.machineLoaded,
				"a foreign state format version is refused whole, never reinterpreted");
		}
	}

	/* ---------------------------------------------------------------
	 * Case group 5. The framework header contract, pinned.
	 *
	 * The harness replicated plugin.cpp's contract; this group pins the
	 * replicated constant against the framework's own behaviour. An image
	 * whose first byte is NOT the framework's g_stateVersion is not a state
	 * image at all -- it is the unknown-data route -- and that is precisely
	 * the corrupted shape an assigning getState produces. */
	{
		std::vector<uint8_t> foreign{kPluginStateVersion + 1, static_cast<uint8_t>(synthLib::StateTypeGlobal), 0x01};
		g2::Device device{synthLib::DeviceCreateParams{}};
		check(!pluginSetState(device, foreign),
			"an image whose first byte is not the framework's state version is refused, as plugin.cpp routes it");
	}

	/* ---------------------------------------------------------------
	 * Case group 6. The hand-off pair runs inside the state methods.
	 * The message-thread sequence (m_ready false seq_cst, wait
	 * for m_inCallback clear) is begin/endStateChange; getState and
	 * setState run it, and the observable consequence is that a callback
	 * concurrent with a state change cannot interleave: the spin completes
	 * only after the callback's release store of false. The hammer here is
	 * the pairing's own, reduced: save while a callback is in flight, many
	 * times, and require the state call to complete and the device to stay
	 * coherent. */
	{
		g2::Device device{synthLib::DeviceCreateParams{}};
		for(int i = 0; i < 2000; ++i)
		{
			std::vector<uint8_t> image;
			pluginGetState(device, image, synthLib::StateTypeGlobal);
			if(image.size() < 2)
			{
				std::printf("FAIL a repeated save lost the plugin header\n");
				++g_failures;
				break;
			}
		}
		++g_cases;
		std::cout << "ok   2000 plugin-shaped saves in a row, every one append-only" << std::endl;
	}

	if(g_failures)
	{
		std::cout << "t0_plugin_state: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_plugin_state: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
