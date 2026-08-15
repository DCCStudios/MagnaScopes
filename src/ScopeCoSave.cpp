#include "ScopeCoSave.h"

#include "ScopeResolver.h"

namespace MagnaScope
{
	namespace
	{
		constexpr std::uint32_t kUniqueID = 'MGSC';
		constexpr std::uint32_t kSessionRecord = 'SSTA';
		constexpr std::uint32_t kSessionVersion = 1;
		// A hand-edited or corrupt record must not make the plugin allocate
		// wildly. Nobody has 100k distinct scope attachments.
		constexpr std::uint32_t kMaxEntries = 100000U;
		constexpr std::uint32_t kMaxStringLength = 4096U;

		bool WriteString(
			const F4SE::SerializationInterface* intfc,
			const std::string& value)
		{
			const auto length = static_cast<std::uint32_t>(value.size());
			if (!intfc->WriteRecordData(length)) {
				return false;
			}
			return length == 0U ||
			       intfc->WriteRecordData(value.data(), length);
		}

		bool ReadString(
			const F4SE::SerializationInterface* intfc,
			std::string& value)
		{
			std::uint32_t length = 0U;
			if (intfc->ReadRecordData(length) != sizeof(length)) {
				return false;
			}
			if (length > kMaxStringLength) {
				return false;
			}
			value.assign(length, '\0');
			if (length == 0U) {
				return true;
			}
			return intfc->ReadRecordData(value.data(), length) == length;
		}

		void F4SEAPI OnSave(const F4SE::SerializationInterface* intfc)
		{
			if (!intfc->OpenRecord(kSessionRecord, kSessionVersion)) {
				logger::error("Could not open the MagnaScope co-save record");
				return;
			}

			const auto& states = SessionStates();
			// Only states the player actually changed are worth persisting.
			// Writing untouched defaults would pin every scope the character
			// has ever raised to whatever the profile said at that moment, so a
			// later change to the profile's authored default would never reach
			// them.
			std::uint32_t count = 0U;
			for (const auto& [key, state] : states) {
				if (state.userTouched) {
					++count;
				}
			}

			if (!intfc->WriteRecordData(count)) {
				return;
			}

			for (const auto& [key, state] : states) {
				if (!state.userTouched) {
					continue;
				}
				const auto& [plugin, formID, omodKey] = key;
				if (!WriteString(intfc, plugin) ||
					!intfc->WriteRecordData(formID) ||
					!WriteString(intfc, omodKey) ||
					!intfc->WriteRecordData(state.variantId) ||
					!intfc->WriteRecordData(state.secondaryIndex) ||
					!intfc->WriteRecordData(state.reticleIndex)) {
					logger::error("MagnaScope co-save write failed partway");
					return;
				}
			}

			logger::info("Wrote {} MagnaScope scope selections to the co-save", count);
		}

		void F4SEAPI OnLoad(const F4SE::SerializationInterface* intfc)
		{
			std::uint32_t type = 0U;
			std::uint32_t version = 0U;
			std::uint32_t length = 0U;

			while (intfc->GetNextRecordInfo(type, version, length)) {
				if (type != kSessionRecord) {
					// Unknown record types are skipped rather than treated as an
					// error, so a future version's extra records cannot break an
					// older build.
					continue;
				}
				if (version != kSessionVersion) {
					logger::warn(
						"Skipping MagnaScope co-save record version {}; this "
						"build reads version {}",
						version,
						kSessionVersion);
					continue;
				}

				std::uint32_t count = 0U;
				if (intfc->ReadRecordData(count) != sizeof(count) ||
					count > kMaxEntries) {
					logger::error("MagnaScope co-save record is malformed");
					return;
				}

				auto& states = SessionStates();
				std::uint32_t restored = 0U;
				for (std::uint32_t index = 0U; index < count; ++index) {
					std::string plugin;
					std::uint32_t formID = 0U;
					std::string omodKey;
					SightSessionState state;

					if (!ReadString(intfc, plugin) ||
						intfc->ReadRecordData(formID) != sizeof(formID) ||
						!ReadString(intfc, omodKey) ||
						intfc->ReadRecordData(state.variantId) !=
							sizeof(state.variantId) ||
						intfc->ReadRecordData(state.secondaryIndex) !=
							sizeof(state.secondaryIndex) ||
						intfc->ReadRecordData(state.reticleIndex) !=
							sizeof(state.reticleIndex)) {
						logger::error(
							"MagnaScope co-save record truncated after {} entries",
							restored);
						return;
					}

					// The variant is restored by id, not by list position: the
					// author may have inserted a variant since this save, and an
					// index would then select the wrong optic silently. Resolving
					// the id to a position happens on selection, where the
					// profile is available.
					state.userTouched = true;
					state.variantNeedsResolve = true;
					states.insert_or_assign(
						SightSessionKey{ plugin, formID, omodKey },
						state);
					++restored;
				}

				logger::info(
					"Restored {} MagnaScope scope selections from the co-save",
					restored);
			}
		}

		void F4SEAPI OnRevert(const F4SE::SerializationInterface*)
		{
			SessionStates().clear();
			logger::info("Cleared MagnaScope scope selections");
		}
	}

	bool RegisterCoSave()
	{
		const auto* serialization = F4SE::GetSerializationInterface();
		if (!serialization) {
			logger::error(
				"Serialization interface unavailable; scope selections will not "
				"persist across saves");
			return false;
		}

		serialization->SetUniqueID(kUniqueID);
		serialization->SetSaveCallback(OnSave);
		serialization->SetLoadCallback(OnLoad);
		serialization->SetRevertCallback(OnRevert);
		return true;
	}
}
